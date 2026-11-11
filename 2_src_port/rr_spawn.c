/* rr_spawn.c - Process creation and optional helper-DLL loading.
 *
 * See rr_spawn.h for return values, ownership and cleanup rules. Process
 * creation uses a Win32 parent attribute; DLL loading uses target-process
 * handles. The separate hide helper makes a request through rr_mem.c.
 */
#include "rr_spawn.h"

#include "rr_mem.h"

#include <windows.h>
#include <winbase.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

/* Parent heuristics. */

/* Choose a live dllhost.exe, explorer.exe or svchost.exe PID as a candidate
 * parent for process creation. Return zero when none is found. A returned
 * PID may exit before the caller opens it.
 */
static DWORD rr_find_trusted_parent(void)
{
    DWORD best = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"dllhost.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"explorer.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"svchost.exe") == 0)
            {
                if (!best || entry.th32ProcessID > best)
                    best = entry.th32ProcessID;
            }
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return best;
}

/* True if a PID refers to a currently running process. */
static BOOL rr_is_running(DWORD pid)
{
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (ph)
    {
        CloseHandle(ph);
        return TRUE;
    }
    return FALSE;
}

/* Attribute list. */

/*
 * Build a one-entry PROC_THREAD_ATTRIBUTE_PARENT_PROCESS list for `parent`.
 * The list is heap-allocated; the caller must DeleteProcThreadAttributeList()
 * it and then HeapFree() it. `parent` must stay open until CreateProcess has
 * returned. Returns NULL when the attribute cannot be applied; the caller
 * decides whether that is tolerable (heuristic parent) or fatal (explicitly
 * requested parent).
 */
static LPPROC_THREAD_ATTRIBUTE_LIST rr_make_parent_attribute(HANDLE parent)
{
    SIZE_T size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST list = NULL;

    /*
     * The sizing call is *expected* to fail with ERROR_INSUFFICIENT_BUFFER and
     * to report the required byte count in `size`; that is the documented
     * design of the API. Only a zero size is a real failure.
     */
    SetLastError(ERROR_SUCCESS);
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    if (size == 0)
        return NULL;

    list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, size);
    if (!list)
        return NULL;

    if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
    {
        HeapFree(GetProcessHeap(), 0, list);
        return NULL;
    }

    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                                   &parent, sizeof(parent), NULL, NULL))
    {
        DeleteProcThreadAttributeList(list);
        HeapFree(GetProcessHeap(), 0, list);
        return NULL;
    }

    return list;
}

/*
 * Create the child. `attrs` may be NULL, in which case a plain STARTUPINFOW is
 * used and no parent-process attribute is applied.
 */
static BOOL rr_create_child(const WCHAR* app, WCHAR* cmdline,
                            LPPROC_THREAD_ATTRIBUTE_LIST attrs,
                            PROCESS_INFORMATION* pi)
{
    if (attrs)
    {
        STARTUPINFOEXW si;
        ZeroMemory(&si, sizeof(si));
        si.StartupInfo.cb = (DWORD)sizeof(si);
        si.lpAttributeList = attrs;
        return CreateProcessW(app, cmdline, NULL, NULL, FALSE,
                              EXTENDED_STARTUPINFO_PRESENT,
                              NULL, NULL, &si.StartupInfo, pi);
    }
    else
    {
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = (DWORD)sizeof(si);
        return CreateProcessW(app, cmdline, NULL, NULL, FALSE, 0,
                              NULL, NULL, &si, pi);
    }
}

/* Exported API. */

DWORD rr_spawn_with_parent(DWORD parent_pid, const char* path, const char* args,
                           char* err, size_t err_cap)
{
    WCHAR wpath[1024];
    WCHAR wargs[8192];
    WCHAR cmdline[16384];
    HANDLE hparent = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    PROCESS_INFORMATION pi;
    DWORD pid = 0;
    BOOL parent_applied = FALSE;

    if (!path || !*path)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_with_parent: null path");
        return 0;
    }

    /* Everything the Win32 create path takes is wide; convert once up front. */
    wpath[0] = L'\0';
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0]))) == 0)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_with_parent: path is invalid or too long");
        return 0;
    }

    wargs[0] = L'\0';
    if (args && *args &&
        MultiByteToWideChar(CP_ACP, 0, args, -1, wargs,
                            (int)(sizeof(wargs) / sizeof(wargs[0]))) == 0)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_with_parent: args are invalid or too long");
        return 0;
    }

    /* Quote the exe so paths with spaces survive argv parsing. */
    if (wargs[0])
    {
        if (swprintf_s(cmdline, sizeof(cmdline) / sizeof(cmdline[0]),
                       L"\"%s\" %s", wpath, wargs) < 0)
        {
            if (err && err_cap)
                snprintf(err, err_cap, "rr_spawn_with_parent: command line too long");
            return 0;
        }
    }
    else if (swprintf_s(cmdline, sizeof(cmdline) / sizeof(cmdline[0]),
                        L"\"%s\"", wpath) < 0)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_with_parent: command line too long");
        return 0;
    }

    /* Keep the selected parent handle open until CreateProcess consumes the
     * parent-process attribute. Close it after the create attempt.
     */
    {
        DWORD wanted = parent_pid ? parent_pid : rr_find_trusted_parent();
        if (wanted)
        {
            hparent = OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION,
                                  FALSE, wanted);
            if (!hparent)
            {
                DWORD code = GetLastError();

                /*
                 * An explicitly requested parent that cannot be opened is a
                 * hard failure: silently spawning with the wrong parent would
                 * report success for something that did not happen. A parent
                 * picked by the heuristic may fall back, since no caller asked
                 * for that specific PID.
                 */
                if (parent_pid)
                {
                    if (err && err_cap)
                        snprintf(err, err_cap,
                                 "rr_spawn_with_parent: OpenProcess(parent %lu) failed (%lu)",
                                 (unsigned long)wanted, (unsigned long)code);
                    return 0;
                }
            }
            else
            {
                attrs = rr_make_parent_attribute(hparent);
                if (!attrs)
                {
                    DWORD acode = GetLastError();
                    CloseHandle(hparent);
                    hparent = NULL;

                    if (parent_pid)
                    {
                        if (err && err_cap)
                            snprintf(err, err_cap,
                                     "rr_spawn_with_parent: parent attribute for pid %lu failed (%lu)",
                                     (unsigned long)wanted, (unsigned long)acode);
                        return 0;
                    }
                }
                else
                {
                    parent_applied = TRUE;
                }
            }
        }
    }

    ZeroMemory(&pi, sizeof(pi));
    if (!rr_create_child(wpath, cmdline, attrs, &pi))
    {
        DWORD code = GetLastError();

        if (attrs)
        {
            DeleteProcThreadAttributeList(attrs);
            HeapFree(GetProcessHeap(), 0, attrs);
        }
        if (hparent)
            CloseHandle(hparent);

        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_with_parent: CreateProcessW failed (%lu)",
                     (unsigned long)code);
        return 0;
    }

    pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (attrs)
    {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
    if (hparent)
        CloseHandle(hparent);

    if (err && err_cap)
        snprintf(err, err_cap,
                 parent_applied ? "ok" : "ok (parent attribute not applied)");
    return pid;
}

typedef struct rr_hide_ctx
{
    DWORD pid;
    BOOL changed;
} rr_hide_ctx;

static BOOL CALLBACK rr_hide_window_cb(HWND hwnd, LPARAM lp)
{
    rr_hide_ctx* ctx = (rr_hide_ctx*)lp;
    DWORD owner = 0;

    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != ctx->pid)
        return TRUE;

    {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_TOOLWINDOW))
        {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
            ShowWindow(hwnd, SW_HIDE);
            ctx->changed = TRUE;
        }
    }
    return TRUE;
}

/* Request process hiding through the driver's SMX_IO_HIDE operation.
 * Window styling is applied separately to remove the taskbar/Alt-Tab entry.
 * The return value reports the driver request result, not an independent
 * check of process visibility.
 */
static BOOL rr_hide_process(void)
{
    rr_hide_ctx ctx;
    BOOL hidden;

    if (!rr_drv_ready() && !rr_drv_open())
        return FALSE;

    hidden = rr_drv_hide_process((uint32_t)GetCurrentProcessId(), 1) ? TRUE : FALSE;

    if (hidden)
    {
        ctx.pid = GetCurrentProcessId();
        ctx.changed = FALSE;
        EnumWindows(rr_hide_window_cb, (LPARAM)&ctx);
    }

    return hidden;
}

/* Resolve an export relative to the target process module layout.
 * Find the module containing the local export (including forwarded exports),
 * reject different process bitness, then apply the export offset to the
 * matching target module base. This assumes compatible module versions;
 * matching names and bitness alone do not verify identical binaries.
 */
static FARPROC rr_remote_export(HANDLE target, DWORD pid, const char* dll,
                                const char* name, char* err, size_t err_cap)
{
    HMODULE local_base = NULL;
    FARPROC local_proc;
    char local_path[MAX_PATH];
    const char* base_name;
    const char* p;
    BOOL self_wow = FALSE;
    BOOL target_wow = FALSE;
    HMODULE target_base = NULL;
    SIZE_T offset;

    local_proc = GetProcAddress(GetModuleHandleA(dll), name);
    if (!local_proc)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_remote_export: %s!%s is not exported here", dll, name);
        return NULL;
    }

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)local_proc, &local_base) || !local_base)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_remote_export: cannot identify the module owning %s", name);
        return NULL;
    }

    if (!GetModuleFileNameA(local_base, local_path, (DWORD)sizeof(local_path)))
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_remote_export: cannot name the module owning %s", name);
        return NULL;
    }

    base_name = local_path;
    for (p = local_path; *p; ++p)
    {
        if (*p == '\\' || *p == '/')
            base_name = p + 1;
    }

    /* Cross-bitness injection would apply a meaningless offset. */
    if (!IsWow64Process(GetCurrentProcess(), &self_wow) ||
        !IsWow64Process(target, &target_wow))
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_remote_export: cannot determine process bitness");
        return NULL;
    }

    if (self_wow != target_wow)
    {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "rr_remote_export: bitness mismatch (this process is %s, target is %s)",
                     self_wow ? "WOW64" : "native", target_wow ? "WOW64" : "native");
        return NULL;
    }

    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W entry;
            WCHAR wbase[MAX_PATH];

            entry.dwSize = sizeof(entry);
            if (MultiByteToWideChar(CP_ACP, 0, base_name, -1, wbase,
                                    (int)(sizeof(wbase) / sizeof(wbase[0]))) != 0)
            {
                if (Module32FirstW(snap, &entry))
                {
                    do
                    {
                        if (_wcsicmp(entry.szModule, wbase) == 0)
                        {
                            target_base = (HMODULE)entry.modBaseAddr;
                            break;
                        }
                    } while (Module32NextW(snap, &entry));
                }
            }
            CloseHandle(snap);
        }
    }

    if (!target_base)
    {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "rr_remote_export: %s is not loaded in pid %lu",
                     base_name, (unsigned long)pid);
        return NULL;
    }

    offset = (SIZE_T)((const BYTE*)local_proc - (const BYTE*)local_base);
    return (FARPROC)((BYTE*)target_base + offset);
}

static DWORD rr_stage_and_load(HANDLE target, DWORD pid, const char* module_path,
                                const char* module_name, char* err, size_t err_cap)
{
    char full[MAX_PATH * 2];
    DWORD path_len;
    FARPROC load_library;
    void* remote = NULL;
    DWORD result = 0;
    BOOL loaded = FALSE;
    const char* why = NULL;

    if (!module_path || !*module_path)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_stage_and_load: null module path");
        return 0;
    }

    full[0] = '\0';
    path_len = GetFullPathNameA(module_path, (DWORD)(sizeof(full) / sizeof(full[0])),
                                full, NULL);
    if (path_len == 0 || path_len >= (DWORD)(sizeof(full) / sizeof(full[0])))
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_stage_and_load: module path is empty or too long");
        return 0;
    }

    load_library = rr_remote_export(target, pid, "kernel32.dll", "LoadLibraryA",
                                    err, err_cap);
    if (!load_library)
        return 0;

    {
        SIZE_T len = (SIZE_T)strlen(full) + 1;
        SIZE_T written = 0;
        HANDLE thread;

        remote = VirtualAllocEx(target, NULL, len, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
        if (!remote)
        {
            DWORD code = GetLastError();
            if (err && err_cap)
                snprintf(err, err_cap, "rr_stage_and_load: VirtualAllocEx failed (%lu)",
                         (unsigned long)code);
            return 0;
        }

        if (!WriteProcessMemory(target, remote, full, len, &written) || written != len)
        {
            DWORD code = GetLastError();
            VirtualFreeEx(target, remote, 0, MEM_RELEASE);
            if (err && err_cap)
                snprintf(err, err_cap, "rr_stage_and_load: WriteProcessMemory failed (%lu)",
                         (unsigned long)code);
            return 0;
        }

        thread = CreateRemoteThread(target, NULL, 0,
                                    (LPTHREAD_START_ROUTINE)load_library,
                                    remote, 0, NULL);
        if (!thread)
        {
            DWORD code = GetLastError();
            VirtualFreeEx(target, remote, 0, MEM_RELEASE);
            if (err && err_cap)
                snprintf(err, err_cap, "rr_stage_and_load: CreateRemoteThread failed (%lu)",
                         (unsigned long)code);
            return 0;
        }

        {
            DWORD wait = WaitForSingleObject(thread, 30000);

            if (wait == WAIT_OBJECT_0)
            {
                if (GetExitCodeThread(thread, &result) && result != 0)
                    loaded = TRUE;
            }
            else
            {
                /* WAIT_TIMEOUT (or a wait error): the remote thread may still
                 * be executing with `remote` as its argument. Freeing the page
                 * now would be a use-after-free inside the target, so the
                 * buffer is deliberately abandoned until the process exits. */
                why = "remote thread did not finish within 30s; "
                      "the staged module path was left mapped in the target";
            }

            CloseHandle(thread);

            if (wait == WAIT_OBJECT_0)
                VirtualFreeEx(target, remote, 0, MEM_RELEASE);
            remote = NULL;
        }
    }

    if (!loaded)
    {
        if (err && err_cap)
        {
            if (why)
                snprintf(err, err_cap, "rr_stage_and_load: %s (pid %lu)",
                         why, (unsigned long)pid);
            else
                snprintf(err, err_cap, "rr_stage_and_load: LoadLibraryA(\"%s\") in pid %lu failed",
                         (module_name && *module_name) ? module_name : full,
                         (unsigned long)pid);
        }
        return 0;
    }

    return result;
}

/* Best-effort terminate of a process we started ourselves. Used to avoid
 * leaving an orphaned game behind when a later injection step fails. */
static void rr_terminate(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h)
    {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

DWORD rr_spawn_and_inject_dll(DWORD game_pid, const char* game_path, const char* game_args,
                              DWORD parent_pid, const char* helper_path,
                              const char* injected_name, char* err, size_t err_cap)
{
    char spawn_err[256];
    HANDLE target;
    DWORD pid = game_pid;
    DWORD injected;
    BOOL spawned_here = FALSE;

    if (!helper_path || !*helper_path)
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_and_inject_dll: helper_path is required");
        return 0;
    }

    /* An explicit PID means "attach to that game"; validate it before use. */
    if (pid && !rr_is_running(pid))
    {
        if (err && err_cap)
            snprintf(err, err_cap, "rr_spawn_and_inject_dll: game_pid %lu is not running",
                     (unsigned long)pid);
        return 0;
    }

    if (!pid)
    {
        if (!game_path || !*game_path)
        {
            if (err && err_cap)
                snprintf(err, err_cap, "rr_spawn_and_inject_dll: pass game_path or a live game_pid");
            return 0;
        }

        spawn_err[0] = '\0';
        pid = rr_spawn_with_parent(parent_pid, game_path, game_args,
                                   spawn_err, sizeof(spawn_err));
        if (!pid)
        {
            if (err && err_cap)
                snprintf(err, err_cap, "rr_spawn_and_inject_dll: spawn failed: %s",
                         spawn_err[0] ? spawn_err : "unknown");
            return 0;
        }
        spawned_here = TRUE;
    }

    target = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                         FALSE, pid);
    if (!target)
    {
        DWORD code = GetLastError();

        /* If helper loading fails after this call starts the game, attempt to
         * terminate that child and report failure. Cleanup itself can also fail.
         */
        if (spawned_here)
            rr_terminate(pid);

        if (err && err_cap)
            snprintf(err, err_cap,
                     "rr_spawn_and_inject_dll: OpenProcess(pid %lu) failed (%lu)%s",
                     (unsigned long)pid, (unsigned long)code,
                     spawned_here ? "; the game this call spawned was terminated" : "");
        return 0;
    }

    injected = rr_stage_and_load(target, pid, helper_path, injected_name, err, err_cap);
    CloseHandle(target);

    if (!injected)
    {
        /* Same reasoning as above: do not leave a half-prepared game behind. */
        if (spawned_here)
            rr_terminate(pid);
        return 0;
    }

    if (err && err_cap)
        snprintf(err, err_cap, "ok: pid %lu, module 0x%08lx",
                 (unsigned long)pid, (unsigned long)injected);
    return pid;
}

int rr_hide_my_process(void)
{
    return rr_hide_process() ? 1 : 0;
}
