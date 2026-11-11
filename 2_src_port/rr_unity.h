/* rr_unity.h - Managed-object helpers for the supported Unity IL2CPP layout.
 *
 * Object: class pointer at +0x00, monitor at +0x08, instance fields at +0x10.
 * String: length at +0x10, UTF-16 characters at +0x14.
 * Array: length at +0x18, elements at +0x20.
 * List: backing array at +0x10, count at +0x18.
 * Dictionary: buckets at +0x10, entries at +0x18, used count at +0x20.
 *
 * Class-layout constants below describe the runtime used for this snapshot.
 * They are not a compatibility guarantee for other Unity builds. Managed
 * game-field offsets are maintained separately in the generated headers.
 */
#ifndef RR_UNITY_H
#define RR_UNITY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_OBJ_HEADER        0x10u
#define U_CLASS_NAME        0x10u
#define U_CLASS_NS          0x18u
#define U_CLASS_STATIC      0xB8u
#define U_CLASS_GENCONT     0xE8u
#define U_CLASS_INST_SIZE   0xF0u
#define U_STR_LEN           0x10u
#define U_STR_CHARS         0x14u
#define U_ARR_LEN           0x18u
#define U_ARR_DATA          0x20u
#define U_LIST_ITEMS        0x10u
#define U_LIST_SIZE         0x18u
#define U_DICT_BUCKETS      0x10u
#define U_DICT_ENTRIES      0x18u
#define U_DICT_COUNT        0x20u

#define PL(v) ((v) >= 0x10000ULL && (v) < 0x00007FFFFFFFFFFFULL)

/* Return 1 when p passes the managed-object pointer checks. */
int u_valid(uint64_t p);

/* Il2CppClass* of an object. */
uint64_t u_klass(uint64_t obj);

/* "Namespace.Type" (or just "Type" when the namespace is empty). 0 on a bad
 * read. The result is a C string in out. */
int u_class_name(uint64_t klass, char* out, size_t n);

/* Compares an object's class against "Namespace.Type" (namespace optional). */
int u_is(uint64_t obj, const char* qualified);

/* static_fields blob of a class pointer (0 when the class has none). */
uint64_t u_static_fields(uint64_t klass);

/* Convert an IL2CPP UTF-16 string to UTF-8 and NUL-terminate out. Return
 * the bytes written, or zero for an empty, invalid or unreadable string.
 */
int u_string(uint64_t str, char* out, size_t n);

/* For each offset, read the pointer at current + offset and validate it.
 * For offsets {0x38, 0x18, 0x10}, the result is
 * *(*(*(base + 0x38) + 0x18) + 0x10). Return zero if a step fails.
 */
uint64_t u_chain(uint64_t base, const uint32_t* offsets, int count);

/* List<T> / array iteration helpers. Return the element count and fill
 * *data with the address of element 0. */
int u_list(uint64_t list, uint64_t* data);
int u_array(uint64_t arr, uint64_t* data);

/* Read a dictionary used-slot count and the first-entry address.
 * stride_hint receives 0x18 for the supported int/reference layout. Other key
 * or value types require their own verified stride and field offsets.
 */
int u_dict(uint64_t dict, int* count, uint64_t* entries, int* stride_hint);

/* Auto-detection used by --probe: walk the first `samples` entries and report
 * the offset inside an entry that holds a pointer to an object whose class
 * name matches want_class. Returns the offset or -1. */
int u_dict_value_offset(uint64_t dict, const char* want_class, int samples);

/* First float in [lo,hi] found at obj+start .. obj+end (step 4). Probe aid. */
int u_find_float(uint64_t obj, uint32_t start, uint32_t end, float lo, float hi);

#ifdef __cplusplus
}
#endif

#endif /* RR_UNITY_H */
