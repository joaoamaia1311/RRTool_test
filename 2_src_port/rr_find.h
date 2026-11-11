/* rr_find.h - Bounded breadth-first search of reachable managed objects.
 *
 * Start with the resolved world and current players, follow plausible object
 * pointers, and compare runtime class names. A visited set prevents cycles
 * from repeating work. Search depth and object count limit memory-read cost.
 * Objects outside this reachable graph will not be found.
 */
#ifndef RR_FIND_H
#define RR_FIND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t obj;
    char     cls[96];
} RR_FIND_HIT;

/* Breadth-first search from the world model and the current players for objects
 * whose class name contains `needle` (case-insensitive). Returns the number of
 * hits written, or -1 when the world is not available.
 *
 * Bounded on purpose: at most `max_objects` objects are visited and pointers are
 * followed at most `depth` hops from a root, because the graph is cyclic and the
 * read cost is unbounded otherwise. */
int rr_find_class_reachable(const char* needle, RR_FIND_HIT* hits, int max_hits,
                            int max_objects, int depth);

/* How the last search went: roots, objects visited, hits. */
typedef struct {
    int roots, visited, hits, depth_used;
    uint64_t world;
} RR_FIND_STATS;
extern RR_FIND_STATS g_rr_find_stats;

#ifdef __cplusplus
}
#endif
#endif
