/* rr_rapid.h - Throttle aim-state sampling while the fire button is held. */
#ifndef RR_RAPID_H
#define RR_RAPID_H
#include <stdint.h>

/* Read the game's aim state at most 40 times per second and only while the
 * physical fire button is held. A new hold always requires a fresh sample. */
typedef struct { uint32_t last_sample; int active; } RR_RAPID_CLOCK;
static int rr_rapid_sample_due(RR_RAPID_CLOCK* clock, uint32_t now, int enabled, int held) {
    if (!enabled || !held) { clock->active=0; return 0; }
    if (!clock->active || now-clock->last_sample>=25u) {
        clock->active=1; clock->last_sample=now; return 1;
    }
    return 0;
}
#endif
