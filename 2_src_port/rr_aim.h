/* rr_aim.h - Target-region selection and bounded mouse-steering math. */
#ifndef RR_AIM_H
#define RR_AIM_H
#include "rr_game.h"
/* Profile IDs 0..6 retain their saved-setting meanings. Each full-rig area
 * chooses among real joints; ties use renderer order.
 */
enum { RR_AIM_HEAD, RR_AIM_NECK, RR_AIM_CHEST, RR_AIM_PELVIS,
       RR_AIM_CLOSEST, RR_AIM_ARMS, RR_AIM_LEGS, RR_AIM_FACE,
       RR_AIM_HANDS, RR_AIM_FEET, RR_AIM_GROUP_COUNT };
int rr_aim_bone_in_group(int group, int bone);
int rr_aim_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                 int width, int height, RR_V3* out);
/* When choosing among points, prefer the closest one whose ray is clear. */
int rr_aim_visible_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                         int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3));

struct BR_Config;
typedef struct {
    const RR_PLAYER* player;
    RR_V3 point;
    float dx, dy;
} RR_AIM_PICK;

/* A held lock restricts selection to the original player, even outside the acquisition ring. */
RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                       int width, int height, const struct BR_Config* config,
                       uint64_t locked_addr, int32_t locked_id, int (*clear)(RR_V3, RR_V3));

typedef struct { float carry_x, carry_y; } RR_AIM_STEER;
/* Convert a fresh projected error into bounded mouse counts.
 * cal is 100 times the mouse counts per reference pixel at 1080p and 60-degree
 * vertical FOV. Measure it with --aimtest after changing game sensitivity.
 * A value of 100 assumes one mouse count per reference pixel.
 */
void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing, int* mx, int* my);

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth, RR_V3* out);
#endif