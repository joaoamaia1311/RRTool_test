/* rr_draw.h - ESP frame drawing for ROB RUN. */
#ifndef RR_DRAW_H
#define RR_DRAW_H

#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws one frame of markers into the current gpu_begin()/gpu_present() pair.
 * vw/vh are the overlay surface size in pixels. */
void draw_esp(const RR_PLAYER* arr, int n, const RR_CAMERA* cam, int vw, int vh);

typedef struct {
    unsigned fresh, repeated, initial, state, gap, jump;
    unsigned max_gap_ms;
    float max_jump_m;
    unsigned moving_samples, reversals, bone_jumps;
    float max_bone_delta_m;
    unsigned head_spikes, left_spikes, right_spikes;
} RR_CHAM_DIAG;
void cham_diag_enable(void);
void cham_diag_sample(const RR_PLAYER* player);
RR_CHAM_DIAG cham_diag_results(void);

#ifdef __cplusplus
}
#endif

#endif /* RR_DRAW_H */
