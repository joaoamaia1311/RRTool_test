/* rr_spawn.h - Process creation, optional DLL loading and visibility helpers.
 *
 * Spawning and DLL loading use Win32 handles. rr_hide_self() is separate: it
 * requests process-list changes through rr_mem.c. Each API reports failure
 * through its return value and, where provided, an error buffer.
 */
#ifndef RR_SPAWN_H
#define RR_SPAWN_H

#include <stddef.h>

/* Self-contained: forward the Windows 32-bit word type so this header parses
 * correctly even when it is included before <windows.h> is pulled in (rr_spawn.c
 * includes it first). It is exactly 'unsigned long' on the Windows x64 target the
 * port tree builds for. */
typedef unsigned long DWORD;

#ifdef __cplusplus
extern "C" {
#endif

/* Create a child with a requested parent-process attribute. Return its PID
 * or zero on failure. parent_pid == 0 uses a candidate selected by the helper.
 * An unavailable explicit parent fails the call; an automatic choice may
 * fall back without the attribute and reports that in err.
 *
 * path and args use CP_ACP text; args may be NULL. The child uses this
 * process's environment and working directory. err is an optional diagnostic
 * buffer with capacity err_cap bytes.
 */
DWORD rr_spawn_with_parent(DWORD parent_pid, const char* path, const char* args,
                           char* err, size_t err_cap);

/* Load helper_path into game_pid, or start game_path first if game_pid is
 * zero. Return the game PID on success or zero on failure. Paths and arguments
 * use CP_ACP text; game_args may be NULL. parent_pid selects the parent
 * attribute when creating a child. injected_name labels diagnostics.
 *
 * The helper resolves an absolute DLL path and loads it through a remote
 * thread. It rejects cross-bitness targets and derives the export address
 * from the matching target module. Compatible module versions are assumed.
 * If a later step fails after creating the child, cleanup attempts to
 * terminate it. err is an optional diagnostic buffer of err_cap bytes.
 */
DWORD rr_spawn_and_inject_dll(DWORD game_pid, const char* game_path, const char* game_args,
                              DWORD parent_pid, const char* helper_path,
                              const char* injected_name, char* err, size_t err_cap);

/* Request process hiding through the SmxRing driver and adjust the window
 * style after success. Return 1 on an accepted hide request, otherwise 0.
 * The result does not independently verify visibility in other tools.
 */
int rr_hide_my_process(void);

#ifdef __cplusplus
}
#endif
#endif /* RR_SPAWN_H */
