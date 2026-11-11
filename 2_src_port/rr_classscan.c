/* rr_classscan.c - Read-only search for IL2CPP classes and their instances.
 *
 * The scan has three stages: find class-name strings, find candidate class
 * structures that reference those strings, then find objects that reference
 * the classes. Candidate layouts are checked using name, namespace and
 * self-references in the embedded type descriptors. These checks reduce false
 * matches; they do not establish compatibility with every Unity version.
 *
 * Reads are requested in batches of 64 pages. rr_read_batch() splits requests
 * to fit the driver reply limit. Output goes to the console and classscan.log.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "rr_mem.h"

#define CHUNK     0x1000
#define BATCH     64
#define MAXNAME   4      /* class names per run                       */
#define MAXHIT    10     /* name-literal hits kept per class name     */
#define MAXWANT   32     /* total pointers chased in stage 2          */
#define MAXKLASS  10
#define MAXDUMP   6      /* object dumps per class                    */
#define KLASS_LEN 0x160  /* bytes of Il2CppClass dumped/validated     */
#define PROGRESS  64000  /* chunks between progress lines (~256 MB)   */

/* Logging. */

static FILE* g_lg = NULL;

static void cs_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_lg) {
        va_start(ap, fmt);
        vfprintf(g_lg, fmt, ap);
        va_end(ap);
        fflush(g_lg);
    }
}

static void hexdump(uint64_t base, const uint8_t* p, unsigned len) {
    char line[128];
    unsigned i;
    for (i = 0; i < len; i += 16) {
        unsigned n = len - i, j;
        int k = 0;
        if (n > 16) n = 16;
        for (j = 0; j < 16; j++) {
            if (j < n) k += sprintf(line + k, "%02X ", p[i + j]);
            else       k += sprintf(line + k, "   ");
        }
        k += sprintf(line + k, " |");
        for (j = 0; j < n; j++) {
            unsigned char c = p[i + j];
            line[k++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[k] = 0;
        cs_log("  %016llX  %s|\n", (unsigned long long)(base + i), line);
    }
}

/* Read a NUL-terminated printable ASCII identifier. Return its length,
 * or zero if the read fails or the bytes are not a plausible identifier.
 */
static int read_cstr(uint64_t addr, char* out, int cap) {
    uint8_t buf[64];
    int n;
    if (cap < 2) { if (cap > 0) out[0] = 0; return 0; }
    out[0] = 0;
    if (!addr) return 0;
    if (!rr_read(addr, buf, sizeof buf)) return 0;
    for (n = 0; n < (int)sizeof buf && n < cap - 1; n++) {
        if (!buf[n]) break;
        if (buf[n] < 32 || buf[n] > 126) { out[0] = 0; return 0; }
        out[n] = (char)buf[n];
    }
    out[n] = 0;
    return n;
}

/* Region walk. */

/* Called for each committed readable 4 KB page, and once per page boundary for
 * a small bridge buffer so a literal spanning two pages is still found.
 * Return non-zero to stop the walk. */
typedef int (*CHUNK_FN)(uint64_t addr, const uint8_t* p, unsigned len, void* u);

/* Count batches with no successful reads. Missing data makes a negative
 * search result inconclusive.
 */
static long g_bad_batches = 0;

static int region_usable(const RR_REGION* rg, int allow_mapped, int allow_image) {
    uint32_t pr;
    if (rg->state != 0x1000) return 0;                 /* MEM_COMMIT          */
    if (rg->type == 0x20000) { /* MEM_PRIVATE region. */ }
    else if (rg->type == 0x40000 && allow_mapped) { }
    else if (rg->type == 0x1000000 && allow_image) { }
    else return 0;
    pr = rg->protect & 0xFF;
    if (pr == 0) return 0;
    if (pr & 0x01) return 0;                           /* PAGE_NOACCESS       */
    if (pr & 0x100) return 0;                          /* PAGE_GUARD          */
    return 1;
}

static long walk_mem(int allow_mapped, int allow_image, CHUNK_FN fn, void* u,
                     const char* label) {
    uint64_t addr = 0x10000;
    const uint64_t limit = 0x7FFFFFFEFFFFull;
    uint8_t* pool = (uint8_t*)malloc((size_t)BATCH * CHUNK);
    static uint8_t bridge[2 * CHUNK];
    long chunks = 0;
    if (!pool) return -1;

    while (addr < limit) {
        RR_REGION rg;
        if (!rr_query_region(addr, &rg) || !rg.size) { addr += 0x10000; continue; }
        {
            uint64_t start = rg.base < 0x10000 ? 0x10000 : rg.base;
            uint64_t end = rg.base + rg.size;
            uint64_t q;

            if (region_usable(&rg, allow_mapped, allow_image)) {
                for (q = start; q < end; q += (uint64_t)BATCH * CHUNK) {
                    RR_RD_REQ req[BATCH];
                    int n = 0, i;
                    for (i = 0; i < BATCH && q + (uint64_t)i * CHUNK < end; i++) {
                        uint64_t a = q + (uint64_t)i * CHUNK;
                        unsigned sz = (unsigned)((end - a) < CHUNK ? (end - a) : CHUNK);
                        req[n].addr = a;
                        req[n].size = sz;
                        req[n].dst = pool + (size_t)n * CHUNK;
                        /* Zero first: a page that fails to read must not be
                         * mistaken for the previous page's bytes. */
                        memset(req[n].dst, 0, sz);
                        n++;
                    }
                    if (!n) break;
                    if (rr_read_batch(req, n) <= 0) g_bad_batches++;
                    for (i = 0; i < n; i++) {
                        int stop;
                        if (!req[i].size) continue;
                        stop = fn(req[i].addr, (const uint8_t*)req[i].dst, req[i].size, u);
                        if (!stop && i + 1 < n &&
                            req[i + 1].addr == req[i].addr + req[i].size) {
                            unsigned t = 48, h;
                            if (t > req[i].size) t = req[i].size;
                            h = (t < req[i + 1].size) ? t : req[i + 1].size;
                            memcpy(bridge, (const uint8_t*)req[i].dst + req[i].size - t, t);
                            memcpy(bridge + t, req[i + 1].dst, h);
                            stop = fn(req[i].addr + req[i].size - t, bridge, t + h, u);
                        }
                        if (stop) { free(pool); return chunks; }
                    }
                    chunks += n;
                    if (label && (chunks % PROGRESS) < BATCH)
                        cs_log("[%s] %ld MB read...\n", label, (long)((chunks * (long)CHUNK) >> 20));
                }
            }
            addr = (end > addr) ? end : addr + 0x10000;
        }
    }
    free(pool);
    return chunks;
}

/* Stage 1: class-name literals. */

typedef struct NAMESPEC {
    char     text[96];
    unsigned len;
    uint64_t hits[MAXHIT];
    int      nhits;
    int      klasses;
    int      full;
} NAMESPEC;

static NAMESPEC g_name[MAXNAME];
static int      g_nname;

static void name_note_hit(NAMESPEC* ns, uint64_t a) {
    int k;
    if (ns->full) return;
    for (k = 0; k < ns->nhits; k++) if (ns->hits[k] == a) return;
    ns->hits[ns->nhits++] = a;
    if (ns->nhits >= MAXHIT) ns->full = 1;
}

static int cb_find_names(uint64_t base, const uint8_t* p, unsigned len, void* u) {
    int i, alive = 0;
    (void)u;
    for (i = 0; i < g_nname; i++) if (!g_name[i].full) alive = 1;
    if (!alive) return 1;

    for (i = 0; i < g_nname; i++) {
        NAMESPEC* ns = &g_name[i];
        unsigned n = ns->len, o = 0;
        if (ns->full || !n || len < n) continue;
        while (o + n <= len) {
            const uint8_t* f = (const uint8_t*)memchr(p + o, (unsigned char)ns->text[0],
                                                      len - n + 1 - o);
            if (!f) break;
            o = (unsigned)(f - p);
            if (memcmp(p + o, ns->text, n) == 0) name_note_hit(ns, base + o);
            o++;
        }
    }
    return 0;
}

/* Print the full string around each match to help distinguish an exact
 * class name from a partial match inside another name.
 */
static void report_names(long chunks) {
    int i, k;
    for (i = 0; i < g_nname; i++) {
        cs_log("[stage1] \"%s\": %d literal(s)\n", g_name[i].text, g_name[i].nhits);
        for (k = 0; k < g_name[i].nhits; k++) {
            char str[80];
            if (!read_cstr(g_name[i].hits[k], str, (int)sizeof str)) str[0] = 0;
            cs_log("[stage1]   %016llX  \"%s\"\n",
                   (unsigned long long)g_name[i].hits[k], str);
        }
        if (!g_name[i].nhits)
            cs_log("[stage1]   none - check spelling/case; metadata holds the C# name\n");
    }
    cs_log("[stage1] %ld chunks\n", chunks);
}

static int any_hits(void) {
    int i;
    for (i = 0; i < g_nname; i++) if (g_name[i].nhits) return 1;
    return 0;
}

/* Stage 2: Il2CppClass identification. */
typedef struct WANT { uint64_t val; int owner; int done; } WANT;

static WANT g_want[MAXWANT];
static int  g_nwant;
static int  g_unaligned;

typedef struct KRES {
    uint64_t addr;        /* Il2CppClass* */
    int      owner;
    char     parent[64];
    char     ns[64];
    long     instances;
    int      dumped;
    uint64_t sample[MAXDUMP];
} KRES;

static KRES g_kres[MAXKLASS];
static int  g_nkres;

static int klass_known(uint64_t a) {
    int i;
    for (i = 0; i < g_nkres; i++) if (g_kres[i].addr == a) return 1;
    return 0;
}

static void wants_done_for(int owner) {
    int w;
    for (w = 0; w < g_nwant; w++) if (g_want[w].owner == owner) g_want[w].done = 1;
}

/* Verify one candidate. `at` is where a qword equal to `nameval` was found. */
static void try_klass(uint64_t at, uint64_t nameval, int owner) {
    static const uint64_t name_off[3] = { 0x10, 0x08, 0x18 };
    static uint8_t buf[KLASS_LEN];
    int t;

    for (t = 0; t < 3; t++) {
        uint64_t noff = name_off[t], cand, self, parent, nsptr, pn;
        int shift = (int)noff - 0x10;
        char ns[64], pname[64];
        KRES* kr;

        if (at < noff) continue;
        cand = at - noff;
        if (klass_known(cand)) return;
        if (g_nkres >= MAXKLASS) return;

        /* For the layouts checked here, the embedded byval_arg data pointer
         * should refer back to the candidate class.
         */
        if (!rr_read(cand + noff + 0x10, &self, sizeof self)) continue;
        if (self != cand) continue;

        if (!rr_read(cand, buf, sizeof buf)) continue;

        ns[0] = 0;
        nsptr = 0;
        memcpy(&nsptr, buf + 0x18 + shift, 8);      /* Namespace pointer. */
        if (nsptr) read_cstr(nsptr, ns, (int)sizeof ns);

        parent = 0;
        pname[0] = 0;
        memcpy(&parent, buf + 0x58 + shift, 8);     /* Il2CppClass* parent */
        if (parent > 0x10000 && parent < 0x7FFFFFFFFFFFFFFFull) {
            pn = 0;
            if (rr_read(parent + noff, &pn, sizeof pn) && pn) read_cstr(pn, pname, (int)sizeof pname);
        }

        memset(&g_kres[g_nkres], 0, sizeof g_kres[g_nkres]);
        kr = &g_kres[g_nkres];
        kr->addr = cand;
        kr->owner = owner;
        if (ns[0]) strncpy(kr->ns, ns, sizeof kr->ns - 1);
        if (pname[0]) strncpy(kr->parent, pname, sizeof kr->parent - 1);
        g_nkres++;
        g_name[owner].klasses++;
        wants_done_for(owner);

        cs_log("[stage2] KLASS %016llX  %s%s%s  parent=%s\n",
               (unsigned long long)cand, ns[0] ? ns : "", ns[0] ? "." : "",
               g_name[owner].text, pname[0] ? pname : "?");
        cs_log("[stage2]   name_offset=0x%02llX  parent=0x%016llX\n",
               (unsigned long long)noff, (unsigned long long)parent);
        /* Leave instance_size for manual inspection of the dump because its
         * offset depends on the runtime layout.
         */
        cs_log("[stage2]   struct (%u bytes):\n", (unsigned)KLASS_LEN);
        hexdump(cand, buf, KLASS_LEN);
        return;
    }
}

static int cb_find_ptr(uint64_t base, const uint8_t* p, unsigned len, void* u) {
    int step = g_unaligned ? 1 : 8;
    int live = 0, w;
    unsigned o;
    (void)u;
    for (w = 0; w < g_nwant; w++) if (!g_want[w].done) live = 1;
    if (!live) return 1;

    for (o = 0; (int)(o + 8) <= (int)len; o += (unsigned)step) {
        uint64_t v;
        memcpy(&v, p + o, 8);
        if (!v) continue;
        for (w = 0; w < g_nwant; w++) {
            if (g_want[w].done) continue;
            if (v == g_want[w].val) try_klass(base + o, v, g_want[w].owner);
        }
    }
    return 0;
}

/* Stage 3: live instances. */

static int cb_find_obj(uint64_t base, const uint8_t* p, unsigned len, void* u) {
    unsigned o;
    int k;
    (void)u;
    for (o = 0; (int)(o + 8) <= (int)len; o += 8) {
        uint64_t v;
        memcpy(&v, p + o, 8);
        if (!v) continue;
        for (k = 0; k < g_nkres; k++) {
            uint64_t obj;
            if (v != g_kres[k].addr) continue;
            obj = base + o;
            /* byval_arg and this_arg live inside the struct and point at the
             * same address, so the klass struct always self-matches. */
            if (obj >= g_kres[k].addr && obj < g_kres[k].addr + 0x400) break;
            g_kres[k].instances++;
            if (g_kres[k].dumped < MAXDUMP) {
                static uint8_t ob[0x80];
                g_kres[k].sample[g_kres[k].dumped] = obj;
                cs_log("[stage3] %s instance %016llX\n",
                       g_name[g_kres[k].owner].text, (unsigned long long)obj);
                if (rr_read(obj, ob, sizeof ob)) hexdump(obj, ob, sizeof ob);
                else cs_log("  (unreadable)\n");
                g_kres[k].dumped++;
            }
            break;
        }
    }
    return 0;
}

/* Entry. */

int run_classscan(const char* text) {
    char list[MAXNAME][96];
    const char* p;
    int nlist = 0, i, unresolved, rc = 1;
    long chunks;
    ULONGLONG t0, t1;

    /* Parse the comma-separated names before allocating a console. */
    p = text ? text : "";
    while (*p && nlist < MAXNAME) {
        int n = 0;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        while (*p && *p != ',' && n < 95) list[nlist][n++] = *p++;
        list[nlist][n] = 0;
        while (n > 0 && list[nlist][n - 1] == ' ') list[nlist][--n] = 0;
        if (n > 0) nlist++;
        while (*p && *p != ',') p++;
    }
    if (!nlist) return 2;

    /* GUI-subsystem binary: borrow the console so progress is visible live. */
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    freopen("CONOUT$", "w", stdout);
    g_lg = fopen("classscan.log", "w");
    t0 = GetTickCount64();

    cs_log("RobRunESP class scan\n");
    for (i = 0; i < nlist; i++) {
        memset(&g_name[i], 0, sizeof g_name[i]);
        strncpy(g_name[i].text, list[i], sizeof g_name[i].text - 1);
        g_name[i].len = (unsigned)strlen(g_name[i].text);
        cs_log("[scan] class name \"%s\"\n", g_name[i].text);
    }
    g_nname = nlist;

    if (!rr_drv_open()) {
        cs_log("[scan] driver unavailable - map the kernel driver first\n");
        goto out;
    }
    if (!rr_attach("ROB RUN.exe")) {
        cs_log("[scan] game not running\n");
        rc = 3;
        goto out;
    }

    /* Stage 1: one pass, all names. */
    cs_log("\n[stage1] searching committed memory for %d class name(s)\n", nlist);
    chunks = walk_mem(1, 1, cb_find_names, NULL, "stage1");
    if (chunks < 0) { cs_log("[stage1] out of memory\n"); goto out; }
    report_names(chunks);

    /* If no exact name matches, try prefixes to show related names that may
     * help diagnose a renamed type or an incorrect search term.
     */
    if (!any_hits()) {
        cs_log("\n[stage1] no exact literal - retrying with a %d-character prefix\n", 12);
        for (i = 0; i < nlist; i++) {
            g_name[i].nhits = 0;
            g_name[i].full = 0;
            if (g_name[i].len > 12) { g_name[i].len = 12; g_name[i].text[12] = 0; }
            cs_log("[stage1]   prefix \"%s\"\n", g_name[i].text);
        }
        chunks = walk_mem(1, 1, cb_find_names, NULL, "stage1b");
        if (chunks < 0) { cs_log("[stage1b] out of memory\n"); goto out; }
        report_names(chunks);
    }

    /* Stage 2. */
    /* Round-robin over the names so one class that appears many times cannot
     * starve the others out of the budget. */
    g_nwant = 0;
    {
        int k;
        for (k = 0; k < MAXHIT && g_nwant < MAXWANT; k++) {
            for (i = 0; i < nlist && g_nwant < MAXWANT; i++) {
                if (k >= g_name[i].nhits) continue;
                g_want[g_nwant].val = g_name[i].hits[k];
                g_want[g_nwant].owner = i;
                g_want[g_nwant].done = 0;
                g_nwant++;
            }
        }
    }
    if (!g_nwant) { cs_log("\n[stage2] no literals to chase - stopping\n"); goto summary; }

    cs_log("\n[stage2] chasing %d name pointer(s) in private memory\n", g_nwant);
    g_unaligned = 0;
    chunks = walk_mem(0, 0, cb_find_ptr, NULL, "stage2");
    if (chunks < 0) { cs_log("[stage2] out of memory\n"); goto out; }
    unresolved = 0;
    for (i = 0; i < nlist; i++) if (!g_name[i].klasses) unresolved++;
    cs_log("[stage2] %ld chunks, %d class struct(s), %d name(s) unresolved\n",
           chunks, g_nkres, unresolved);

    if (g_nkres == 0) {
        /* Retry at byte alignment after the aligned pass fails. A miss can also
         * mean an unreadable region or an unsupported layout.
         */
        cs_log("[stage2] retrying at every byte offset\n");
        g_unaligned = 1;
        chunks = walk_mem(0, 0, cb_find_ptr, NULL, "stage2b");
        cs_log("[stage2b] %ld chunks, %d class struct(s)\n", chunks, g_nkres);
    }
    if (!g_nkres) { cs_log("\n[stage2] FAILED: no Il2CppClass matched any literal\n"); goto summary; }

    /* Stage 3. */
    cs_log("\n[stage3] looking for objects whose klass pointer is one of %d\n", g_nkres);
    chunks = walk_mem(0, 0, cb_find_obj, NULL, "stage3");
    cs_log("[stage3] %ld chunks\n", chunks);

summary:
    cs_log("\n==== summary ====\n");
    for (i = 0; i < nlist; i++) {
        int k;
        cs_log("%-36s literals=%d structs=%d\n", g_name[i].text,
               g_name[i].nhits, g_name[i].klasses);
        for (k = 0; k < g_nkres; k++) {
            long j;
            if (g_kres[k].owner != i) continue;
            cs_log("   klass %016llX  instances=%ld\n",
                   (unsigned long long)g_kres[k].addr, g_kres[k].instances);
            for (j = 0; j < g_kres[k].dumped && j < MAXDUMP; j++)
                cs_log("     obj %016llX\n", (unsigned long long)g_kres[k].sample[j]);
        }
    }
    rc = g_nkres ? 0 : 1;

out:
    if (g_bad_batches)
        cs_log("\n[scan] WARNING: %ld batches returned no data - any 'no hits' above is void\n",
               g_bad_batches);
    t1 = GetTickCount64();
    cs_log("done in %lu s\n", (unsigned long)((t1 - t0) / 1000));
    if (g_lg) { fclose(g_lg); g_lg = NULL; }
    return rc;
}
