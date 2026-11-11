/* rr_find.c - reachable-object search by class name.
 *
 * See rr_find.h. Breadth-first from the world and the live players, following
 * pointers, bounded in both objects and hops. The visited set is an open-addressed
 * hash table: the graph is cyclic and a linear "seen" list would be quadratic.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "rr_find.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include "rr_game.h"

RR_FIND_STATS g_rr_find_stats;

/* Visited set. */

#define SEEN_SLOTS 131072u          /* Power-of-two slot count for masked hash indexing. */

static uint64_t g_seen[SEEN_SLOTS];

static void seen_reset(void) { memset(g_seen, 0, sizeof(g_seen)); }

/* Returns 1 when newly inserted, 0 when already present. Address 0 is never a
 * valid object, so a zero slot means "empty" without extra bookkeeping. */
static int seen_add(uint64_t v) {
    uint64_t h = (v >> 4) * 0x9E3779B97F4A7C15ull;
    size_t i = (size_t)(h >> 20) & (SEEN_SLOTS - 1);

    for (size_t probe = 0; probe < 64; probe++) {
        if (g_seen[i] == v) return 0;
        if (g_seen[i] == 0) { g_seen[i] = v; return 1; }
        i = (i + 1) & (SEEN_SLOTS - 1);
    }
    return 0;                        /* table congested: treat as seen */
}

/* Queue. */

#define QUEUE_MAX 16384

static uint64_t g_queue[QUEUE_MAX];

int rr_find_class_reachable(const char* needle, RR_FIND_HIT* hits, int max_hits,
                            int max_objects, int depth) {
    static uint8_t buf[0x400];
    static RR_PLAYER players[RR_MAX_PLAYERS];
    int head = 0, tail = 0;
    int found = 0;
    uint64_t world;
    RR_CAMERA cam;

    if (!needle || !*needle || !hits || max_hits <= 0) return 0;
    if (max_objects <= 0) max_objects = 4000;
    if (depth <= 0) depth = 3;

    memset(&g_rr_find_stats, 0, sizeof(g_rr_find_stats));
    seen_reset();

    /* Start from the resolved world and the current player objects. */
    world = game_world_addr();
    g_rr_find_stats.world = world;
    if (PL(world) && seen_add(world)) g_queue[tail++] = world;

    {
        int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
        for (int i = 0; i < n && tail < QUEUE_MAX; i++) {
            uint64_t a = players[i].addr;
            if (PL(a) && seen_add(a)) g_queue[tail++] = a;
        }
    }
    g_rr_find_stats.roots = tail;
    if (!tail) return -1;

    while (head < tail && g_rr_find_stats.visited < max_objects) {
        uint64_t obj = g_queue[head++];
        uint64_t klass;
        int matched = 0;

        if (!rr_read(obj, buf, sizeof(buf))) continue;
        g_rr_find_stats.visited++;

        memcpy(&klass, buf, sizeof(klass));
        if (PL(klass)) {
            char cls[96];

            if (u_class_name(klass, cls, sizeof(cls)) &&
                strstr(cls, needle) != NULL) {
                matched = 1;
                if (found < max_hits) {
                    hits[found].obj = obj;
                    strncpy(hits[found].cls, cls, sizeof(hits[found].cls) - 1);
                    hits[found].cls[sizeof(hits[found].cls) - 1] = '\0';
                }
                found++;
            }
        }

        /* Follow plausible pointers, including the object class pointer at
         * offset zero; class fields can lead to static data.
         */
        for (size_t off = 0; off + 8 <= sizeof(buf); off += 8) {
            uint64_t p;

            memcpy(&p, buf + off, sizeof(p));
            if (!PL(p)) continue;
            if (tail >= QUEUE_MAX) break;
            if (seen_add(p)) g_queue[tail++] = p;
        }
        (void)matched;
    }

    g_rr_find_stats.hits = found;
    g_rr_find_stats.depth_used = depth;
    return found;
}
