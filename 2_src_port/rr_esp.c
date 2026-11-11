/* rr_esp.c - ROB RUN overlay entry point and application lifecycle.
 *
 * Owns the window, hotkeys, configuration, worker threads and diagnostic
 * command dispatch. rr_game.c supplies snapshots, rr_draw.c renders them,
 * and menu_webview.cpp hosts the embedded HTML settings page. The transport
 * in rr_mem.c sends game-memory requests to the SmxRing driver.
 *
 * Offline modes: --menu, --demo and --selftest.
 * Other diagnostics may attach to a process, use the driver or modify state;
 * read their implementations before running them.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <tlhelp32.h>      /* Thread enumeration for diagnostic requests. */
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

#include "menu_bridge.h"
#include "gpu.h"
#include "rr_mem.h"
#include "rr_spawn.h"      /* Process-hide helper; success depends on the mapped driver. */

/* A focus-return event requests a rescan because scene pointers may have
 * changed while the game was inactive. The reader consumes the flag.
 */
static volatile LONG g_rescan_now = 0;
#include "rr_unity.h"
#include "rr_chain.h"
#include "rr_game.h"
#include "rr_draw.h"
#include "rr_aim.h"
#include "rr_los.h"
#include "rr_find.h"
#include "robrun_offsets.h"

#pragma comment(lib, "winmm.lib")

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define GAME_EXE      "ROB RUN.exe"

static HWND  g_hwnd   = NULL;
static int   g_sw = 0, g_sh = 0;
static volatile LONG g_quit = 0;

/* Snapshot. */

static CRITICAL_SECTION g_cs;
static RR_PLAYER g_buf[2][RR_MAX_PLAYERS];
static RR_CAMERA g_cam[2];
static int       g_nbuf[2];
static int       g_front = 0;
static unsigned  g_snapshot_seq;
static DWORD     g_snapshot_at;
static volatile LONG g_repaint_pending;
static volatile LONG g_present_retry;
static int       g_nrender = 0;
static RR_PLAYER g_render[RR_MAX_PLAYERS];
static RR_CAMERA g_rcam;

static volatile LONG g_game_ok = 0;
static int g_show_footer = 1;   /* status line: useful while probing, hide once it works */

/* Hotkeys can arrive through WM_HOTKEY, Raw Input, a low-level hook or
 * GetAsyncKeyState polling. Each source is logged to distinguish missing
 * input from a failed action. Held-key state and a debounce interval combine
 * duplicate deliveries into one action.
 */
static int g_hk_menu = 0, g_hk_esp = 0, g_hk_quit = 0;
static HHOOK g_kb_hook = NULL;
static DWORD g_hk_last[4] = { 0, 0, 0, 0 };
/* 1 while the physical key is held. Set when a press is acted on, cleared by
 * the release. This is what makes a held key one action instead of a stream. */
static volatile LONG g_hk_down[4] = { 0, 0, 0, 0 };
static long g_cashfind_bad;

static void hklogf(const char* fmt, ...) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0; else path[0] = 0;
    strncat(path, "hotkeys.log", MAX_PATH - strlen(path) - 1);
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%lu ", (unsigned long)GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}


static void hk_action(int id) {
    BR_Config* b = br_get();
    if (id == 1) {
        br_request_toggle();
        hklogf("F1 menu=%s", b->menu_open ? "open" : "closed");
        if (b->menu_open) g_show_footer = 1;
    } else if (id == 2) {
        b->enabled = !b->enabled;
        hklogf("INSERT esp=%s", b->enabled ? "on" : "off");
    } else if (id == 3) {
        hklogf("END quit");
        InterlockedExchange(&g_quit, 1);
        if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    }
}

static int vk_to_id(int vk) {
    if (vk == VK_F1) return 1;
    if (vk == VK_INSERT) return 2;
    if (vk == VK_END) return 3;
    return 0;
}

static void hk_release(int id) {
    if (id < 1 || id > 3) return;
    InterlockedExchange(&g_hk_down[id], 0);
}

static void hk_deliver(int id, const char* src) {
    DWORD now, prev, age;
    int was_down;
    if (id < 1 || id > 3) return;
    now = GetTickCount();

    /* Ignore auto-repeat while the key is down. Release clears the state;
     * a timeout recovers if the release event was lost.
     */
    was_down = (int)InterlockedCompareExchange(&g_hk_down[id], 1, 0);
    prev = (DWORD)InterlockedExchange((volatile LONG*)&g_hk_last[id], (LONG)now);
    age = now - prev;

    if (was_down && age < 2000) {
        hklogf("key id=%d src=%s held, ignored", id, src);
        return;
    }
    /* Use an atomic exchange because input sources can run on different
     * threads. Only one caller should act on a duplicate delivery.
     */
    if (age < 300) {
        hklogf("key id=%d src=%s debounced (age=%lu)", id, src, (unsigned long)age);
        return;
    }
    hklogf("key id=%d src=%s act (age=%lu)", id, src, (unsigned long)age);
    hk_action(id);
}

/* Low-level hook: catches the key even when another process (the game) owns
 * the foreground and even when RegisterHotKey was refused. */

static LRESULT CALLBACK kb_hook_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN) {
            int id = vk_to_id((int)k->vkCode);
            if (id) PostMessageA(g_hwnd, WM_APP + 2, (WPARAM)id, 0);
        } else if (w == WM_KEYUP || w == WM_SYSKEYUP) {
            /* The release is what separates a real second press from the
             * auto-repeat stream a held key produces. Without it, holding
             * INSERT flips the ESP on and off for as long as it is held. */
            int id = vk_to_id((int)k->vkCode);
            if (id) PostMessageA(g_hwnd, WM_APP + 3, (WPARAM)id, 0);
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

static int game_focused(void) {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return g_rr_pid && pid == g_rr_pid;
}

typedef struct { DWORD pid; HWND window; long area; } GAME_WINDOW_SEARCH;
static BOOL CALLBACK find_game_window(HWND window, LPARAM param) {
    GAME_WINDOW_SEARCH* s = (GAME_WINDOW_SEARCH*)param;
    DWORD pid = 0; RECT rc;
    GetWindowThreadProcessId(window, &pid);
    if (pid != s->pid || !IsWindowVisible(window) || IsIconic(window) ||
        GetWindow(window, GW_OWNER) || !GetClientRect(window, &rc)) return TRUE;
    long width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width > 100 && height > 100 && width * height > s->area) {
        s->area = width * height; s->window = window;
    }
    return TRUE;
}

static void sync_game_window(void) {
    if (!g_rr_pid || !g_hwnd) return;
    GAME_WINDOW_SEARCH s = { g_rr_pid, NULL, 0 };
    EnumWindows(find_game_window, (LPARAM)&s);
    if (!s.window) return;
    RECT client, overlay; POINT origin = {0, 0};
    if (!GetClientRect(s.window, &client) || !ClientToScreen(s.window, &origin) ||
        !GetWindowRect(g_hwnd, &overlay)) return;
    int width = client.right - client.left, height = client.bottom - client.top;
    if (overlay.left != origin.x || overlay.top != origin.y ||
        overlay.right - overlay.left != width || overlay.bottom - overlay.top != height) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* Raw Input: device-level delivery, independent of focus and of the async key
 * table (which some anti-cheats filter for other processes). */
static int raw_input_on(void) {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;       /* generic desktop */
    rid.usUsage = 0x06;           /* keyboard        */
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = g_hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) ? 1 : 0;
}
static char g_exe[64] = GAME_EXE;  /* overridable with --exe for driver tests */
static int  g_demo = 0;            /* --demo: synthetic players, no driver/game */
static char g_status[192] = "starting";

/* Cfg. */

typedef struct {
    const char* key;
    size_t offset;
    int minimum, maximum;
} CFG_INT;

#define CFG_FIELD(field, initial, minimum, maximum) { #field, offsetof(BR_Config, field), minimum, maximum },
static const CFG_INT g_cfg_ints[] = { BR_SETTINGS(CFG_FIELD) };
#undef CFG_FIELD

int br_config_set(BR_Config* b, const char* key, int value) {
    for (size_t i = 0; i < sizeof(g_cfg_ints) / sizeof(g_cfg_ints[0]); ++i) {
        const CFG_INT* field = &g_cfg_ints[i];
        if (strcmp(key, field->key) != 0) continue;
        if (value < field->minimum) value = field->minimum;
        if (value > field->maximum) value = field->maximum;
        *(int*)((char*)b + field->offset) = value;
        return 1;
    }
    return 0;
}

static int cfg_path(wchar_t* path, size_t capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)capacity);
    if (!length || length >= capacity) return 0;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    size_t prefix = (size_t)(slash + 1 - path);
    return wcscpy_s(path + prefix, capacity - prefix, L"CoreRR.cfg") == 0;
}

static int cfg_save_at(const wchar_t* path, const BR_Config* b) {
    wchar_t temp[MAX_PATH + 20];
    if (swprintf_s(temp, _countof(temp), L"%ls.%lu.tmp", path, GetCurrentProcessId()) < 0) return 0;
    FILE* f = _wfopen(temp, L"w");
    if (!f) return 0;
    fprintf(f, "# CoreRR settings\n");
    for (size_t i = 0; i < sizeof(g_cfg_ints) / sizeof(g_cfg_ints[0]); ++i) {
        const CFG_INT* field = &g_cfg_ints[i];
        int value = *(const int*)((const char*)b + field->offset);
        fprintf(f, "%s=%d\n", field->key, value);
    }
    for (int i = 0; i < BR_COLOR_COUNT; ++i)
        fprintf(f, "color%d=%u,%u,%u,0\n", i, (unsigned)b->color[i].rgb[0],
                (unsigned)b->color[i].rgb[1], (unsigned)b->color[i].rgb[2]);
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(temp);
    return 0;
}

static int cfg_load_at(const wchar_t* path, BR_Config* b) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return 0;
    BR_Config loaded = *b;
    int legacy_visible = -1, legacy_head = -1;
    int have_los = 0, have_bone = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char* value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;
        if (!strncmp(line, "color", 5)) {
            char* end;
            long index = strtol(line + 5, &end, 10);
            int red, green, blue;
            if (end != line + 5 && !*end && index >= 0 && index < BR_COLOR_COUNT &&
                sscanf(value, "%d,%d,%d", &red, &green, &blue) == 3 &&
                (unsigned)red <= 255 && (unsigned)green <= 255 && (unsigned)blue <= 255) {
                loaded.color[index].rgb[0] = (unsigned char)red;
                loaded.color[index].rgb[1] = (unsigned char)green;
                loaded.color[index].rgb[2] = (unsigned char)blue;
            }
            continue;
        }
        char* end;
        long number = strtol(value, &end, 10);
        if (end == value || *end) continue;
        if (!strcmp(line, "aa_visible") && (number == 0 || number == 1))
            legacy_visible = (int)number;
        else if (!strcmp(line, "aa_head") && (number == 0 || number == 1))
            legacy_head = (int)number;
        else if (br_config_set(&loaded, line, (int)number)) {
            if (!strcmp(line, "aa_los_on")) have_los = 1;
            if (!strcmp(line, "aa_bone")) have_bone = 1;
        }
    }
    int ok = !ferror(f);
    fclose(f);
    if (ok) {
        if (!have_los && legacy_visible >= 0) loaded.aa_los_on = legacy_visible;
        if (!have_bone && legacy_head >= 0) loaded.aa_bone = legacy_head ? 0 : 1;
#define BR_APPLY(field, initial, minimum, maximum) b->field = loaded.field;
        BR_SETTINGS(BR_APPLY)
#undef BR_APPLY
        memcpy(b->color, loaded.color, sizeof(b->color));
    }
    return ok;
}

int br_config_save(void) {
    wchar_t path[MAX_PATH];
    return cfg_path(path, MAX_PATH) && cfg_save_at(path, br_get());
}

int br_config_load(void) {
    wchar_t path[MAX_PATH];
    return cfg_path(path, MAX_PATH) && cfg_load_at(path, br_get());
}

/* Aim assist. */

static void aa_move_mouse(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

static int aa_clear_path(RR_V3 start, RR_V3 end) {
    return rr_los_check(start,end)==RR_LOS_CLEAR;
}

static void aa_tick(unsigned snapshot_seq, DWORD snapshot_at) {
    static unsigned last_snapshot;
    static DWORD last_update;
    static uint64_t locked_addr;
    static int32_t locked_id;
    static int last_group;
    static RR_V3 last_position;
    static DWORD last_position_at;
    static RR_AIM_STEER steer;
    static DWORD last_diag;
    BR_Config* b = br_get();
    DWORD now = timeGetTime();
    int held = b->aa_on && (GetAsyncKeyState(b->aa_key) & 0x8000);
    if (!held) locked_addr = 0;

    /* Log aim-worker gating decisions twice per second while enabled: hold
     * key, foreground window, snapshot age and target selection.
     */
    if (b->aa_on && now - last_diag > 500) {
        last_diag = now;
        hklogf("aim: held=%d key=0x%02X menu=%d focused=%d cam=%d n=%d age=%lu keydown=%d",
               held, b->aa_key, b->menu_open, game_focused(),
               g_rcam.valid, g_nrender, (unsigned long)(now - snapshot_at),
               (GetAsyncKeyState(b->aa_key) & 0x8000) ? 1 : 0);
    }

    if (!held || b->menu_open ||
        !game_focused() || !g_rcam.valid || !g_nrender || now - snapshot_at > 50) {
        steer = (RR_AIM_STEER){0};
        last_position_at = 0;
        return;
    }
    /* Let the game apply the previous input before issuing a new correction. */
    DWORD elapsed = now - last_update;
    if (snapshot_seq == last_snapshot || elapsed < 15) return;
    last_snapshot = snapshot_seq;
    last_update = now;
    int group = b->aa_bone;
    RECT rc;
    int vw = g_sw, vh = g_sh;
    if (g_hwnd && GetClientRect(g_hwnd, &rc)) { vw = rc.right; vh = rc.bottom; }
    float cx = vw * 0.5f, cy = vh * 0.5f;
    RR_AIM_PICK pick = rr_aim_pick(g_render, g_nrender, &g_rcam, vw, vh, b,
                                  locked_addr, locked_id, b->aa_los_on ? aa_clear_path : NULL);
    if (!pick.player) {
        steer = (RR_AIM_STEER){0};
        last_position_at = 0;
        return;
    }
    const RR_PLAYER* p = pick.player;
    if (!locked_addr || last_group != group) {
        steer = (RR_AIM_STEER){0};
    } else {
        RR_V3 predicted;
        float sx, sy;
        if (last_position_at && !p->inveh && rr_aim_motion_point(pick.point, p->pos, last_position,
                                            snapshot_at - last_position_at, now - snapshot_at,
                                            b->aa_smooth, &predicted) &&
            game_w2s(&g_rcam, &predicted, vw, vh, &sx, &sy)) {
            float dx = sx - cx, dy = sy - cy;
            if (isfinite(dx) && isfinite(dy) &&
                (!b->aa_los_on || aa_clear_path(g_rcam.pos, predicted))) {
                pick.dx = dx;
                pick.dy = dy;
            }
        }
    }
    locked_addr = p->addr;
    locked_id = p->id;
    last_group = group;
    last_position = p->pos;
    last_position_at = snapshot_at;
    int mx, my;
    rr_aim_step(&steer, pick.dx, pick.dy, g_rcam.fov, vh, b->aa_smooth, b->aa_aim_cal,
                elapsed, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, &mx, &my);
    if (mx || my)
        hklogf("aim: move mx=%d my=%d dist=%.1f err=(%.1f,%.1f)", mx, my, p->dist,
               pick.dx, pick.dy);
    aa_move_mouse(mx, my);
}

/* Threads. */

/* Demo. */

/* Deterministic synthetic players: six targets spread in front of a camera at
 * eye height looking down +Z. Positions rotate slowly so the frame is alive
 * and any projection mistake is obvious (boxes would slide together). */
static int demo_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam) {
    static const char* names[] = { "PLAYER ONE", "PLAYER TWO", "PLAYER THREE",
                                   "PLAYER FOUR", "PLAYER FIVE", "PLAYER SIX" };
    if (!out || max <= 0) return 0;

    memset(cam, 0, sizeof(*cam));
    cam->pos.x = 0.0f; cam->pos.y = 1.7f; cam->pos.z = 0.0f;
    cam->fwd.x = 0.0f; cam->fwd.y = -0.02f; cam->fwd.z = 1.0f;
    cam->rot.w = 1.0f;
    cam->fov = 70.0f;
    cam->valid = 1;

    static DWORD t0 = 0;
    if (!t0) t0 = GetTickCount();
    float t = (float)(GetTickCount() - t0) * 0.0004f;

    int n = max < 7 ? max : 7;
    for (int i = 0; i < n; i++) {
        RR_PLAYER* p = &out[i];
        memset(p, 0, sizeof(*p));
        float a = t + (float)i * 1.0472f;
        float dist = 9.0f + (float)i * 6.0f;
        p->addr = 0x1000 + (uint64_t)i;
        p->id = 100 + i;
        p->pos.x = sinf(a) * dist * 0.55f;
        p->pos.y = 0.0f;
        p->pos.z = cosf(a) * dist + 6.0f;
        p->head = p->pos;
        p->head.y += 1.8f;
        float dx = p->pos.x - cam->pos.x, dy = p->pos.y - cam->pos.y, dz = p->pos.z - cam->pos.z;
        p->dist = sqrtf(dx * dx + dy * dy + dz * dz);
        p->dead = (i == 4);
        if (i == 6) { p->local = RR_LOCAL_CONFIRMED; p->pos = (RR_V3){0}; }
        if (i == 1 || i == 3) p->relationships |= RR_REL_CLAN;
        if (i == 2 || i == 3) p->relationships |= RR_REL_POLICE;
        p->valid = 1;
        snprintf(p->name, sizeof(p->name), "%s", names[i % 6]);
    }
    return n;
}
/* Exit the overlay without requesting driver unload or process relinking.
 * rr_drv_close() closes this client handle only. The mapped driver remains
 * resident; the current hide implementation leaves its detached list entry
 * self-linked. This behavior is not a general guarantee of kernel safety.
 */
static void overlay_exit(int code) {
    br_config_save();
    ExitProcess((UINT)code);
}

static DWORD WINAPI ReadThread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    int tick = 0;
    int dead_ticks = 0;
    int game_gone = 0;
    /* Measure snapshot construction separately from time since publication.
     * g_snapshot_at is stamped after sampling, so a fresh publication can still
     * contain data that took a long time to collect.
     */
    DWORD snap_started = 0, rate_t0 = 0, last_snap_ms = 0;
    int rate_n = 0, last_hz = 0;
    /* The snapshot grew with the skinned mesh; keeping two full player arrays
       on this thread's 1 MiB stack leaves too little room for rig traversal. */
    static RR_PLAYER tmp[RR_MAX_PLAYERS];
    for (;;) {
        /* The game can start after the overlay, or restart mid-session: keep
         * trying to attach so the tool recovers instead of reporting a
         * permanent "game not found". */
        if (!g_demo && g_rr_pid == 0) {
            if (rr_drv_open()) rr_attach(GAME_EXE);
            if (g_rr_pid) { game_init(); }
        }
        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

        /* --demo: the same snapshot the game produces, generated locally.
         * It exercises projection, culling, box/name/distance drawing and the
         * menu bridge without a driver, a game or an anchor. */
        if (g_demo) {
            RR_CAMERA cam;
            int n = demo_snapshot(tmp, RR_MAX_PLAYERS, &cam);
            EnterCriticalSection(&g_cs);
            int back = g_front ^ 1;
            memcpy(g_buf[back], tmp, sizeof(RR_PLAYER) * (size_t)n);
            g_nbuf[back] = n;
            g_cam[back] = cam;
            g_front = back;
            LeaveCriticalSection(&g_cs);
            InterlockedExchange(&g_game_ok, 1);
            Sleep(16);
            continue;
        }
        RR_CAMERA cam;
        memset(&cam, 0, sizeof(cam));
        int n = 0;
        snap_started = timeGetTime();
        __try {
            n = game_snapshot(tmp, RR_MAX_PLAYERS, &cam);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hklogf("exception in game_snapshot");
            n = 0;
        }
        {
            DWORD finished = timeGetTime();
            last_snap_ms = finished - snap_started;
            if (!rate_t0) rate_t0 = finished;
            ++rate_n;
            if (finished - rate_t0 >= 1000) {
                last_hz = (int)((rate_n * 1000u) / (finished - rate_t0));
                rate_n = 0;
                rate_t0 = finished;
            }
        }

        /* Retry discovery when snapshots are empty or the world anchor is missing.
         * A nonzero entry count alone does not establish that discovery succeeded.
         */
        /* Retry after 40 empty/missing-world iterations. Elapsed time depends on
         * the actual reader cadence.
         */
        /* Reattach after a failed initialization or a process restart. */
        if (InterlockedExchange(&g_rescan_now, 0) && !game_discovery_active()) {
            game_init();
            n = game_snapshot(tmp, RR_MAX_PLAYERS, &cam);
        }
        /* Check the image signature for attachment liveness. Three failed checks
         * trigger reattachment while tolerating a brief read failure.
         */
        if ((tick % 20) == 0) {
            uint8_t mz[2];
            uint64_t img = rr_image_base();
            if (img && rr_read(img, mz, sizeof(mz))) {
                dead_ticks = 0;
            } else if (++dead_ticks >= 3) {
                dead_ticks = 0;
                /* Clear first: if the relaunch has not happened yet, the attach
                 * below fails and the loop at the top of this function takes
                 * over, so the stale anchors must already be gone by then. */
                hklogf("target process lost - clearing anchors and re-attaching");
                game_reattach_reset();
                if (rr_attach(g_exe)) {
                    hklogf("re-attached to pid %u", g_rr_pid);
                    game_init();
                    n = game_snapshot(tmp, RR_MAX_PLAYERS, &cam);
                } else {
                    g_rr_pid = 0;   /* the attach loop at the loop head retries */
                }
            }
        }
        /* Close after two successful process snapshots omit the game. Enumeration
         * errors are handled separately so they do not count as confirmed exits.
         */
        if ((tick % 240) == 0 && g_rr_pid) {
            if (rr_process_visible(g_rr_pid) == 0) {
                if (++game_gone >= 2) {
                    hklogf("target pid %u exited - shutting down", (unsigned)g_rr_pid);
                    overlay_exit(0);
                }
            } else {
                game_gone = 0;
            }
        }
        if (!game_discovery_active() && (tick % 40) == 0 &&
            (n == 0 || game_world_addr() == 0)) {
            game_init();
            n = game_snapshot(tmp, RR_MAX_PLAYERS, &cam);
        }

        EnterCriticalSection(&g_cs);
        int back = g_front ^ 1;
        memcpy(g_buf[back], tmp, sizeof(RR_PLAYER) * (size_t)n);
        g_nbuf[back] = n;
        g_cam[back] = cam;
        g_front = back;
        ++g_snapshot_seq;
        g_snapshot_at = timeGetTime();
        LeaveCriticalSection(&g_cs);

        InterlockedExchange(&g_game_ok, n > 0);
        if ((tick % 3000) == 0) {
            char dpath[MAX_PATH];
            GetModuleFileNameA(NULL, dpath, MAX_PATH);
            char* dslash = strrchr(dpath, 0x5C);
            if (dslash) *(dslash + 1) = 0;
            strncat(dpath, "status.log", MAX_PATH - strlen(dpath) - 1);
            FILE* df = fopen(dpath, "a");
            if (df) {
                char stx[192];
                game_status(stx, sizeof(stx));
                fprintf(df, "%lu pid=%lu n=%d inst=%d world=%llX snap=%lums hz=%d status=[%s]\n",
                        (unsigned long)GetTickCount(), (unsigned long)GetCurrentProcessId(), n, game_instance_mode(),
                        (unsigned long long)game_world_addr(), (unsigned long)last_snap_ms, last_hz, stx);
                fclose(df);
            }
        }
        if (n > 0 && !br_get()->menu_open) g_show_footer = 0;   /* it was only a probe aid */
        if ((tick % 250) == 0) {
            char st[192];
            game_status(st, sizeof(st));
            EnterCriticalSection(&g_cs);
            snprintf(g_status, sizeof(g_status), "%s players=%d", st, n);
            LeaveCriticalSection(&g_cs);
            BR_Config* b = br_get();
            b->telem_game_pid = (int)g_rr_pid;
        }
        tick++;

        /* 1 ms tick: a high-resolution waitable timer gives ~1 ms periods
         * without the CPU cost of a spin loop (Sleep(1) lands at ~1.6 ms). */
        {
            static HANDLE tick_timer;
            if (!tick_timer) {
                tick_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
                if (!tick_timer) tick_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
                LARGE_INTEGER due;
                due.QuadPart = -10000;            /* 1 ms */
                SetWaitableTimer(tick_timer, &due, 1, NULL, NULL, FALSE);
            }
            WaitForSingleObject(tick_timer, 3);
        }
    }
    return 0;
}

/* Hash frame inputs to skip rendering when the signature is unchanged.
 * As with any finite hash, collisions are possible.
 */
static uint64_t frame_signature(void) {
    uint64_t h = 1469598103934665603ULL;
#define MIXB(v) do { uint32_t _b_; memcpy(&_b_, &(v), 4); h ^= _b_; h *= 1099511628211ULL; } while (0)
#define MIX(v)  do { h ^= (uint64_t)(v); h *= 1099511628211ULL; } while (0)
    MIX(g_nrender);
    /* raw float bits: any camera movement at all must change the frame */
    MIXB(g_rcam.pos.x); MIXB(g_rcam.pos.y); MIXB(g_rcam.pos.z);
    MIXB(g_rcam.fwd.x); MIXB(g_rcam.fwd.y); MIXB(g_rcam.fwd.z);
    MIXB(g_rcam.right.x); MIXB(g_rcam.right.y); MIXB(g_rcam.right.z);
    MIXB(g_rcam.up.x); MIXB(g_rcam.up.y); MIXB(g_rcam.up.z); MIXB(g_rcam.fov);
    MIXB(g_rcam.rot.x); MIXB(g_rcam.rot.y); MIXB(g_rcam.rot.z); MIXB(g_rcam.rot.w);
    BR_Config* b = br_get();
    MIX(b->enabled); MIX(b->show_box); MIX(b->show_name); MIX(b->show_distance);
    MIX(b->show_head); MIX(b->show_self); MIX(b->show_skeleton); MIX(b->show_weapon); MIX(b->show_armor); MIX(b->show_crouch); MIX(b->show_health); MIX(b->show_vehicle_markers); MIX(b->show_vehicle_occupants); MIX(b->cham_on);
    MIX(b->radar_on); MIX(b->radar_range); MIX(b->radar_world); MIX(b->show_containers); MIX(b->show_doors); MIX(b->show_dropped_loot); MIX(b->show_vehicle_loot);    MIX(b->line_w); MIX(b->aa_on); MIX(b->aa_fov_px); MIX(b->menu_open);
    MIX(b->max_distance); MIX(b->skel_distance);
    MIX(b->show_clan); MIX(b->show_police);
    MIX(b->show_loot); MIX(b->show_loot_nodes); MIX(b->vehicle_chams);
    MIX(b->loot_tier_none); MIX(b->loot_tier_common); MIX(b->loot_tier_uncommon);
    MIX(b->loot_tier_rare); MIX(b->loot_tier_epic); MIX(b->loot_tier_legendary); MIX(b->loot_tier_exotic);
    int lim = g_nrender;
    for (int i = 0; i < lim; i++) {
        const RR_PLAYER* p = &g_render[i];
        MIXB(p->pos.x); MIXB(p->pos.y); MIXB(p->pos.z);
        MIXB(p->head.x); MIXB(p->head.y); MIXB(p->head.z);
        MIXB(p->dist);
        MIX(p->relationships); MIX(p->full_rig);
        if (b->radar_on && p->local) MIXB(p->rot_yaw);
        MIX(p->id); MIX(p->local); MIX(p->hidden_in_object); MIX(p->dead); MIX(p->armor); MIX(p->crouch); MIX(p->health); MIX(p->is_vehicle); MIX(p->is_loot); MIX(p->loot_rarity); MIX(p->occupants); MIX(p->occupants_known);
        if (b->vehicle_chams && p->is_vehicle) {
            MIX(p->vehicle_cham_valid);
            if (p->vehicle_cham_valid) for (int j = 0; j < 12; ++j) MIXB(p->vehicle_pose[j]);
        }
        if (b->cham_on) {
            MIX(p->cham_valid);
            MIX(p->cham_rig);
            if (p->cham_valid) for (int j=0;j<RR_CHAM_BONES;++j) for (int k=0;k<12;++k) MIXB(p->cham_skin[j][k]);
        }
        for (size_t j = 0; j < sizeof(p->name); ++j) MIX((unsigned char)p->name[j]);
        for (size_t j = 0; j < sizeof(p->weapon); ++j) MIX((unsigned char)p->weapon[j]);
        for (size_t j = 0; j < sizeof(p->role); ++j) MIX((unsigned char)p->role[j]);
        if ((b->show_skeleton || b->cham_on) && p->nbones > 0 && p->nbones <= RR_MAX_BONES) {
            MIX(p->nbones);
            for (int k = 0; k < p->nbones; ++k) {
                MIXB(p->bone[k][0]); MIXB(p->bone[k][1]); MIXB(p->bone[k][2]);
            }
        }
    }
#undef MIX
#undef MIXB
    return h;
}
static int KeyPressed(int vk, int* was) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = down && !*was;
    *was = down;
    return hit;
}

static DWORD WINAPI RenderThread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    int k_f1 = 0, k_ins = 0, k_end = 0;
    uint64_t last_sig = 0;
    DWORD last_post = 0;
    for (;;) {
        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

        if (KeyPressed(VK_F1, &k_f1)) hk_deliver(1, "poll");
        if (KeyPressed(VK_INSERT, &k_ins)) hk_deliver(2, "poll");
        if (KeyPressed(VK_END, &k_end)) { hk_deliver(3, "poll"); break; }

        /* publish a render snapshot */
        EnterCriticalSection(&g_cs);
        g_nrender = g_nbuf[g_front];
        memcpy(g_render, g_buf[g_front], sizeof(RR_PLAYER) * (size_t)g_nrender);
        g_rcam = g_cam[g_front];
        unsigned snapshot_seq = g_snapshot_seq;
        DWORD snapshot_at = g_snapshot_at;
        g_status[sizeof(g_status) - 1] = 0;
        LeaveCriticalSection(&g_cs);

        /* Demo mode must not generate mouse input from synthetic targets. */
        if (!g_demo) { __try { aa_tick(snapshot_seq, snapshot_at); } __except (EXCEPTION_EXECUTE_HANDLER) { hklogf("exception in aa_tick"); } }

        /* Skip presenting an unchanged frame to reduce compositor work and leave
         * more time for window input and the WebView2 menu.
         */
        {
            BR_Config* b = br_get();
            DWORD now = timeGetTime();
            int period = b->menu_open ? 16 : 1;
            if (g_nrender == 0) period = 200;      /* nothing to draw: 5 fps  */
            if (g_hwnd && now - last_post >= (DWORD)period &&
                !InterlockedCompareExchange(&g_repaint_pending, 0, 0)) {
                uint64_t sig = frame_signature();
                if ((sig != last_sig || now - last_post > 500 ||
                     InterlockedCompareExchange(&g_present_retry, 0, 0)) && gpu_frame_ready()) {
                    InterlockedExchange(&g_repaint_pending, 1);
                    if (PostMessageA(g_hwnd, WM_APP + 1, 0, 0)) {
                        last_sig = sig;
                        last_post = now;
                    } else {
                        InterlockedExchange(&g_repaint_pending, 0);
                    }
                }
            }
        }
        Sleep(g_nrender > 0 ? 1 : 48);
    }
    return 0;
}

/* Window. */

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_TIMER:
        if (w == 18 && !g_demo) sync_game_window();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
    case WM_SETFOCUS:
        /* Request discovery when focus returns so changed world/camera pointers
         * can be checked again.
         */
        InterlockedExchange(&g_rescan_now, 1);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_APP + 1:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_HOTKEY:
        hk_deliver((int)w, "hotkey");
        return 0;
    case WM_APP + 2:                      /* from the low-level hook */
        hk_deliver((int)w, "hook");
        return 0;
    case WM_APP + 3:                      /* key released (low-level hook) */
        hk_release((int)w);
        return 0;
    case WM_INPUT: {                      /* device-level keyboard */
        static BYTE buf[128];
        UINT size = sizeof(buf);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            RAWINPUT* ri = (RAWINPUT*)buf;
            if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                const RAWKEYBOARD* kb = &ri->data.keyboard;
                int id = vk_to_id((int)kb->VKey);
                if (id && (kb->Flags & RI_KEY_BREAK)) hk_release(id);
                else if (id) hk_deliver(id, "raw");
            }
        }
        return DefWindowProc(h, m, w, l);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        static RR_PLAYER paint_players[RR_MAX_PLAYERS];
        RR_CAMERA paint_camera;
        int paint_count;
        EnterCriticalSection(&g_cs);
        paint_count = g_nrender;
        if (paint_count < 0) paint_count = 0;
        if (paint_count > RR_MAX_PLAYERS) paint_count = RR_MAX_PLAYERS;
        memcpy(paint_players, g_render, sizeof(RR_PLAYER) * (size_t)paint_count);
        paint_camera = g_rcam;
        LeaveCriticalSection(&g_cs);
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int vw = rc.right - rc.left, vh = rc.bottom - rc.top;
        if (vw > 0 && vh > 0) {
            if (!gpu_ready() && !gpu_init(h, vw, vh)) {
                MessageBoxA(NULL, "GPU backend unavailable (D3D11/DComp)", "RobRunESP", 0);
                EndPaint(h, &ps);
                InterlockedExchange(&g_repaint_pending, 0);
                return 0;
            }
            gpu_size(vw, vh);
            if (!g_demo) game_refresh_render_camera(&paint_camera);
            __try {
                gpu_begin();
                __try { draw_esp(paint_players, paint_count, &paint_camera, vw, vh); }
                __except (EXCEPTION_EXECUTE_HANDLER) { hklogf("exception in draw_esp"); }

                /* footer: state, so an empty screen says *why* it is empty */
                if (g_show_footer) {
                    gpu_font(11.0f);
                    char line[256];
                    snprintf(line, sizeof(line), "ROB RUN  %s", g_status);
                    r_text(12.0f, (float)vh - 18.0f, line, RARGB(170, 170, 170, 170),
                           RT_LEFT | RT_OUTLINE);
                }
                InterlockedExchange(&g_present_retry, !gpu_present());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                hklogf("exception in paint");
                InterlockedExchange(&g_present_retry, 1);
            }
        }
        EndPaint(h, &ps);
        InterlockedExchange(&g_repaint_pending, 0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProc(h, m, w, l);
}

/* Offline tests. */

/* A GUI-subsystem binary has no stdout of its own, so every test/probe line
 * goes to the console *and* to a log file: whoever runs --selftest from a
 * terminal sees it, whoever double-clicks it still has selftest.log. */
static FILE* g_out = NULL;

static void out_open(const char* path) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    freopen("CONOUT$", "w", stdout);
    g_out = fopen(path, "w");
}

static void out_close(void) {
    if (g_out) { fclose(g_out); g_out = NULL; }
    FreeConsole();
}

static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_out) {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
        fflush(g_out);
    }
}

static int g_fail = 0;
static void check(int cond, const char* what) {
    out("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}


static int run_selftest(void) {
    wchar_t temp_dir[MAX_PATH], config_path[MAX_PATH];
    /* Use the same writable working directory as the self-test log. */
    DWORD temp_len = GetCurrentDirectoryW(_countof(temp_dir), temp_dir);
    int have_temp = temp_len && temp_len < _countof(temp_dir) &&
        GetTempFileNameW(temp_dir, L"rrc", 0, config_path);
    BR_Config written = {0}, loaded = {0};
    written.radar_range = 275;
    written.radar_on = 1;
    written.radar_world = 1;
    written.show_health = written.show_crouch = written.show_vehicle_markers = written.show_vehicle_occupants = 1;
    written.show_containers = written.show_doors = written.show_dropped_loot = written.show_vehicle_loot = 1;
    written.aa_los_on = 1;
    written.aa_closest_first = 1;
    written.show_loot = written.show_loot_nodes = written.vehicle_chams = 1;
    written.show_self = 1;
    written.loot_tier_none = written.loot_tier_uncommon = written.loot_tier_epic = written.loot_tier_exotic = 1;
    written.aa_key = VK_RBUTTON;
    written.aa_bone = RR_AIM_FEET;
    written.color[7].rgb[0] = 117;
    check(have_temp && cfg_save_at(config_path, &written) && cfg_load_at(config_path, &loaded) &&
          loaded.radar_range == 275 && loaded.radar_on == 1 && loaded.radar_world == 1 &&
          loaded.show_containers == 1 && loaded.show_doors == 1 && loaded.show_dropped_loot == 1 && loaded.show_vehicle_loot == 1 &&
          loaded.show_health == 1 && loaded.show_crouch == 1 && loaded.show_vehicle_markers == 1 && loaded.show_vehicle_occupants == 1 && loaded.aa_los_on == 1 &&
          loaded.aa_closest_first == 1 && loaded.aa_key == VK_RBUTTON && loaded.aa_bone == RR_AIM_FEET && loaded.color[7].rgb[0] == 117,
          "config round trip saves radar, aim and colors");
    check(loaded.show_loot && loaded.show_loot_nodes && loaded.vehicle_chams,
          "config round trip saves loot and vehicle chams switches");
    check(loaded.show_self == 1, "config round trip saves self ESP switch");
    check(br_loot_rarity_visible(&loaded, 0) && !br_loot_rarity_visible(&loaded, 1) &&
          br_loot_rarity_visible(&loaded, 2) && !br_loot_rarity_visible(&loaded, 3) &&
          br_loot_rarity_visible(&loaded, 4) && !br_loot_rarity_visible(&loaded, 5) &&
          br_loot_rarity_visible(&loaded, 6) && br_loot_rarity_visible(&loaded, -1),
          "loot tier filters round trip and map each rarity");
    if (have_temp) {
        written.radar_range = 125;
        check(cfg_save_at(config_path, &written) && cfg_load_at(config_path, &loaded) &&
              loaded.radar_range == 125, "config save replaces the previous profile");
        FILE* legacy = _wfopen(config_path, L"w");
        int legacy_written = 0;
        if (legacy) {
            fputs("aa_visible=1\naa_head=0\n", legacy);
            legacy_written = fclose(legacy) == 0;
        }
        loaded.aa_los_on = 0;
        check(legacy_written && cfg_load_at(config_path, &loaded) &&
              loaded.aa_los_on == 1 && loaded.aa_bone == 1 && loaded.radar_range == 125,
              "config imports legacy aim settings and retains unspecified values");
        legacy = _wfopen(config_path, L"a");
        legacy_written = 0;
        if (legacy) {
            fputs("aa_los_on=0\naa_bone=2\naa_visible=1\naa_head=0\n", legacy);
            legacy_written = fclose(legacy) == 0;
        }
        check(legacy_written && cfg_load_at(config_path, &loaded) &&
              loaded.aa_los_on == 0 && loaded.aa_bone == 2,
              "config prefers current keys over legacy aliases");
        DeleteFileW(config_path);
    }

    /* 1. Check the IL2CPP layout constants used by this reader. */
    check(U_OBJ_HEADER == 0x10, "IL2CPP object header is 16 bytes");
    check(U_CLASS_STATIC == 0xB8, "Il2CppClass.static_fields at 0xB8 (metadata 39 layout)");
    check(U_STR_LEN == 0x10 && U_STR_CHARS == 0x14, "Il2CppString length/char layout");
    check(U_DICT_ENTRIES == 0x18 && U_DICT_COUNT == 0x20, "Dictionary _entries/_count layout");

    /* 2. generated offsets: present and plausible */
    check(RR_OFF_CHAR_TRANSFORM == 0x58, "BattleCharacterModel.Transform from dump (0x58)");
    check(RR_OFF_WEM_BATTLE_CHARS == 0x18, "WorldEntitiesModel._battleCharacters from dump (0x18)");
    check(RR_OFF_WORLD_ENTITIES == 0x38, "WorldModel.WorldEntitiesModel from dump (0x38)");
    check(RR_OFF_CAM_POS == 0x10 && RR_OFF_CAM_ROT == 0x1C, "BattleCameraModel Position/Orientation");

    /* 3. projection math: camera at origin looking +Z, 90 deg vertical FOV */
    RR_CAMERA cam;
    memset(&cam, 0, sizeof(cam));
    cam.pos.x = 0; cam.pos.y = 0; cam.pos.z = 0;
    cam.fwd.x = 0; cam.fwd.y = 0; cam.fwd.z = 1;
    cam.rot.w = 1.0f;
    cam.fov = 90.0f;
    cam.valid = 1;

    const int W = 1920, H = 1080;
    float sx = 0, sy = 0;
    RR_V3 p = { 0, 0, 10 };
    check(game_w2s(&cam, &p, W, H, &sx, &sy) == 1 &&
          fabsf(sx - 960.0f) < 0.5f && fabsf(sy - 540.0f) < 0.5f,
          "world point on the axis projects to the screen centre");

    p.x = 10; p.y = 0; p.z = 10;            /* 45 deg right */
    int inside = game_w2s(&cam, &p, W, H, &sx, &sy);
    /* 90 deg is the VERTICAL fov, so the horizontal half-angle is
     * atan(aspect * tan(45)) = 60.6 deg and a 45 deg ray is inside the frame:
     * ndc_x = tan(45) / aspect = 0.5625 -> x = 1500. */
    check(inside == 1 && fabsf(sx - 1500.0f) < 1.0f && fabsf(sy - 540.0f) < 0.5f,
          "45 deg right ray lands where the aspect ratio puts it");

    /* the ray exactly on the horizontal edge: atan(aspect*tan(45)) */
    p.x = 1.7777778f * 10.0f; p.y = 0; p.z = 10;
    game_w2s(&cam, &p, W, H, &sx, &sy);
    check(sx > 1918.0f && sx < 1922.0f, "ray on the horizontal edge lands on the right border");

    p.x = 0; p.y = 10; p.z = 10;            /* 45 deg up */
    game_w2s(&cam, &p, W, H, &sx, &sy);
    check(fabsf(sx - 960.0f) < 0.5f && sy < 5.0f, "45 deg up ray lands on the top edge");

    p.x = 0; p.y = 0; p.z = -5;
    check(game_w2s(&cam, &p, W, H, &sx, &sy) == 0, "point behind the camera is rejected");
    (void)inside;

    /* 4. chain file parser (no driver needed: parsing only) */
    {
        const char* tmp = "selftest_chain.txt";
        FILE* f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "# test\nworld = GameAssembly.dll+0x81B7148\ncamera = world+0x38 +0x18\n");
            fclose(f);
        }
        int n = chain_load(tmp);
        check(n == 2, "chain file parsed two symbols");
        check(chain_defined("world") && chain_defined("camera"), "chain symbols are registered");
        DeleteFileA(tmp);
    }

    out("\n%s (%d failure%s)\n", g_fail ? "SELFTEST FAILED" : "SELFTEST OK",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail;
}

/* Probe. */

static void cls_of(uint64_t obj, char* out, size_t n) {
    out[0] = 0;
    uint64_t k = u_klass(obj);
    if (!k) { snprintf(out, n, "<no class>"); return; }
    char nm[120];
    if (!u_class_name(k, nm, sizeof(nm))) { snprintf(out, n, "<bad name>"); return; }
    snprintf(out, n, "%s", nm);
}

static int run_probe(void) {
    if (!rr_attach(g_exe)) {
        out("[probe] '%s' is not running\n", g_exe);
        return 2;
    }
    out("[probe] pid %u\n", g_rr_pid);
    if (!rr_drv_open()) {
        out("[probe] kernel backend unavailable (map kernel-mode.sys first)\n");
        return 3;
    }
    uint64_t mod = 0;
    uint32_t size = 0;
    if (!rr_module("GameAssembly.dll", &mod, &size)) {
        out("[probe] GameAssembly.dll not in the module list of '%s'\n", g_exe);
        /* List loaded modules to help diagnose a missing GameAssembly.dll and
         * check whether the PEB/loader walk returns plausible results.
         */
        {
            static char names[96][64];
            uint64_t bases[96];
            uint32_t sizes[96];
            int n = rr_modules(&names[0][0], bases, sizes, 96);
            out("[probe] target modules: %d\n", n);
            for (int i = 0; i < n && i < 16; i++)
                out("    %-40s base=%llX size=%X\n", names[i],
                    (unsigned long long)bases[i], sizes[i]);
        }
        return 4;
    }
    out("[probe] GameAssembly.dll base=%llX size=%X\n",
           (unsigned long long)mod, size);
    out("[probe] image base=%llX\n", (unsigned long long)rr_image_base());
    {
        uint64_t world = 0, battle = 0, camera = 0;
        int found = game_entry_probe(&world, &battle, &camera);
        out("[probe] UnityEntry: %s world=%llX battle=%llX camera=%llX\n",
            found ? "validated" : "unavailable; scan fallback",
            (unsigned long long)world, (unsigned long long)battle,
            (unsigned long long)camera);
    }

    chain_load("robrun_chain.txt");
    out("[probe] chain symbols: %d\n", chain_count());
    for (int i = 0; i < chain_count(); i++) {
        uint64_t v = chain_value(i);
        char cn[120];
        cls_of(v, cn, sizeof(cn));
        out("  %-12s = %llX  (%s)\n", chain_name(i), (unsigned long long)v, v ? cn : "null");
    }

    /* static-field candidates straight from the generated header */
    const struct { const char* n; uint32_t rva; } statics[] = {
        { "WorldEntitiesModel", RR_SF_WORLDENTITIESMODEL },
        { "WorldModel",         RR_SF_WORLDMODEL },
        { "BattleCameraModel",  RR_SF_BATTLECAMERAMODEL },
        { "BattleCharacterModel", RR_SF_BATTLECHARACTERMODEL },
    };
    for (int i = 0; i < (int)(sizeof(statics) / sizeof(statics[0])); i++) {
        if (!statics[i].rva) continue;
        uint64_t p = rr_u64(mod + statics[i].rva);
        char cn[120];
        cls_of(p, cn, sizeof(cn));
        out("  static %-22s rva=%07X -> %llX (%s)\n", statics[i].n,
               statics[i].rva, (unsigned long long)p, p ? cn : "null");
    }

    int ok = game_init();
    out("[probe] game_init: %s\n", ok ? "world found" : "no world");
    char st[192];
    game_status(st, sizeof(st));
    out("[probe] status: %s\n", st);
    out("[probe] prim offset=0x%X dict offset=0x%X\n",
           game_prim_offset(), game_dict_offset());

    static RR_PLAYER players[RR_MAX_PLAYERS];
    RR_CAMERA cam;
    int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    out("[probe] players=%d camera=%s\n", n, cam.valid ? "valid" : "missing");
    for (int i = 0; i < n && i < 16; i++) {
        out("  #%-3d id=%-5d pos=(%.2f, %.2f, %.2f) dist=%.1f dead=%d\n",
               i, players[i].id, players[i].pos.x, players[i].pos.y, players[i].pos.z,
               players[i].dist, players[i].dead);
    }
    if (n > 0) {
        uint64_t obj = players[0].addr;
        char cn[120];
        cls_of(obj, cn, sizeof(cn));
        out("[probe] player[0] class=%s\n", cn);
        uint64_t hits[8];
        int h = chain_sweep_ptr(obj, 0x10, 0x200, "BattleCameraModel", hits, 8);
        out("[probe] player[0] -> BattleCameraModel hits: %d\n", h);
        for (int i = 0; i < h; i++) out("    %llX\n", (unsigned long long)hits[i]);
    }
    if (PL(chain_addr("world"))) {
        uint64_t w = chain_addr("world");
        uint64_t hits[8];
        int h = chain_sweep_ptr(w, 0x10, 0x200, "WorldEntitiesModel", hits, 8);
        out("[probe] world -> WorldEntitiesModel hits (offset 0x38 expected): %d\n", h);
        for (int i = 0; i < h; i++) {
            uint64_t e = hits[i];
            for (uint32_t off = 0x10; off <= 0x200; off += 8) {
                if (rr_u64(w + off) == e) out("    at +0x%X\n", off);
            }
        }
    }
    return n >= 0 ? 0 : 1;
}

/* Input test. */

/* --inputtest logs events from the same low-level keyboard hook used by
 * the overlay. Compare this log with the hotkey log to narrow down where
 * input delivery or action handling fails.
 */
static LRESULT CALLBACK inputtest_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        out("key vk=0x%02X %s ctrl=%d\n", (unsigned)k->vkCode,
            vk_to_id((int)k->vkCode) ? "<hotkey>" : "",
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0);
    }
    return CallNextHookEx(NULL, code, w, l);
}

static int run_inputtest(int seconds) {
    if (seconds <= 0) seconds = 30;
    out("input test: %d s, press F1 / Ctrl+F1 / INSERT / END ...\n", seconds);
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, inputtest_proc, GetModuleHandleW(NULL), 0);
    out("low-level hook: %s\n", h ? "installed" : "FAILED");
    if (!h) return 1;
    DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    while (GetTickCount() < end) {
        MSG m;
        while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        Sleep(10);
    }
    UnhookWindowsHookEx(h);
    out("input test done\n");
    return 0;
}

/* Crash log. */

/* Record unhandled exceptions so an unexpected exit leaves diagnostics. */
/* Proper x64 walk (StackWalk64), so a crash log names the caller, not just the
 * CRT routine that happened to fault. */
static void rr_log_stack(EXCEPTION_POINTERS* ep, FILE* f) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) return;
    typedef BOOL (WINAPI *PFN_SW64)(DWORD,HANDLE,HANDLE,LPSTACKFRAME64,PVOID,
        PREAD_PROCESS_MEMORY_ROUTINE64,PFUNCTION_TABLE_ACCESS_ROUTINE64,
        PGET_MODULE_BASE_ROUTINE64,PTRANSLATE_ADDRESS_ROUTINE64);
    typedef BOOL (WINAPI *PFN_SYM)(HANDLE,PCSTR,BOOL);
    typedef BOOL (WINAPI *PFN_SF)(HANDLE,DWORD64,PSYMBOL_INFO,PDWORD64);
    PFN_SW64 pSW = (PFN_SW64)(void*)GetProcAddress(dbg, "StackWalk64");
    PFN_SYM pSI = (PFN_SYM)(void*)GetProcAddress(dbg, "SymInitialize");
    PFN_SF pSF = (PFN_SF)(void*)GetProcAddress(dbg, "SymFromAddr");
    if (!pSW || !pSI) return;
    static int inited = 0;
    if (!inited) { pSI(GetCurrentProcess(), NULL, TRUE); inited = 1; }

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;

    for (int k = 0; k < 12; k++) {
        if (!pSW(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
                 &sf, &ctx, NULL, NULL, NULL, NULL)) break;
        HMODULE hm = NULL;
        char m[MAX_PATH] = "?";
        if (GetModuleHandleExA(0x4 | 0x2, (LPCSTR)sf.AddrPC.Offset, &hm) && hm)
            GetModuleFileNameA(hm, m, MAX_PATH);
        fprintf(f, "  frame[%d] %s+0x%llX\n", k, m,
                (unsigned long long)(sf.AddrPC.Offset - (uint64_t)hm));
    }
}

static LONG WINAPI rr_crash_filter(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("crash.log", "a");
    if (f) {
        {
            /* Name the module and RVA so the fault can be placed in a source file */
            char mod[MAX_PATH] = "?";
            HMODULE hm = NULL;
            if (GetModuleHandleExA(0x00000004 /*FROM_ADDRESS*/ | 0x00000002 /*UNCHANGED_REFCOUNT*/,
                                  (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &hm) && hm)
                GetModuleFileNameA(hm, mod, MAX_PATH);
            fprintf(f, "%lu exception=%08lX address=%p module=%s rva=0x%llX\n",
                    (unsigned long)GetTickCount(),
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress, mod,
                    (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)hm));
        }
        rr_log_stack(ep, f);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Loot object dump. */

/* --lootdump prints the cached objects behind displayed loot names.
 * Inspect the model, description, loot description and item description at
 * their resolved addresses to study fields not used by the renderer.
 */
static void loot_hexdump(uint64_t base, const uint8_t* p, unsigned len) {
    unsigned i, j;
    for (i = 0; i < len; i += 16) {
        unsigned n = len - i;
        if (n > 16) n = 16;
        out("    %016llX  ", (unsigned long long)(base + i));
        for (j = 0; j < 16; j++) {
            if (j < n) out("%02X ", p[i + j]);
            else out("   ");
        }
        out(" |");
        for (j = 0; j < n; j++) {
            unsigned char c = p[i + j];
            out("%c", (c >= 32 && c < 127) ? c : '.');
        }
        out("|\n");
    }
}

static void loot_dump_obj(uint64_t addr, const char* label) {
    static uint8_t buf[0x800];
    char cn[96];
    uint64_t klass;
    uint32_t isz_e8 = 0, isz_f0 = 0;
    int ok_e8, ok_f0;
    unsigned isize, want, o;

    if (!PL(addr)) {
        out("  %s ---\n", label);
        return;
    }
    klass = u_klass(addr);
    if (!u_class_name(klass, cn, sizeof cn)) strcpy(cn, "?");
    ok_e8 = PL(klass) ? rr_read(klass + U_CLASS_GENCONT, &isz_e8, 4) : 0;
    ok_f0 = PL(klass) ? rr_read(klass + U_CLASS_INST_SIZE, &isz_f0, 4) : 0;

    /* Try the current and older instance-size locations, then use a bounded
     * measured window. A fallback size is an estimate and may omit fields.
     */
    if (ok_f0 && isz_f0 >= 0x18 && isz_f0 <= 0x800) isize = isz_f0;
    else if (ok_e8 && isz_e8 >= 0x18 && isz_e8 <= 0x800) isize = isz_e8;
    else if (strstr(cn, "LootNodeModel") || strstr(cn, "RandomLootObjectModel")) isize = 0xC0;
    else isize = 0x100;

    out("  %s @ %016llX  class=%s  size=0x%X  [0xF0=%u/%s 0xE8=%u/%s]\n",
        label, (unsigned long long)addr, cn, isize,
        isz_f0, ok_f0 ? "ok" : "unreadable", isz_e8, ok_e8 ? "ok" : "unreadable");

    want = isize;
    while (want > 0x18 && !rr_read(addr, buf, want)) want -= 8;
    if (!rr_read(addr, buf, want)) { out("    (unreadable)\n"); return; }

    for (o = 0x10; o + 8 <= want; o += 8) {
        uint64_t v, k2;
        char c2[96];
        memcpy(&v, buf + o, 8);
        if (!v) continue;
        if (!PL(v)) {
            uint32_t lo;
            float fv;
            memcpy(&lo, &v, 4);
            memcpy(&fv, &lo, 4);
            out("    +0x%02X = %d, %d   (0x%016llX)  lo32 f=%g\n",
                o, (int)lo, (int)(uint32_t)(v >> 32),
                (unsigned long long)v, (double)fv);
            continue;
        }
        k2 = u_klass(v);
        if (!u_class_name(k2, c2, sizeof c2)) {
            /* Print both 32-bit halves when a value is not identified as a pointer;
             * a packed pair of integers can be numerically larger than the pointer cutoff.
             */
            uint32_t lo;
            float fv;
            memcpy(&lo, &v, 4);
            memcpy(&fv, &lo, 4);
            out("    +0x%02X = %d, %d   (0x%016llX)  lo32 f=%g\n",
                o, (int)lo, (int)(uint32_t)(v >> 32),
                (unsigned long long)v, (double)fv);
            continue;
        }
        out("    +0x%02X -> %s @ %016llX", o, c2, (unsigned long long)v);
        if (strcmp(c2, "String") == 0) {
            char s[128];
            if (u_string(v, s, sizeof s)) out("  \"%s\"", s);
        } else if (strstr(c2, "IntData")) {
            out("  value=%d", rr_i32(v + 0x18));
        } else if (strstr(c2, "BoolData")) {
            out("  value=%u", (unsigned)rr_u32(v + 0x18));
        } else if (strstr(c2, "FloatData")) {
            out("  value=%.3f", rr_f32(v + 0x18));
        }
        out("\n");

        /* One level deeper for the wrapper types. A PrimitiveModel`1 /
         * ReferenceData`1 holds the real value at +0x10, so a field whose class
         * is only a wrapper tells us nothing until it is opened: the node's
         * per-instance amount, if it has one, is exactly this shape -
         * node+0x20 -> PrimitiveModel`1 -> +0x10 -> IntData -> +0x18. */
        if (strstr(c2, "PrimitiveModel") || strstr(c2, "ReferenceData")) {
            uint64_t inner = rr_u64(v + 0x10);
            char c3[96];
            if (PL(inner) && u_class_name(u_klass(inner), c3, sizeof c3)) {
                out("        inner %s @ %016llX", c3, (unsigned long long)inner);
                if (strstr(c3, "IntData")) out(" value=%d", rr_i32(inner + 0x18));
                else if (strstr(c3, "FloatData")) out(" value=%.3f", rr_f32(inner + 0x18));
                else if (strstr(c3, "BoolData")) out(" value=%u", (unsigned)rr_u32(inner + 0x18));
                else if (strstr(c3, "StringData")) {
                    uint64_t sp = rr_u64(inner + 0x18);
                    char s[96];
                    if (PL(sp) && u_string(sp, s, sizeof s)) out(" \"%s\"", s);
                } else if (strcmp(c3, "String") == 0) {
                    char s[96];
                    if (u_string(inner, s, sizeof s)) out(" \"%s\"", s);
                }
                out("\n");
            }
        }
    }
    loot_hexdump(addr, buf, want);
}

/* Case-insensitive substring test. Used for the name filter because the item
 * name in the data does not have to match the case the ESP draws. */
static int ci_contains(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; ++hay) {
        size_t k = 0;
        while (k < nl && hay[k]) {
            char a = hay[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            ++k;
        }
        if (k == nl) return 1;
    }
    return 0;
}

static void loot_dump_entry(const RR_LOOT_ENT* e, int idx, const char* tag) {
    out("\n%s[%d] id=%d kind=%d rarity=%d name=\"%s\" pos=(%.1f, %.1f, %.1f)\n",
        tag, idx, e->id, e->kind, e->rarity, e->name, e->pos.x, e->pos.y, e->pos.z);
    loot_dump_obj(e->obj, "node");
    loot_dump_obj(e->desc, "desc");
    loot_dump_obj(e->loot, "loot");
    loot_dump_obj(e->inv, "item");
}

static void loot_report(const RR_LOOT_DEBUG* d, int got) {
    out("[lootdump] world=%016llX nodes dict=%016llX count=%d loose dict=%016llX count=%d cached=%d\n",
        (unsigned long long)d->world,
        (unsigned long long)d->dict_nodes, d->entries_nodes,
        (unsigned long long)d->dict_loose, d->entries_loose, got);
    out("[lootdump] nodes: seen=%d empty=%d unresolved=%d   loose: seen=%d empty=%d unresolved=%d\n",
        d->nodes_seen, d->nodes_empty, d->nodes_bad,
        d->loose_seen, d->loose_empty, d->loose_bad);
}

static int run_lootdump(const char* filter) {
    static RR_LOOT_ENT ents[256];
    RR_LOOT_DEBUG dbg;
    int loops, n = 0, i, shown = 0, got;
    enum { MAX_SHOW = 3 };

    if (!rr_attach(g_exe)) { out("[lootdump] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[lootdump] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[lootdump] game_init: no world\n"); return 4; }
    out("[lootdump] pid %u filter=\"%s\"\n", g_rr_pid, filter ? filter : "");

    /* Drive the cache directly: the discovery worker only refreshes it every two
     * seconds and only when the overlay's loot switches are on, which a CLI
     * process cannot rely on. */
    memset(&dbg, 0, sizeof dbg);
    got = rr_loot_refresh(&dbg);
    loot_report(&dbg, got);
    for (loops = 0; got <= 0 && loops < 40; ++loops) {
        Sleep(200);
        got = rr_loot_refresh(&dbg);
    }
    if (got > 0) n = rr_loot_snapshot(ents, 256);
    out("[lootdump] %d loot entries after %d poll(s)\n", n, loops);
    if (!n) {
        loot_report(&dbg, got);
        out("[lootdump] nothing published.\n"
            "           world=0 means the scan found no WorldEntitiesModel;\n"
            "           count=0 means the dictionary is empty (area not streamed in);\n"
            "           count>0 with cached=0 means the entries failed validation.\n");
        return 5;
    }

    /* The names first: the filter is a convenience, and guessing at a name that
     * the data does not use is what makes a good dump look like an empty one. */
    out("\n[lootdump] all entries:\n");
    for (i = 0; i < n; ++i)
        out("  [%02d] kind=%d id=%-5d rarity=%d name=\"%s\"\n",
            i, ents[i].kind, ents[i].id, ents[i].rarity, ents[i].name);

    /* "#73" dumps exactly that index from the list above; otherwise nodes are
     * preferred over loose loot, because a filter like "cash" matches both and
     * the interesting one is the node. */
    {
        int want_idx = (filter && filter[0] == '#') ? atoi(filter + 1) : -1;
        if (want_idx >= 0) {
            if (want_idx < n) {
                loot_dump_entry(&ents[want_idx], want_idx, "");
                shown = 1;
            } else {
                out("\n[lootdump] index %d out of range (0..%d)\n", want_idx, n - 1);
            }
        } else {
            int pass, k;
            for (pass = 0; pass < 2 && shown < MAX_SHOW; ++pass) {
                int want_kind = pass == 0 ? 2 : 1;
                for (k = 0; k < n && shown < MAX_SHOW; ++k) {
                    if (ents[k].kind != want_kind) continue;
                    if (!ci_contains(ents[k].name, filter)) continue;
                    loot_dump_entry(&ents[k], k, "");
                    ++shown;
                }
            }
        }
    }

    /* Nothing matched: the node's structure does not depend on its name, so dump
     * a couple of them anyway rather than returning empty-handed again. */
    if (!shown) {
        int fallback = 0;
        out("\n[lootdump] no name matched \"%s\" - dumping nodes regardless\n", filter);
        for (i = 0; i < n && fallback < 2; ++i) {
            if (ents[i].kind != 2) continue;
            loot_dump_entry(&ents[i], i, "  ");
            ++fallback;
        }
        shown = fallback;
    }

    out("\n[lootdump] %d of %d entries dumped\n", shown, n);
    return shown ? 0 : 6;
}

/* --lootset writes a requested range to each distinct cached loot-table
 * record. Read back each write to check the client value at that moment.
 * This does not establish that the server accepted a change.
 */
static int run_lootset(const char* filter, int vmin, int vmax) {
    static RR_LOOT_ENT ents[256];
    static uint64_t done[32];
    RR_LOOT_DEBUG dbg;
    int n, i, k, ndone = 0, written = 0;

    if (!rr_attach(g_exe)) { out("[lootset] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[lootset] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[lootset] game_init: no world\n"); return 4; }
    (void)rr_loot_refresh(&dbg);
    n = rr_loot_snapshot(ents, 256);
    out("[lootset] %d entries, filter=\"%s\", target %d..%d\n",
        n, filter ? filter : "", vmin, vmax);
    if (!n) return 5;

    for (i = 0; i < n; ++i) {
        RR_LOOT_ENT* e = &ents[i];
        int old_min, old_max, new_min = -1, new_max = -1, dup = 0;

        if (!ci_contains(e->name, filter)) continue;
        if (!u_is(e->loot, "LootNodeLootDescription")) continue;
        for (k = 0; k < ndone; ++k) if (done[k] == e->loot) { dup = 1; break; }
        if (dup) continue;
        if (ndone < (int)(sizeof(done) / sizeof(done[0]))) done[ndone++] = e->loot;

        old_min = rr_i32(e->loot + 0x28);
        old_max = rr_i32(e->loot + 0x2C);
        if (rr_write(e->loot + 0x28, &vmin, 4) && rr_write(e->loot + 0x2C, &vmax, 4)) {
            new_min = rr_i32(e->loot + 0x28);
            new_max = rr_i32(e->loot + 0x2C);
        }
        out("  %-22s table %016llX  %d..%d -> %d..%d  %s\n",
            e->name, (unsigned long long)e->loot, old_min, old_max,
            new_min, new_max,
            (new_min == vmin && new_max == vmax) ? "VERIFIED" : "NOT APPLIED");
        if (new_min == vmin && new_max == vmax) written++;
    }
    out("[lootset] %d distinct table(s), %d written and verified\n", ndone, written);
    return written ? 0 : 6;
}

/* --itemset writes the shared item-description value and checks the
 * client result. The meaning and persistence of that value depend on the
 * game build; a successful local write does not prove a payout changed.
 */
static int run_itemset(const char* filter, int value) {
    static RR_LOOT_ENT ents[256];
    static uint64_t done[32];
    RR_LOOT_DEBUG dbg;
    int n, i, k, ndone = 0, written = 0;

    if (!rr_attach(g_exe)) { out("[itemset] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[itemset] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[itemset] game_init: no world\n"); return 4; }
    (void)rr_loot_refresh(&dbg);
    n = rr_loot_snapshot(ents, 256);
    out("[itemset] %d entries, filter=\"%s\", value=%d\n", n, filter ? filter : "", value);
    if (!n) return 5;

    for (i = 0; i < n; ++i) {
        RR_LOOT_ENT* e = &ents[i];
        int old_v, new_v = -1, dup = 0;

        if (!ci_contains(e->name, filter)) continue;
        if (!u_is(e->inv, "InventoryItemDescription")) continue;
        for (k = 0; k < ndone; ++k) if (done[k] == e->inv) { dup = 1; break; }
        if (dup) continue;
        if (ndone < (int)(sizeof(done) / sizeof(done[0]))) done[ndone++] = e->inv;

        old_v = rr_i32(e->inv + 0x2C);
        if (rr_write(e->inv + 0x2C, &value, 4)) new_v = rr_i32(e->inv + 0x2C);
        out("  %-22s item %016llX  value %d -> %d  %s\n",
            e->name, (unsigned long long)e->inv, old_v, new_v,
            (new_v == value) ? "VERIFIED" : "NOT APPLIED");
        if (new_v == value) written++;

        /* Loose loot also stores an instance quantity at +0x28 in this build.
         * Write it separately from the shared item-description value.
         */
        if (u_is(e->loot, "LootObjectItemDescription")) {
            int lo = rr_i32(e->loot + 0x28), ln = -1;
            if (rr_write(e->loot + 0x28, &value, 4)) ln = rr_i32(e->loot + 0x28);
            out("  %-22s loot %016llX  count %d -> %d  %s\n",
                e->name, (unsigned long long)e->loot, lo, ln,
                (ln == value) ? "VERIFIED" : "NOT APPLIED");
        }
    }
    out("[itemset] %d distinct item(s), %d written and verified\n", ndone, written);
    return written ? 0 : 6;
}

/* --cashscan searches the local character object graph for a supplied
 * amount as int32 and int64 values at four-byte alignment. Search depth
 * bounds traversal cost. A numeric match is a candidate, not proof that the
 * field controls the displayed balance. This diagnostic only reads memory.
 */
typedef struct { uint64_t addr, parent; uint32_t poff; int depth; } CASHNODE;

static int cash_visited(const CASHNODE* q, int n, uint64_t a) {
    int i;
    for (i = 0; i < n; ++i) if (q[i].addr == a) return 1;
    return 0;
}

static int run_cashscan(long long amount, int maxdepth) {
    static CASHNODE q[3000];
    char cn[96];
    uint64_t start;
    int qn = 0, head = 0, hits = 0;

    if (!rr_attach(g_exe)) { out("[cashscan] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[cashscan] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[cashscan] game_init: no world\n"); return 4; }

    start = rr_local_player();
    if (!start) {
        RR_CAMERA cam;
        static RR_PLAYER players[RR_MAX_PLAYERS];
        int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
        out("[cashscan] no local player flagged yet (%d entities) - try again in game\n", n);
        return 5;
    }
    if (!u_class_name(u_klass(start), cn, sizeof cn)) strcpy(cn, "?");
    out("[cashscan] local player %016llX (%s), looking for %lld, depth %d\n",
        (unsigned long long)start, cn, amount, maxdepth);

    q[qn].addr = start; q[qn].parent = 0; q[qn].poff = 0; q[qn].depth = 0; qn++;

    while (head < qn) {
        CASHNODE n = q[head++];
        uint64_t klass = u_klass(n.addr);
        uint32_t isz = 0;
        unsigned want = 0x120, o;
        static uint8_t buf[0x400];

        if (!u_class_name(klass, cn, sizeof cn)) strcpy(cn, "?");
        if (PL(klass)) {
            isz = rr_u32(klass + U_CLASS_INST_SIZE);
            if (isz >= 0x18 && isz < 0x401) want = isz;
        }
        while (want > 0x18 && !rr_read(n.addr, buf, want)) want -= 8;
        if (!rr_read(n.addr, buf, want)) continue;

        /* 4-byte granularity catches an int32 wallet that is not 8-aligned. */
        for (o = 0x10; o + 4 <= want; o += 4) {
            int32_t v32;
            memcpy(&v32, buf + o, 4);
            if ((long long)v32 == amount) {
                out("[cashscan] HIT int32 %lld at %016llX  [%s]%s\n", amount,
                    (unsigned long long)(n.addr + o), cn,
                    n.parent ? "" : " (root object)");
                if (n.parent)
                    out("[cashscan]    reached from %016llX +0x%X\n",
                        (unsigned long long)n.parent, n.poff);
                hits++;
            }
            if (o + 8 <= want) {
                int64_t v64;
                memcpy(&v64, buf + o, 8);
                if (v64 == amount) {
                    out("[cashscan] HIT int64 %lld at %016llX  [%s]\n", amount,
                        (unsigned long long)(n.addr + o), cn);
                    hits++;
                }
            }
        }

        /* Any IntData/Int32Data in the graph is worth listing by value even when
         * it does not match: it shows what the wallet-sized fields look like. */
        if (strstr(cn, "IntData") || strstr(cn, "Int32Data")) {
            out("[cashscan]   intdata %016llX %s value=%d\n",
                (unsigned long long)n.addr, cn, rr_i32(n.addr + 0x18));
        }

        if (n.depth >= maxdepth) continue;
        for (o = 0x10; o + 8 <= want; o += 8) {
            uint64_t v, k2;
            char c2[96];
            memcpy(&v, buf + o, 8);
            if (!PL(v)) continue;
            k2 = u_klass(v);
            if (!u_class_name(k2, c2, sizeof c2)) continue;
            if (qn >= (int)(sizeof(q) / sizeof(q[0]))) break;
            if (cash_visited(q, qn, v)) continue;
            q[qn].addr = v; q[qn].parent = n.addr; q[qn].poff = o; q[qn].depth = n.depth + 1; qn++;
        }
    }
    out("[cashscan] %d object(s) walked, %d hit(s)\n", qn, hits);
    return hits ? 0 : 6;
}

/* Read a printable class name from a candidate Il2CppClass. Check the
 * embedded type self-pointer at both supported offsets and report the result
 * as stronger/weaker evidence rather than assuming one universal layout.
 */
static int klass_name_at(uint64_t v, char* name, size_t n, int* strong) {
    uint64_t image, self;
    int i, any = 0;

    *strong = 0;
    if (!PL(v) || (v & 7)) return 0;
    image = rr_u64(v);
    if (!PL(image)) return 0;
    if (!u_class_name(v, name, n)) return 0;
    for (i = 0; name[i] && i < (int)n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c > 126) return 0;
        any = 1;
    }
    if (!any) return 0;
    if (rr_read(v + 0x20, &self, sizeof self) && self == v) *strong = 1;
    else if (rr_read(v + 0x18, &self, sizeof self) && self == v) *strong = 1;
    return 1;
}

/* Distinct class names seen behind the value matches, so a class that appears
 * many times can be told from a one-off false positive at a glance. */
typedef struct { char name[64]; int count, strong; } CASHCLS;
static CASHCLS g_cashcls[48];
static int g_ncashcls;

static void cashcls_note(const char* n, int strong) {
    int i;
    for (i = 0; i < g_ncashcls; ++i) {
        if (strcmp(g_cashcls[i].name, n) == 0) { g_cashcls[i].count++; g_cashcls[i].strong |= strong; return; }
    }
    if (g_ncashcls < (int)(sizeof(g_cashcls) / sizeof(g_cashcls[0]))) {
        memset(&g_cashcls[g_ncashcls], 0, sizeof(g_cashcls[g_ncashcls]));
        strncpy(g_cashcls[g_ncashcls].name, n, sizeof(g_cashcls[g_ncashcls].name) - 1);
        g_cashcls[g_ncashcls].count = 1;
        g_cashcls[g_ncashcls].strong = strong;
        g_ncashcls++;
    }
}

/* Report one hit, with the class of the object that contains it found by
 * scanning back for the object header (klass pointer at offset 0). Returns 1
 * when a class was identified, 0 when the hit was just a number. */
static int cashfind_report(uint64_t hit, long long amount, int verbose) {
    char cn[96];
    unsigned off;
    for (off = 0x10; off <= 0x400; off += 8) {
        uint64_t base = hit - off;
        int strong = 0;
        if (base < 0x10000) break;
        if (klass_name_at(base, cn, sizeof cn, &strong)) {
            cashcls_note(cn, strong);
            if (verbose)
                out("[cashfind] %016llX = %lld   in %s +0x%X (%s)\n",
                    (unsigned long long)hit, amount, cn, off, strong ? "strong" : "weak");
            return 1;
        }
    }
    return 0;
}

/* --cashfind searches committed memory for a supplied int32 value and
 * keeps matches near a plausible managed-object header. This filters noise
 * but does not prove that a matched field controls the displayed amount.
 */
static int run_cashfind(long long amount) {
    enum { BATCH = 64, CHUNK = 0x1000, MAXSHOW = 80 };
    uint64_t addr = 0x10000;
    const uint64_t limit = 0x7FFFFFFEFFFFull;
    uint8_t* pool;
    long chunks = 0;
    int hits = 0, raw = 0;
    long long last_mb = -1;

    if (!rr_attach(g_exe)) { out("[cashfind] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[cashfind] kernel backend unavailable\n"); return 3; }
    pool = (uint8_t*)malloc((size_t)BATCH * CHUNK);
    if (!pool) return 6;
    out("[cashfind] searching all private memory for the int32 %lld\n", amount);

    /* Check class identification against the independently resolved local
     * player before interpreting search results. Failure here makes negative
     * results from the same identification logic inconclusive.
     */
    {
        uint64_t lp = rr_local_player();
        if (!lp) {
            out("[cashfind] self-check skipped: no local player flagged yet\n");
        } else {
            uint64_t k = u_klass(lp);
            char n[96];
            int strong = 0;
            if (klass_name_at(k, n, sizeof n, &strong))
                out("[cashfind] self-check: player klass %016llX -> %s (%s)\n",
                    (unsigned long long)k, n, strong ? "strong" : "weak");
            else {
                free(pool);
                out("[cashfind] self-check FAILED: could not name the local player's own class\n"
                    "[cashfind] the identification logic is broken, so a scan would mean nothing - stop\n");
                return 7;
            }
        }
    }

    while (addr < limit) {
        RR_REGION rg;
        if (!rr_query_region(addr, &rg) || !rg.size) { addr += 0x10000; continue; }
        {
            uint64_t start = rg.base < 0x10000 ? 0x10000 : rg.base;
            uint64_t end = rg.base + rg.size;
            uint32_t pr = rg.protect & 0xFF;
            int usable = rg.state == 0x1000 && rg.type == 0x20000 &&
                         pr != 0 && !(pr & 0x01) && !(pr & 0x100);
            uint64_t q;
            for (q = start; usable && q < end; q += (uint64_t)BATCH * CHUNK) {
                RR_RD_REQ req[BATCH];
                int n = 0, i;
                for (i = 0; i < BATCH && q + (uint64_t)i * CHUNK < end; i++) {
                    uint64_t a = q + (uint64_t)i * CHUNK;
                    unsigned sz = (unsigned)((end - a) < CHUNK ? (end - a) : CHUNK);
                    req[n].addr = a; req[n].size = sz; req[n].dst = pool + (size_t)n * CHUNK;
                    memset(req[n].dst, 0, sz);
                    n++;
                }
                if (!n) break;
                if (rr_read_batch(req, n) <= 0) g_cashfind_bad++;
                for (i = 0; i < n; i++) {
                    uint8_t* p = (uint8_t*)(pool + (size_t)i * CHUNK);
                    unsigned o;
                    for (o = 0; o + 4 <= req[i].size; o += 4) {
                        int32_t v;
                        memcpy(&v, p + o, 4);
                        if ((long long)v != amount) continue;
                        raw++;
                        if (cashfind_report(req[i].addr + (uint64_t)o, amount, hits < MAXSHOW))
                            hits++;
                    }
                }
                chunks += n;
                {
                    long long mb = ((long long)chunks * CHUNK) >> 20;
                    if (mb != last_mb) {
                        last_mb = mb;
                        out("[cashfind] %lld MB read, %d raw, %d identified\n", mb, raw, hits);
                    }
                }
            }
            addr = (end > addr) ? end : addr + 0x10000;
        }
    }
    free(pool);
    out("[cashfind] %ld chunks, %d value(s) matched, %d inside an identifiable object%s\n",
        chunks, raw, hits, hits >= MAXSHOW ? " (display limit reached)" : "");
    /* Group matches by candidate class to highlight repeated patterns for
     * further inspection. Match frequency alone does not establish field meaning.
     */
    if (g_ncashcls) {
        int i, k;
        out("[cashfind] classes behind the matches:\n");
        for (i = 0; i < g_ncashcls; ++i) {
            int best = i;
            for (k = i + 1; k < g_ncashcls; ++k)
                if (g_cashcls[k].count > g_cashcls[best].count) best = k;
            if (best != i) { CASHCLS t = g_cashcls[i]; g_cashcls[i] = g_cashcls[best]; g_cashcls[best] = t; }
            out("  %-40s %3d  %s\n", g_cashcls[i].name, g_cashcls[i].count,
                g_cashcls[i].strong ? "strong" : "weak");
        }
    }
    if (g_cashfind_bad)
        out("[cashfind] WARNING: %ld batches returned no data\n", g_cashfind_bad);
    return hits ? 0 : 6;
}

/* --xfprobe samples both character transform fields and counts observed
 * changes. Comparing their cadence helps separate replicated updates from
 * rendered interpolation. The observed rate depends on the current session.
 */
static int run_xfprobe(int ms) {
    static RR_PLAYER players[RR_MAX_PLAYERS];
    RR_CAMERA cam;
    uint64_t addr = 0, t58 = 0, t60 = 0;
    float pa[3], pb[3];
    int have = 0, ca = 0, cb = 0, differ = 0, samples = 0;
    DWORD last_a = 0, last_b = 0;
    long sum_a = 0, sum_b = 0;

    if (!rr_attach(g_exe)) { out("[xfprobe] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[xfprobe] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[xfprobe] no world - get into a match first\n"); return 4; }

    int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    float best = 1e30f;
    int best_i = -1;
    for (int i = 0; i < n; ++i) {
        if (!players[i].valid || players[i].local || players[i].is_loot ||
            players[i].is_vehicle || players[i].dead) continue;
        if (players[i].dist > 0.5f && players[i].dist < best) {
            best = players[i].dist;
            best_i = i;
        }
    }
    if (best_i < 0) { out("[xfprobe] no remote player in view (%d entries)\n", n); return 5; }

    addr = players[best_i].addr;
    t58 = rr_u64(addr + RR_OFF_CHAR_TRANSFORM);
    t60 = rr_u64(addr + RR_OFF_CHAR_REPL_TRANSFORM);
    out("[xfprobe] %d entries, target %016llX at %.1f m\n",
        n, (unsigned long long)addr, (double)best);
    out("[xfprobe] Transform          0x58 -> %016llX\n", (unsigned long long)t58);
    out("[xfprobe] ReplicatedTransform 0x60 -> %016llX\n", (unsigned long long)t60);
    if (!PL(t58) && !PL(t60)) {
        out("[xfprobe] neither offset resolved to a model - offsets are stale\n");
        return 6;
    }
    if (PL(t58) && rr_read(t58 + 0x10, pa, sizeof(pa)) == 0) memset(pa, 0, sizeof(pa));
    if (PL(t60) && rr_read(t60 + 0x10, pb, sizeof(pb)) == 0) memset(pb, 0, sizeof(pb));
    have = 1;

    if (ms < 500) ms = 500;
    if (ms > 5000) ms = 5000;
    out("[xfprobe] sampling for %d ms - keep the target moving\n", ms);
    DWORD t0 = timeGetTime();
    for (;;) {
        DWORD now = timeGetTime();
        if ((int)(now - t0) >= ms) break;
        if (PL(t58)) {
            float v[3];
            if (rr_read(t58 + 0x10, v, sizeof(v)) && memcmp(v, pa, sizeof(v)) != 0) {
                if (have && last_a) sum_a += (long)(now - last_a);
                ++ca;
                last_a = now;
                memcpy(pa, v, sizeof(pa));
            }
        }
        if (PL(t60)) {
            float v[3];
            if (rr_read(t60 + 0x10, v, sizeof(v)) && memcmp(v, pb, sizeof(v)) != 0) {
                if (have && last_b) sum_b += (long)(now - last_b);
                ++cb;
                last_b = now;
                memcpy(pb, v, sizeof(pb));
            }
        }
        if (PL(t58) && PL(t60) && memcmp(pa, pb, sizeof(pa)) != 0) ++differ;
        ++samples;
        Sleep(1);
    }

    out("[xfprobe] %d samples over %d ms\n", samples, ms);
    out("[xfprobe] 0x58 Transform          : %d changes, mean interval %.1f ms\n",
        ca, ca > 1 ? (double)sum_a / (double)(ca - 1) : 0.0);
    out("[xfprobe] 0x60 ReplicatedTransform: %d changes, mean interval %.1f ms\n",
        cb, cb > 1 ? (double)sum_b / (double)(cb - 1) : 0.0);
    out("[xfprobe] samples where the two disagree: %d of %d\n", differ, samples);
    out("[xfprobe] --- read this ---\n");
    out("[xfprobe] an interval near the simulated tick is the network transform;\n"
        "[xfprobe] one changing far more often is interpolated and is what the\n"
        "[xfprobe] overlay should be reading. If they never disagree, 0x60 is not\n"
        "[xfprobe] a separate transform on this build and the fix has to be\n"
        "[xfprobe] extrapolation by time-since-last-change instead.\n");
    return 0;
}

/* --aimtest samples the camera around a known SendInput movement to
 * estimate the current counts-to-angle ratio. No observed rotation can have
 * several causes, including focus, input filtering or a stale camera sample.
 */
static float aimtest_yaw(const RR_CAMERA* c) {
    return atan2f(c->fwd.x, c->fwd.z) * 57.29577951f;
}

static int run_aimtest(void) {
    static RR_PLAYER players[RR_MAX_PLAYERS];
    RR_CAMERA cam;
    float before, after;
    int n;

    if (!rr_attach(g_exe)) {
        out("[aimtest] '%s' is not running\n", g_exe);
        return 2;
    }
    if (!rr_drv_open()) {
        out("[aimtest] kernel backend unavailable\n");
        return 3;
    }
    if (!game_init()) {
        out("[aimtest] no world - get into a match first\n");
        return 4;
    }

    n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    if (!cam.valid) {
        out("[aimtest] camera invalid (players=%d) - cannot measure\n", n);
        return 5;
    }
    before = aimtest_yaw(&cam);
    out("[aimtest] players=%d  yaw before = %.3f deg\n", n, before);
    out("[aimtest] click into the game now - injecting in 5 s\n");
    Sleep(5000);

    out("[aimtest] sending 20 x +8 px horizontal\n");
    for (int i = 0; i < 20; i++) {
        aa_move_mouse(8, 0);
        Sleep(15);
    }
    Sleep(250);

    n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    after = cam.valid ? aimtest_yaw(&cam) : before;
    out("[aimtest] players=%d  yaw after  = %.3f deg  (delta %.3f)\n", n, after,
        after - before);

    /* Estimate mouse counts per degree from the observed yaw change. This
     * calibration depends on the current game sensitivity and valid camera reads.
     */
    float counts_sent = 20.0f * 8.0f;
    if (fabsf(after - before) > 0.05f) {
        float delta = fabsf(after - before);
        float deg_per_count = delta / counts_sent;
        /* Convert counts per degree into counts per reference pixel using the
         * vertical angle represented by a reference pixel.
         */
        int vh = g_sh > 16 ? g_sh : 1080;
        float deg_per_px = cam.fov / (float)vh;
        float counts_per_px = deg_per_px / deg_per_count;
        out("[aimtest] PASS: injected input moves the view (delta %.3f deg for %.0f counts)\n",
            after - before, (double)counts_sent);
        out("[aimtest] CALIBRATION %.2f mouse counts per degree   (%.4f deg per count)\n",
            (double)(counts_sent / delta), (double)deg_per_count);
        out("[aimtest] at %.0f deg FOV over %d px: %.3f counts per screen pixel\n",
            (double)cam.fov, vh, (double)counts_per_px);
        out("[aimtest] aim maths assumed 1.000 -> set aa_aim_cal=%d\n",
            (int)(counts_per_px * 100.0f + 0.5f));
        out("[aimtest] (CoreRR.cfg: aa_aim_cal=%d, then restart the overlay)\n",
            (int)(counts_per_px * 100.0f + 0.5f));
    } else
        out("[aimtest] FAIL: the view did not move. The game is not accepting\n"
            "           injected mouse input, so no aim tuning can help.\n");
    return 0;
}

/* --ctxread requests a thread context and logs the returned status.
 * It requests no suspension or register modification. The result tests this
 * driver operation only; it does not establish thread execution behavior.
 */
static int find_game_threads(uint32_t pid, uint32_t* out, int max);

static int run_ctxread(void) {
    uint32_t tids[64];
    int n, got = 0, denied = 0;
    long first_fail = 0;

    if (!rr_attach(g_exe)) {
        out("[ctx] '%s' is not running\n", g_exe);
        return 2;
    }
    if (!rr_drv_open()) {
        out("[ctx] kernel backend unavailable (map the new kernel-mode.sys)\n");
        return 3;
    }

    n = find_game_threads(g_rr_pid, tids, 64);
    out("[ctx] pid %u, %d thread(s)\n", g_rr_pid, n);
    if (!n) return 4;

    for (int i = 0; i < n && i < 16; i++) {
        RR_THREAD_CTX ctx;
        long st = rr_drv_get_context(tids[i], &ctx);

        if (st == 0 && ctx.ok) {
            got++;
            out("[ctx] tid %-6u rip=%016llX rsp=%016llX rbp=%016llX flags=%08llX\n",
                tids[i], (unsigned long long)ctx.rip, (unsigned long long)ctx.rsp,
                (unsigned long long)ctx.rbp, (unsigned long long)ctx.rflags);
        } else {
            denied++;
            if (!first_fail) first_fail = st;
        }
    }

    out("[ctx] %d context(s) readable, %d not", got, denied);
    if (denied && first_fail)
        out(" (first status %08lX)\n", (unsigned long)first_fail);
    else
        out("\n");

    if (!got) {
        out("[ctx] no context obtainable - a context hijack is not available\n"
            "      on this system, and the write-only feature set stands.\n");
        return 5;
    }
    out("[ctx] contexts are readable; the next step is a stub that restores\n"
        "      the saved registers before resuming.\n");
    return 0;
}

/* Enumerate up to max thread IDs belonging to pid. Return the stored count,
 * or zero if the process has no matching threads or enumeration fails.
 */
static int find_game_threads(uint32_t pid, uint32_t* out, int max) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (n < max) out[n++] = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

/* Driver write self-test. */

/* --writetest checks driver reads and writes against a buffer owned by
 * this process, including rejection of an invalid address and split-batch
 * behavior. It requires the driver; it is separate from --selftest.
 */
static int run_writetest(void) {
    const size_t N = 64;
    uint8_t pattern[64], back[64];
    uint8_t* buf;
    int pass = 0, fail = 0;

    out("driver write test\n");

    if (!rr_drv_open()) {
        out("FAIL: driver not available (mapped? elevated? session key?)\n");
        return 1;
    }

    buf = (uint8_t*)VirtualAlloc(NULL, N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        out("FAIL: could not allocate the test buffer\n");
        rr_drv_close();
        return 1;
    }

    /* Address requests to this process so the test uses its own allocation. */
    g_rr_pid = GetCurrentProcessId();
    out("target pid %u (this process)\n", (unsigned)g_rr_pid);
    out("buffer %p, %u bytes\n", (void*)buf, (unsigned)N);

    for (size_t i = 0; i < N; i++) pattern[i] = (uint8_t)(0xA5u ^ (unsigned)(i * 7u));
    memset(buf, 0, N);

    if (rr_write((uint64_t)(uintptr_t)buf, pattern, N)) {
        pass++;
        out("write: accepted\n");
    } else {
        fail++;
        out("FAIL: rr_write rejected\n");
    }

    memset(back, 0, sizeof(back));
    if (!rr_read((uint64_t)(uintptr_t)buf, back, N)) {
        fail++;
        out("FAIL: read back rejected\n");
    } else if (memcmp(back, pattern, N) != 0) {
        fail++;
        out("FAIL: memory does not hold what was written\n");
    } else {
        pass++;
        out("read back: %u bytes match\n", (unsigned)N);
    }

    /* A write to an address no process maps must be refused, not "succeed":
     * reporting a failed write as done is the failure mode that would make a
     * feature look like it worked while changing nothing. */
    if (rr_write(0x10, pattern, 8)) {
        fail++;
        out("FAIL: write to 0x10 reported success\n");
    } else {
        pass++;
        out("write to 0x10 correctly refused\n");
    }

    /* Check a 64-page request (256 KiB) to exercise client-side splitting
     * across the driver reply limit. Verify item statuses and returned contents.
     */
    {
        enum { PAGES = 64, PSIZE = 0x1000 };
        const size_t BIG = (size_t)PAGES * PSIZE;
        static uint8_t back2[PAGES * PSIZE];
        uint8_t* big = (uint8_t*)VirtualAlloc(NULL, BIG, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
        if (!big) {
            fail++;
            out("FAIL: could not allocate the batch buffer\n");
        } else {
            RR_RD_REQ req[PAGES];
            int i, got, match = 1;
            size_t k;
            for (k = 0; k < BIG; k++) big[k] = (uint8_t)(k * 13u + 7u);
            memset(back2, 0, sizeof(back2));
            for (i = 0; i < PAGES; i++) {
                req[i].addr = (uint64_t)(uintptr_t)(big + (size_t)i * PSIZE);
                req[i].size = PSIZE;
                req[i].dst = back2 + (size_t)i * PSIZE;
            }
            got = rr_read_batch(req, PAGES);
            for (k = 0; k < BIG && match; k++) if (back2[k] != big[k]) match = 0;
            if (got == PAGES && match) {
                pass++;
                out("batch read: %d x 4 KB copied and matched (%u KB)\n",
                    PAGES, (unsigned)(BIG >> 10));
            } else {
                fail++;
                out("FAIL: batch read returned %d of %d want, content %s\n",
                    got, PAGES, match ? "ok" : "MISMATCH");
            }
            VirtualFree(big, 0, MEM_RELEASE);
        }
    }

    VirtualFree(buf, 0, MEM_RELEASE);
    g_rr_pid = 0;
    rr_drv_close();

    out("result: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

/* Main. */

/* Scanner entry points live in their own translation units (rr_nodescan.c,
 * rr_classscan.c) because they own a whole-address-space read loop and have
 * nothing to do with the overlay. */
int run_nodescan(const char* text);
int run_classscan(const char* text);

/* `--no-pause`: the diagnostic modes normally wait for a key so a double-clicked
 * window does not vanish, but a .bat driving them would block forever. */
static int g_cli_no_pause;

static void cli_pause(void) {
    if (g_cli_no_pause) return;
    cli_pause();
}

/* Read-only, headless sample of the same pose filter used by the renderer. */
static int run_chamdiag(int trace) {
    if (!rr_attach(g_exe) || !rr_drv_open()) { out("attach or driver unavailable\n"); return 2; }
    BR_Config* cfg=br_get();
    cfg->enabled=1; cfg->cham_on=1; cfg->show_self=0;
    cfg->vehicle_chams=0; cfg->show_loot=0; cfg->show_loot_nodes=0; cfg->aa_on=0;
    out("initializing live reader\n");
    uint64_t world_hint=0;
    FILE* status=fopen("..\\1_run\\status.log","r");
    if (status) {
        char line[512];
        while (fgets(line,sizeof(line),status)) {
            char* found=strstr(line,"world=");
            if (found) world_hint=_strtoui64(found+6,NULL,16);
        }
        fclose(status);
    }
    if (!game_pose_diag_init(world_hint)) { out("live roster unavailable\n"); return 3; }
    game_pose_diag_enable(); cham_diag_enable();
    FILE* trace_file=trace?fopen("chamtrace.csv","w"):NULL;
    uint64_t trace_target=0;
    if (trace && !trace_file) { out("could not open trace file\n"); return 4; }
    if (trace_file) {
        fprintf(trace_file,"utc_ms,tick_ms,frame,id,player_x,player_y,player_z");
        const char* labels[4]={"head","neck","left","right"};
        for(int b=0;b<4;++b) for(int k=0;k<12;++k) fprintf(trace_file,",%s_%d",labels[b],k);
        fputc('\n',trace_file);
    }
    static RR_PLAYER players[RR_MAX_PLAYERS]; RR_CAMERA camera;
    DWORD start=timeGetTime(), report=start;
    unsigned ticks=0, players_seen=0;
    while (timeGetTime()-start < (trace?90000u:30000u)) {
        int n=game_snapshot(players,RR_MAX_PLAYERS,&camera);
        if (trace_file && !trace_target) {
            int chosen=-1;
            for (int i=0;i<n;++i) if (players[i].valid && !players[i].local && !players[i].is_loot &&
                !players[i].is_vehicle && players[i].cham_valid) {
                if (!_stricmp(players[i].name,"Ranzar")) { chosen=i; break; }
            }
            if (chosen>=0) { trace_target=players[chosen].addr; out("trace target name=%s id=%d\n",players[chosen].name,players[chosen].id); }
        }
        for (int i=0;i<n;++i) {
            if (players[i].valid && !players[i].local && !players[i].is_vehicle && !players[i].is_loot) ++players_seen;
            cham_diag_sample(&players[i]);
            if (trace_file && trace_target && players[i].addr==trace_target && players[i].cham_valid) {
                FILETIME ft; ULARGE_INTEGER utc;
                GetSystemTimeAsFileTime(&ft); utc.LowPart=ft.dwLowDateTime; utc.HighPart=ft.dwHighDateTime;
                unsigned long long utc_ms=(utc.QuadPart-116444736000000000ULL)/10000ULL;
                RR_PLAYER* x=&players[i];
                fprintf(trace_file,"%llu,%lu,%u,%d,%.6f,%.6f,%.6f",utc_ms,(unsigned long)timeGetTime(),ticks,x->id,x->pos.x,x->pos.y,x->pos.z);
                const int bones[4]={4,3,6,17};
                for(int b=0;b<4;++b) for(int k=0;k<12;++k) fprintf(trace_file,",%.7g",x->cham_skin[bones[b]][k]);
                fputc('\n',trace_file);
            }
        }
        ++ticks;
        DWORD now=timeGetTime();
        if (now-report >= 10000) {
            RR_CHAM_DIAG c=cham_diag_results(); RR_POSE_DIAG p=game_pose_diag_results();
            out("elapsed=%lu ticks=%u observed=%u fresh=%u repeated=%u initial=%u state=%u gap=%u jump=%u max_gap_ms=%u max_jump_m=%.3f attempts=%u success=%u failed=%u mesh_reused=%u cham_reused=%u no_mesh=%u\n",
                (unsigned long)(now-start),ticks,players_seen,c.fresh,c.repeated,c.initial,c.state,c.gap,c.jump,c.max_gap_ms,c.max_jump_m,
                p.attempts,p.success,p.failed,p.reused_mesh,p.reused_cham,p.missing_rig);
            out("motion moving=%u reversals=%u bone_jumps=%u max_bone_delta_m=%.3f head_spikes=%u left_spikes=%u right_spikes=%u\n",c.moving_samples,c.reversals,c.bone_jumps,c.max_bone_delta_m,c.head_spikes,c.left_spikes,c.right_spikes);
            out("local_quaternions samples=%u neck_reversals=%u left_reversals=%u right_reversals=%u paired_reads=%u paired_changes=%u\n",p.quat_samples,p.neck_reversals,p.left_reversals,p.right_reversals,p.pair_samples,p.pair_changes);
            out("asset_rest both_arms=%u rest_then_return=%u\n",p.both_arms_rest,p.rest_reversions);
            out("rotation_return_run_lengths 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u\n",p.rotation_run_return[0],p.rotation_run_return[1],p.rotation_run_return[2],p.rotation_run_return[3],p.rotation_run_return[4],p.rotation_run_return[5]);
            out("coordinated_pose_holds=%u\n",p.held_spikes);
            out("standing_samples=%u seated_samples=%u seated_holds=%u seated_root_bypass=%u\n",p.standing_samples,p.seated_samples,p.seated_holds,p.seated_root_bypass);
            out("rotation_masks none=%u head=%u left=%u head_left=%u right=%u head_right=%u arms=%u all=%u\n",p.change_mask[0],p.change_mask[1],p.change_mask[2],p.change_mask[3],p.change_mask[4],p.change_mask[5],p.change_mask[6],p.change_mask[7]);
            out("rig_mapping_joints=%u close_alternatives=%u\n",p.mapping_joints,p.mapping_ambiguous);
            out("rig_named_joints=%u name_mismatches=%u\n",p.named_joints,p.mismatched_joints);
            for (int b=0;b<RR_CHAM_BONES;++b) if (p.ambiguous_by_bone[b]) out("rig_ambiguous bone=%d count=%u\n",b,p.ambiguous_by_bone[b]);
            for (int b=0;b<RR_CHAM_BONES;++b) if (p.mismatch_by_bone[b]) out("rig_mismatch bone=%d count=%u\n",b,p.mismatch_by_bone[b]);
            report=now;
        }
        Sleep(8);
    }
    if (trace_file) fclose(trace_file);
    return 0;
}
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)nShow;
    SetUnhandledExceptionFilter(rr_crash_filter);
    int mode_menu = lpCmd && strstr(lpCmd, "--menu");
    int mode_probe = lpCmd && strstr(lpCmd, "--probe");
    int mode_chamdiag = lpCmd && strstr(lpCmd, "--chamdiag");
    int mode_chamtrace = lpCmd && strstr(lpCmd, "--chamtrace");
    int mode_test = lpCmd && strstr(lpCmd, "--selftest");
    int mode_input = lpCmd && strstr(lpCmd, "--inputtest");
    int mode_write = lpCmd && strstr(lpCmd, "--writetest");
    int mode_nodescan = lpCmd && strstr(lpCmd, "--nodescan");
    int mode_classscan = lpCmd && strstr(lpCmd, "--classscan");
    int mode_lootdump = lpCmd && strstr(lpCmd, "--lootdump");
    int mode_lootset = lpCmd && strstr(lpCmd, "--lootset");
    int mode_itemset = lpCmd && strstr(lpCmd, "--itemset");
    int mode_cashscan = lpCmd && strstr(lpCmd, "--cashscan");
    int mode_cashfind = lpCmd && strstr(lpCmd, "--cashfind");
    int mode_ctx = lpCmd && strstr(lpCmd, "--ctxread");
    int mode_aimtest = lpCmd && strstr(lpCmd, "--aimtest");
    int mode_xfprobe = lpCmd && strstr(lpCmd, "--xfprobe");
    int mode_demo = lpCmd && strstr(lpCmd, "--demo");
    /* Process hiding requests a kernel process-list change. Availability and
     * behavior depend on the mapped driver and Windows build; check the request
     * result rather than assuming that the process became hidden.
     */
    /* Hiding is requested by default unless --nohide is supplied. Several
     * diagnostic modes bypass it explicitly below; menu/demo modes should also
     * be started with --nohide when studying the UI without driver access.
     */
    int mode_nohide = lpCmd && strstr(lpCmd, "--nohide");
    int mode_hide = !mode_nohide;
    /* Scripted runs must not stop at the end-of-mode prompt. */
    g_cli_no_pause = (lpCmd && strstr(lpCmd, "--no-pause")) ? 1 : 0;
    g_demo = mode_demo;
    if (mode_hide && !mode_nohide && !mode_test && !mode_probe && !mode_input &&
        !mode_write && !mode_aimtest && !mode_ctx && !mode_xfprobe)
        (void)rr_hide_my_process();
    {
        const char* e = lpCmd ? strstr(lpCmd, "--exe ") : NULL;
        if (e) {
            e += 6;
            size_t i = 0;
            while (*e && *e != ' ' && i + 1 < sizeof(g_exe)) g_exe[i++] = *e++;
            g_exe[i] = 0;
        }
    }

    if (mode_test) {
        out_open("selftest.log");
        out("RobRunESP selftest\n");
        int rc = run_selftest();
        if (getenv("RR_PAUSE")) { out("press enter..."); (void)getchar(); }
        out_close();
        return rc;
    }
    if (mode_chamdiag || mode_chamtrace) {
        out_open("chamdiag.log");
        int rc=run_chamdiag(mode_chamtrace);
        out_close();
        return rc;
    }
    if (mode_input) {
        out_open("inputtest.log");
        int secs = 30;
        const char* s = lpCmd ? strstr(lpCmd, "--inputtest") : NULL;
        if (s) { s += 11; while (*s == ' ') s++; if (*s >= '0' && *s <= '9') secs = atoi(s); }
        int rc = run_inputtest(secs);
        out_close();
        return rc;
    }
    if (mode_probe) {
        out_open("probe.log");
        out("RobRunESP probe\n");
        int rc = run_probe();
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_write) {
        out_open("writetest.log");
        int rc = run_writetest();
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_xfprobe) {
        int ms = 2000;
        const char* a = lpCmd ? strstr(lpCmd, "--xfprobe ") : NULL;
        if (a) { int v = atoi(a + 10); if (v > 0) ms = v; }
        out_open("xfprobe.log");
        out("RobRunESP transform probe\n");
        int rc = run_xfprobe(ms);
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_aimtest) {
        out_open("aimtest.log");
        out("RobRunESP injected input test\n");
        int rc = run_aimtest();
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_nodescan) {
        out_open("nodescan.log");
        out("RobRunESP node scan\n");
        const char* a = strstr(lpCmd, "--nodescan ");
        int rc = run_nodescan(a ? a + 11 : "node_cash_1");
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_cashfind) {
        const char* a = lpCmd ? strstr(lpCmd, "--cashfind ") : NULL;
        long long amount = 0;
        if (a) (void)sscanf(a + 11, "%lld", &amount);
        out_open("cashfind.log");
        out("RobRunESP whole-heap value search");
        int rc = run_cashfind(amount);
        out_close();
        return rc;
    }
    if (mode_cashscan) {
        const char* a = lpCmd ? strstr(lpCmd, "--cashscan ") : NULL;
        long long amount = 0;
        int depth = 3;
        if (a) (void)sscanf(a + 11, "%lld %d", &amount, &depth);
        if (depth < 1) depth = 1;
        if (depth > 5) depth = 5;
        out_open("cashscan.log");
        out("RobRunESP cash field scan");
        int rc = run_cashscan(amount, depth);
        out_close();
        return rc;
    }
    if (mode_itemset) {
        const char* a = lpCmd ? strstr(lpCmd, "--itemset ") : NULL;
        int value = 0;
        char filt[64];
        memset(filt, 0, sizeof filt);
        strcpy(filt, "cash");
        if (a) (void)sscanf(a + 10, "%d %63s", &value, filt);
        out_open("itemset.log");
        out("RobRunESP item value write");
        int rc = run_itemset(filt, value);
        out_close();
        return rc;
    }
    if (mode_lootset) {
        const char* a = lpCmd ? strstr(lpCmd, "--lootset ") : NULL;
        int vmin = 0, vmax = 0;
        char filt[64];
        memset(filt, 0, sizeof filt);
        strcpy(filt, "cash");
        if (a) (void)sscanf(a + 10, "%d %d %63s", &vmin, &vmax, filt);
        out_open("lootset.log");
        out("RobRunESP loot table write\n");
        int rc = run_lootset(filt, vmin, vmax);
        out_close();
        return rc;
    }
    if (mode_lootdump) {
        const char* a = lpCmd ? strstr(lpCmd, "--lootdump ") : NULL;
        const char* f = a ? a + 11 : "";
        out_open("lootdump.log");
        out("RobRunESP loot object dump\n");
        int rc = run_lootdump(f);
        out_close();
        return rc;
    }
    if (mode_classscan) {
        /* run_classscan owns its own log and console; it must not share the
         * out() file handle or the two would truncate each other's log. */
        const char* a = strstr(lpCmd, "--classscan ");
        int rc = run_classscan(a ? a + 12 : "LootNodeModel,InventoryItemDescription");
        return rc;
    }
    if (mode_ctx) {
        out_open("ctxread.log");
        out("RobRunESP thread context probe\n");
        int rc = run_ctxread();
        cli_pause();
        out_close();
        return rc;
    }

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    br_get();                       /* allocates the shared config + defaults */
    br_config_load();

    if (!mode_menu && !mode_demo) {
        if (!rr_attach(GAME_EXE)) {
            MessageBoxA(NULL, "ROB RUN is not running", "RobRunESP", 0);
            return 1;
        }
        if (!rr_drv_open()) {
            MessageBoxA(NULL, "Kernel backend unavailable - run START.bat to map the driver",
                        "RobRunESP", 0);
            return 2;
        }
        /* Run discovery on ReadThread because scanning can take seconds. This
         * keeps the window, menu and hotkeys responsive while the footer shows progress.
         */
        g_show_footer = 1;
        snprintf(g_status, sizeof(g_status), "starting - discovering world...");
    }
    if (mode_demo) {
        snprintf(g_status, sizeof(g_status), "DEMO - synthetic players");
        g_show_footer = 1;
    }
    /* Standalone UI opens immediately; the overlay opens its menu with F1. */
    br_set_menu_visible(mode_menu ? 1 : 0);
    br_start_menu();

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "RR_Overlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "RR_Overlay", "RobRunESP", WS_POPUP | WS_VISIBLE, 0, 0, g_sw, g_sh,
        NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 4;

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    /* Real system hotkeys: delivered as WM_HOTKEY to the overlay thread even
     * while the game (or the menu) has the foreground. MOD_NOREPEAT so holding
     * the key does not flap the menu. The polled path in the render thread
     * covers the keys another application already registered. */
    g_hk_menu = RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_NOREPEAT, VK_F1) ? 1 : 0;
    g_hk_esp  = RegisterHotKey(g_hwnd, 2, MOD_CONTROL | MOD_NOREPEAT, VK_INSERT) ? 1 : 0;
    g_hk_quit = RegisterHotKey(g_hwnd, 3, MOD_CONTROL | MOD_NOREPEAT, VK_END) ? 1 : 0;
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc, GetModuleHandleW(NULL), 0);
    if (!mode_menu && !mode_demo) { sync_game_window(); SetTimer(g_hwnd, 18, 500, NULL); }
    int raw_ok = raw_input_on();
    hklogf("init F1=%d INSERT=%d END=%d llhook=%d rawinput=%d",
           g_hk_menu, g_hk_esp, g_hk_quit, g_kb_hook ? 1 : 0, raw_ok);

    if (!mode_menu && !mode_demo) rr_los_start();
    if (!mode_menu) CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    if (!mode_menu) CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_quit, 1);
    br_stop_menu();
    br_config_save();
    rr_los_stop();
    /* After the message loop exits, close this client device handle. The
     * mapped image stays resident. overlay_exit() describes the exit policy.
     */
    rr_drv_close();
    return 0;
}

















































