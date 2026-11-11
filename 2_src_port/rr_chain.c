/* rr_chain.c - anchor file parser and pointer sweep (see rr_chain.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "rr_mem.h"
#include "rr_unity.h"
#include "rr_chain.h"

typedef struct {
    char     name[48];
    uint32_t base_kind;    /* 0 GameAssembly.dll, 1 main image, 2 absolute address, 3 named symbol */
    char     base_sym[48];
    uint64_t base;         /* module RVA or absolute address             */
    uint32_t steps[RR_MAX_STEPS];
    int      nsteps;
    int      tried;
    uint64_t value;
} SYM;

static SYM g_sym[RR_MAX_SYMBOLS];
static int g_nsym = 0;

static void trim(char* s) {
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int find_sym(const char* n) {
    for (int i = 0; i < g_nsym; i++)
        if (!_stricmp(g_sym[i].name, n)) return i;
    return -1;
}

int chain_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && g_nsym < RR_MAX_SYMBOLS) {
        char* hash = strchr(line, '#');           /* Discard the trailing # comment, if present. */
        if (hash) *hash = 0;
        char* semi = strchr(line, ';');
        if (semi) *semi = 0;
        trim(line);
        if (!line[0]) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[48];
        snprintf(name, sizeof(name), "%s", line);
        trim(name);

        char* expr = eq + 1;
        trim(expr);

        SYM s;
        memset(&s, 0, sizeof(s));
        snprintf(s.name, sizeof(s.name), "%s", name);

        /* Token: up to the next '+' */
        char token[64];
        int ti = 0;
        char* p = expr;
        while (*p && *p != '+' && ti < (int)sizeof(token) - 1) token[ti++] = *p++;
        token[ti] = 0;
        trim(token);

        if (!_stricmp(token, "GameAssembly.dll")) {
            s.base_kind = 0;
        } else if (!_stricmp(token, "ROB RUN.exe") || !_stricmp(token, "main")) {
            s.base_kind = 1;
            s.base = 0;   /* Resolve the main image base when the expression is evaluated. */
        } else if (isdigit((unsigned char)token[0]) || (token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))) {
            s.base_kind = 2;
            s.base = strtoull(token, NULL, 0);
        } else {
            s.base_kind = 3;
            snprintf(s.base_sym, sizeof(s.base_sym), "%s", token);
        }

        /* Parse the remaining +offset steps. Only the first offset after
         * GameAssembly.dll is treated as an RVA; other steps add and dereference.
         */
        while (*p == '+') {
            p++;
            char* end = NULL;
            unsigned long long v = strtoull(p, &end, 0);
            if (end == p) break;
            p = end;
            while (*p && isspace((unsigned char)*p)) p++;
            if (s.nsteps < RR_MAX_STEPS) s.steps[s.nsteps++] = (uint32_t)v;
        }

        int idx = find_sym(name);
        if (idx >= 0) g_sym[idx] = s;
        else g_sym[g_nsym++] = s;
    }
    fclose(f);
    return g_nsym;
}

static uint64_t eval(int idx, int depth);

static uint64_t eval(int idx, int depth) {
    if (idx < 0 || idx >= g_nsym || depth > 8) return 0;
    SYM* s = &g_sym[idx];
    if (s->tried) return s->value;
    s->tried = 1;
    s->value = 0;

    uint64_t p = 0;
    switch (s->base_kind) {
    case 0: {   /* module + first step is an RVA, not a deref */
        uint64_t mod = 0;
        if (!rr_module("GameAssembly.dll", &mod, NULL) || !mod) return (s->value = 0);
        p = mod;
        if (s->nsteps > 0) {
            p += s->steps[0];
            for (int i = 1; i < s->nsteps; i++) {
                uint64_t v = rr_u64(p + s->steps[i]);
                if (!PL(v)) return (s->value = 0);
                p = v;
            }
            return (s->value = p);
        }
        return (s->value = p);
    }
    case 1:
        p = rr_image_base();
        if (!p) return (s->value = 0);
        for (int i = 0; i < s->nsteps; i++) {
            uint64_t v = rr_u64(p + s->steps[i]);
            if (!PL(v)) return (s->value = 0);
            p = v;
        }
        return (s->value = p);
    case 2:
        p = s->base;
        for (int i = 0; i < s->nsteps; i++) {
            uint64_t v = rr_u64(p + s->steps[i]);
            if (!PL(v)) return (s->value = 0);
            p = v;
        }
        return (s->value = p);
    default: {
        int bi = find_sym(s->base_sym);
        if (bi < 0) return (s->value = 0);
        p = eval(bi, depth + 1);
        if (!PL(p)) return (s->value = 0);
        for (int i = 0; i < s->nsteps; i++) {
            uint64_t v = rr_u64(p + s->steps[i]);
            if (!PL(v)) return (s->value = 0);
            p = v;
        }
        return (s->value = p);
    }
    }
}

uint64_t chain_addr(const char* name) {
    int i = find_sym(name);
    return i < 0 ? 0 : eval(i, 0);
}

int chain_defined(const char* name) { return find_sym(name) >= 0; }

int chain_count(void) { return g_nsym; }

const char* chain_name(int i) { return (i >= 0 && i < g_nsym) ? g_sym[i].name : ""; }

uint64_t chain_value(int i) { return (i >= 0 && i < g_nsym) ? eval(i, 0) : 0; }

int chain_sweep_ptr(uint64_t obj, uint32_t start, uint32_t end,
                    const char* want_class, uint64_t* hits, int max) {
    int n = 0;
    if (!PL(obj) || !want_class || max <= 0) return 0;
    for (uint32_t off = start; off + 8 <= end; off += 8) {
        uint64_t v = rr_u64(obj + off);
        if (!PL(v)) continue;
        if (u_is(v, want_class)) {
            if (n < max) hits[n] = v;
            n++;
            if (n >= max) break;
        }
    }
    return n;
}
