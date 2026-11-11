/* Node/prefab name scan: finds a UTF-16 GameObject name in the game heap.
 * Batch reads: 64 x 4 KB pages per IOCTL. Read-only.
 *
 * Writes to BOTH stdout and nodescan.log, because this exe is a GUI-subsystem
 * binary: plain printf reaches no console when launched from a .bat. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "rr_mem.h"

#define CHUNK 0x1000
#define BATCH 64

static FILE* g_lg = NULL;

static void scan_log(const char* fmt, ...) {
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

int run_nodescan(const char* text) {
    wchar_t pat[128];
    int plen = 0;
    while (text[plen] && plen < 120) { pat[plen] = (wchar_t)(unsigned char)text[plen]; plen++; }
    if (!plen) return 2;
    unsigned pb = (unsigned)plen * 2;

    g_lg = fopen("nodescan.log", "w");
    scan_log("RobRunESP node scan\n");

    if (!rr_drv_open()) {
        scan_log("[scan] driver unavailable - map the kernel driver first\n");
        if (g_lg) { fclose(g_lg); g_lg = NULL; }
        return 4;
    }
    if (!rr_attach("ROB RUN.exe")) {
        scan_log("[scan] game not running\n");
        if (g_lg) { fclose(g_lg); g_lg = NULL; }
        return 3;
    }

    uint8_t* pool = (uint8_t*)malloc((size_t)BATCH * CHUNK);
    if (!pool) return 5;

    uint64_t addr = 0x10000, limit = 0x7FFFFFFEFFFFull;
    int hits = 0, regions = 0;
    long chunks = 0, bad = 0;
    scan_log("[scan] looking for %s\n", text);

    while (addr < limit && hits < 200) {
        RR_REGION rg;
        if (!rr_query_region(addr, &rg) || rg.size == 0) { addr += 0x10000; continue; }

        uint64_t start = rg.base < 0x10000 ? 0x10000 : rg.base;
        uint64_t end = rg.base + rg.size;
        uint32_t pr = rg.protect & 0xFF;
        int usable = (rg.state == 0x1000) && (rg.type == 0x20000 || rg.type == 0x40000) &&
                     (pr == 0x02 || pr == 0x04 || pr == 0x08 || pr == 0x10 ||
                      pr == 0x20 || pr == 0x40 || pr == 0x80);
        if (usable) regions++;

        for (uint64_t q = start; usable && q < end; q += (uint64_t)BATCH * CHUNK) {
            RR_RD_REQ req[BATCH];
            int n = 0;
            for (int i = 0; i < BATCH && q + (uint64_t)i * CHUNK < end; i++) {
                req[n].addr = q + (uint64_t)i * CHUNK;
                req[n].size = CHUNK;
                req[n].dst = pool + (size_t)i * CHUNK;
                n++;
            }
            if (!n) break;
            if (rr_read_batch(req, n) <= 0) bad++;
            for (int i = 0; i < n && hits < 200; i++) {
                uint8_t* c = pool + (size_t)i * CHUNK;
                for (int o = 0; o + (int)pb <= CHUNK; o += 2) {
                    if (memcmp(c + o, pat, pb) == 0) {
                        scan_log("[scan] hit %016llX\n",
                                 (unsigned long long)(req[i].addr + (uint64_t)o));
                        if (++hits >= 200) break;
                    }
                }
            }
            chunks += n;
        }
        addr = (end > addr) ? end : addr + 0x10000;
    }

    free(pool);
    scan_log("[scan] %d regions, %ld chunks, %d hits, %ld bad batches\n",
             regions, chunks, hits, bad);
    if (bad)
        scan_log("[scan] WARNING: %ld batches returned no data - the result is not trustworthy\n", bad);
    if (g_lg) { fclose(g_lg); g_lg = NULL; }
    return hits ? 0 : 1;
}
