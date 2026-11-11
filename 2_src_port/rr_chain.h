/* rr_chain.h - Named address expressions loaded from robrun_chain.txt.
 *
 * An expression can start with GameAssembly.dll, the main image, an absolute
 * address, or another symbol. For GameAssembly.dll, the first offset is an
 * RVA (relative virtual address); later offsets add and dereference a pointer.
 * Symbol-based chains add and dereference every offset.
 *
 * These optional expressions support diagnostics. Runtime world discovery
 * is implemented separately in rr_game.c.
 */
#ifndef RR_CHAIN_H
#define RR_CHAIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RR_MAX_SYMBOLS 64
#define RR_MAX_STEPS   8

/* Load robrun_chain.txt from the supplied path and return the symbol count.
 * Zero means that no symbols were loaded; callers may use other discovery paths.
 */
int chain_load(const char* path);

/* Absolute address of a symbol, or 0. Symbols are resolved lazily: a name is
 * evaluated the first time it is asked for, so "world" may be defined before
 * "camera" uses it. */
uint64_t chain_addr(const char* name);

/* 1 when the symbol exists at all (even if it currently resolves to 0). */
int chain_defined(const char* name);

/* Number of symbols loaded, for the status line. */
int chain_count(void);

/* Enumerates loaded symbols (index < chain_count). */
const char* chain_name(int index);
uint64_t    chain_value(int index);

/* Sweeps [obj+start, obj+end) for pointers whose class name matches
 * want_class. Fills hits[] and returns the number found (up to max).
 * This is the discovery primitive behind --probe: it answers "which field of
 * this object points at the world / camera / player list?". */
int chain_sweep_ptr(uint64_t obj, uint32_t start, uint32_t end,
                    const char* want_class, uint64_t* hits, int max);

#ifdef __cplusplus
}
#endif

#endif /* RR_CHAIN_H */
