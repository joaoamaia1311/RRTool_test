/* rr_unity.c - IL2CPP object helpers (see rr_unity.h for the layout map). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "rr_mem.h"
#include "rr_unity.h"

int u_valid(uint64_t p) { return PL(p) && (p & 7) == 0 ? 1 : PL(p); }

uint64_t u_klass(uint64_t obj) {
    if (!PL(obj)) return 0;
    uint64_t k = rr_u64(obj);
    return PL(k) ? k : 0;
}

int u_class_name(uint64_t klass, char* out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!PL(klass)) return 0;
    uint64_t namep = rr_u64(klass + U_CLASS_NAME);
    if (!PL(namep)) return 0;
    /* Class names are NUL-terminated ASCII in the metadata, but a short name
     * can sit at the very end of a committed page: a 160-byte read then fails
     * with STATUS_PARTIAL_COPY even though the string is fine. Retry with
     * shorter windows instead of pretending the name is unreadable. */
    static const size_t tries[] = { 160, 96, 64, 32, 16, 8 };
    char tmp[160];
    size_t got = 0;
    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
        size_t want = tries[t];
        if (want > sizeof(tmp)) want = sizeof(tmp);
        if (rr_read(namep, tmp, want)) { got = 0; break; }
        got = 1;
    }
    if (got) return 0;                 /* every window failed */
    tmp[sizeof(tmp) - 1] = 0;
    size_t i = 0;
    while (i + 1 < sizeof(tmp) && i + 1 < n && tmp[i]) { out[i] = tmp[i]; i++; }
    out[i] = 0;
    return i > 0;
}

int u_is(uint64_t obj, const char* qualified) {
    uint64_t k = u_klass(obj);
    if (!k || !qualified) return 0;
    char name[160];
    if (!u_class_name(k, name, sizeof(name))) return 0;

    const char* dot = strrchr(qualified, '.');
    /* accept both "Type" and "Namespace.Type" (the dump's namespaces are not
     * always what the runtime reports for nested/global types) */
    if (dot) {
        const char* iface = strstr(qualified, ".");
        (void)iface;
        if (!strcmp(name, dot + 1)) return 1;
    }
    if (!strcmp(name, qualified)) return 1;
    return 0;
}

uint64_t u_static_fields(uint64_t klass) {
    if (!PL(klass)) return 0;
    uint64_t sf = rr_u64(klass + U_CLASS_STATIC);
    return PL(sf) ? sf : 0;
}

int u_string(uint64_t str, char* out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!PL(str)) return 0;
    int32_t len = rr_i32(str + U_STR_LEN);
    if (len <= 0 || len > 4096) return 0;
    wchar_t wbuf[1025];
    if (len > 1024) len = 1024;
    if (!rr_read(str + U_STR_CHARS, wbuf, (size_t)len * 2)) return 0;
    wbuf[len] = 0;
    int written = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, out, (int)n - 1, NULL, NULL);
    if (written <= 0) { out[0] = 0; return 0; }
    out[written < (int)n - 1 ? written : (int)n - 1] = 0;
    return written;
}

uint64_t u_chain(uint64_t base, const uint32_t* offsets, int count) {
    uint64_t p = base;
    for (int i = 0; i < count; i++) {
        if (!PL(p)) return 0;
        uint64_t v = rr_u64(p + offsets[i]);
        if (!PL(v)) return 0;
        p = v;
    }
    return p;
}

int u_list(uint64_t list, uint64_t* data) {
    if (data) *data = 0;
    if (!PL(list)) return 0;
    uint64_t items = rr_u64(list + U_LIST_ITEMS);
    int32_t size = rr_i32(list + U_LIST_SIZE);
    if (!PL(items) || size <= 0 || size > 100000) return 0;
    if (data) *data = items + U_ARR_DATA;
    return size;
}

int u_array(uint64_t arr, uint64_t* data) {
    if (data) *data = 0;
    if (!PL(arr)) return 0;
    int32_t len = rr_i32(arr + U_ARR_LEN);
    if (len <= 0 || len > 100000) return 0;
    if (data) *data = arr + U_ARR_DATA;
    return len;
}

int u_dict(uint64_t dict, int* count, uint64_t* entries, int* stride_hint) {
    if (count) *count = 0;
    if (entries) *entries = 0;
    if (!PL(dict)) return 0;
    int32_t n = rr_i32(dict + U_DICT_COUNT);
    uint64_t ent = rr_u64(dict + U_DICT_ENTRIES);
    if (n <= 0 || n > 100000 || !PL(ent)) return 0;
    if (count) *count = n;
    if (entries) *entries = ent + U_ARR_DATA;
    if (stride_hint) *stride_hint = 0x18;   /* Dictionary<int, TRef> in IL2CPP */
    return n;
}

int u_dict_value_offset(uint64_t dict, const char* want_class, int samples) {
    int n = 0;
    uint64_t ent = 0;
    if (!u_dict(dict, &n, &ent, NULL)) return -1;
    if (n > samples) n = samples;

    /* Candidate entry strides for a Dictionary<int, T>: with a 4-byte key the
     * value is 8-byte aligned, so (4+4+4+pad+8) = 0x18, and for a struct
     * payload it can grow. Probe the plausible ones. */
    static const int strides[] = { 0x18, 0x20, 0x10, 0x28 };
    for (int s = 0; s < (int)(sizeof(strides) / sizeof(strides[0])); s++) {
        int stride = strides[s];
        for (uint32_t voff = 8; voff + 8 <= (uint32_t)stride; voff += 8) {
            int good = 0, tested = 0;
            for (int i = 0; i < n; i++) {
                uint64_t e = ent + (uint64_t)i * stride;
                uint64_t v = rr_u64(e + voff);
                if (!PL(v)) continue;
                tested++;
                if (u_is(v, want_class)) good++;
            }
            if (tested >= (n < 3 ? n : 3) && good == tested && good > 0)
                return (int)voff;
        }
    }
    return -1;
}

int u_find_float(uint64_t obj, uint32_t start, uint32_t end, float lo, float hi) {
    if (!PL(obj)) return -1;
    for (uint32_t off = start; off + 4 <= end; off += 4) {
        float v = rr_f32(obj + off);
        if (!rr_ok()) continue;
        if (v >= lo && v <= hi) return (int)off;
    }
    return -1;
}
