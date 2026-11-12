/* rr_silent.c - Módulo de Silent Aim */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define OFF_CHAR_LOOK      0x70u
#define OFF_LOOK_VALUE     0x10u
#define OFF_LOOK_YAW       0x1Cu
#define OFF_LOOK_PITCH     0x20u
#define OFF_CAM_POS        0x10u
#define OFF_CAM_FWD        0x3Cu

typedef struct { float x, y, z; } SA_V3;

// Funções externas do seu projeto
int rr_read(uint64_t addr, void* buf, size_t size);
int rr_write(uint64_t addr, const void* buf, size_t size);
uint64_t resolve_local_character(void);
uint64_t resolve_battle_camera(void);
int class_named(uint64_t obj, const char* name);

static volatile LONG g_sa_on = 0;
static volatile float g_sa_point[3] = { 0.0f, 0.0f, 0.0f };
static volatile LONG g_sa_thread_up = 0;

static int silent_write(void) {
    const uint64_t local = resolve_local_character();
    if (!local) return 0;

    uint64_t look_model = 0;
    if (!rr_read(local + OFF_CHAR_LOOK, &look_model, sizeof look_model)) return 0;
    if (!look_model) return 0;
    if (!class_named(look_model, "LookDirectionModel")) return 0;

    const uint64_t cam = resolve_battle_camera();
    if (!cam) return 0;

    SA_V3 eye;
    if (!rr_read(cam + OFF_CAM_POS, &eye, sizeof eye)) return 0;

    const float dx = g_sa_point[0] - eye.x;
    const float dy = g_sa_point[1] - eye.y;
    const float dz = g_sa_point[2] - eye.z;
    const float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (!(len > 0.5f) || !isfinite(len)) return 0;

    const float v[3] = {dx / len, dy / len, dz / len};
    return rr_write(look_model + OFF_LOOK_VALUE, v, sizeof v);
}

static DWORD WINAPI silent_worker(LPVOID unused) {
    (void)unused;
    for (;;) {
        if (InterlockedCompareExchange(&g_sa_on, 0, 0)) {
            silent_write();
        }
        Sleep(1);
    }
}

// Função principal chamada pelo loop do cheat
void silent_aim_update(int menu_enabled, int target_valid, SA_V3 target) {
    // Se o botão estiver desativado no menu ou não houver alvo válido, desliga o silent
    if (!menu_enabled || !target_valid) {
        InterlockedExchange(&g_sa_on, 0);
        return;
    }

    g_sa_point[0] = target.x;
    g_sa_point[1] = target.y;
    g_sa_point[2] = target.z;
    InterlockedExchange(&g_sa_on, 1);

    // Inicializa a thread de background apenas uma vez
    if (InterlockedCompareExchange(&g_sa_thread_up, 1, 0) == 0) {
        HANDLE th = CreateThread(NULL, 0, silent_worker, NULL, 0, NULL);
        if (th) CloseHandle(th);
        else InterlockedExchange(&g_sa_thread_up, 0);
    }
}