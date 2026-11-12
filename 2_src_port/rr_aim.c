/* rr_aim.c - Select target points and convert projected errors to mouse counts.
 * Selection uses snapshot data; the caller decides whether to send input.
 */
#include "rr_aim.h"
#include "rr_cham_anchors.h"
#include "rr_body_joints.h"
#include "menu_bridge.h"
#include <math.h>

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                       int width, int height, const BR_Config* config,
                       uint64_t locked_addr, int32_t locked_id, int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    float best_priority = INFINITY, best_d2 = INFINITY;
    float cx = width * 0.5f, cy = height * 0.5f;
    float radius = (float)(config->aa_fov_px > 0 ? config->aa_fov_px : 150);
    float limit = locked_addr ? INFINITY : radius * radius;
    int group = config->aa_bone;
    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (locked_addr && (p->addr != locked_addr || p->id != locked_id)) continue;
        if (!p->addr || !p->valid || p->dead || p->is_vehicle || p->is_loot || p->local || p->hidden_in_object) continue;
        if (!rr_relationship_allowed(p->relationships, config->aa_target_clan, config->aa_target_police)) continue;
        if (!isfinite(p->dist) || (config->aa_dist > 0 && p->dist > (float)config->aa_dist)) continue;

        float sx, sy;
        RR_V3 point;
        if (!rr_aim_point(p, group, camera, width, height, &point) ||
            !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        float dx = sx - cx, dy = sy - cy;
        float d2 = dx*dx + dy*dy;
        if (!isfinite(d2) || d2 > limit) continue;
        /* Keep acquisition outside the FOV cheap. If the nearest member is
         * blocked, search the same area for another visible joint. */
        if (clear && !clear(camera->pos,point)) {
            if (!rr_aim_visible_point(p,group,camera,width,height,&point,clear) ||
                !game_w2s(camera,&point,width,height,&sx,&sy)) continue;
            dx=sx-cx; dy=sy-cy; d2=dx*dx+dy*dy;
            if (!isfinite(d2) || d2>limit) continue;
        }
        float priority = config->aa_closest_first ? p->dist : d2;
        if (priority < best_priority || (priority == best_priority && d2 < best_d2)) {
            best = (RR_AIM_PICK){p, point, dx, dy};
            best_priority = priority;
            best_d2 = d2;
        }
        if (locked_addr) break;
    }
    return best;
}

static float aim_response_ms(int smooth) {
    return 12.0f + 8.0f * fmaxf(1.0f, fminf(10.0f, (float)smooth));
}

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth, RR_V3* out) {
    if (sample_ms < 10 || sample_ms > 80 || age_ms > 50) return 0;
    float vx = (position.x - previous.x) * 1000.0f / sample_ms;
    float vz = (position.z - previous.z) * 1000.0f / sample_ms;
    float speed = hypotf(vx, vz);
    if (!(speed >= 0.35f && speed <= 12.0f)) return 0;

    /* Compensate for input smoothing and snapshot age while keeping the
     * correction short enough to recover quickly after a change in direction. */
    float seconds = fminf(0.065f, (aim_response_ms(smooth) + age_ms) * 0.001f);
    seconds = fminf(seconds, 0.55f / speed);
    *out = (RR_V3){ point.x + vx * seconds, point.y, point.z + vz * seconds };
    return 1;
}

static int aim_axis(float error, float gain, float limit, float* carry) {
    if (fabsf(error) < 0.75f) {
        *carry = 0.0f;
        return 0;
    }
    if (error * *carry < 0.0f) *carry = 0.0f;
    float step = fmaxf(-limit, fminf(limit, error * gain + *carry));
    int counts = (int)step;
    *carry = step - counts;
    return counts;
}

void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing, int* mx, int* my) {
    *mx = *my = 0;
    if (!isfinite(dx) || !isfinite(dy) || !(fov > 10.0f && fov < 170.0f) || height < 16) {
        *state = (RR_AIM_STEER){0};
        return;
    }
    /* Project to the equivalent error at 1080p / 60 degrees. The same
     * angular error should produce the same correction at a different zoom. */
    float scale = (1080.0f / height) * (tanf(fov * 3.14159265f / 360.0f) / 0.57735027f);
    /* Convert the normalized pixel error to mouse counts using cal. This
     * ratio depends on the game sensitivity; --aimtest measures it for the current
     * setup. A value of 100 assumes one count per reference pixel.
     */
    if (!(cal > 0)) cal = 100;
    float calib = (float)cal / 100.0f;
    scale *= calib;
    /* Scale the per-step limit with calibration so it represents a similar
     * angular correction at different sensitivities.
     */
    float limit = fmaxf(8.0f, 32.0f * calib);
    float dt = (float)(elapsed_ms > 24 ? 24 : elapsed_ms);
    float response = aim_response_ms(smooth);
    float gain = 1.0f - expf(-dt / response);
    float vertical_gain = firing ? 1.0f - expf(-dt / (response * 0.8f)) : gain;
    *mx = aim_axis(dx * scale, gain, limit, &state->carry_x);
    *my = aim_axis(dy * scale, vertical_gain, limit, &state->carry_y);
}

int rr_aim_bone_in_group(int group, int bone) {
    if (bone<0 || bone>=RR_CHAM_BONES) return 0;
    switch (group) {
    case RR_AIM_HEAD: return bone>=RR_RIG_M_HEAD_1_JNT && bone<=RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_NECK: return bone==RR_RIG_M_NECK_JNT || bone==RR_RIG_L_ARM_0_JNT || bone==RR_RIG_R_ARM_0_JNT;
    case RR_AIM_CHEST: return bone==RR_RIG_M_SPINE_2_JNT || bone==RR_RIG_M_SPINE_3_JNT;
    case RR_AIM_PELVIS: return bone==RR_RIG_M_SPINE_1_JNT || bone==RR_RIG_L_LEG_1_JNT || bone==RR_RIG_R_LEG_1_JNT;
    case RR_AIM_ARMS: return (bone>=RR_RIG_L_ARM_0_JNT && bone<=RR_RIG_L_ARM_3_JNT) ||
                            (bone>=RR_RIG_R_ARM_0_JNT && bone<=RR_RIG_R_ARM_3_JNT) ||
                            bone==RR_RIG_L_ARMTWIST_JNT || bone==RR_RIG_R_ARMTWIST_JNT;
    case RR_AIM_LEGS: return (bone>=RR_RIG_L_LEG_1_JNT && bone<=RR_RIG_L_LEG_3_JNT) ||
                            (bone>=RR_RIG_R_LEG_1_JNT && bone<=RR_RIG_R_LEG_3_JNT);
    case RR_AIM_FACE: return bone>=RR_RIG_L_EYELIDUP_JNT && bone<=RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_HANDS: return (bone>=RR_RIG_L_ARM_3_JNT && bone<=RR_RIG_L_THUMB_3_JNT) ||
                             (bone>=RR_RIG_R_ARM_3_JNT && bone<=RR_RIG_R_THUMB_3_JNT);
    case RR_AIM_FEET: return (bone>=RR_RIG_L_LEG_3_JNT && bone<=RR_RIG_L_FOOT_2_JNT) ||
                            (bone>=RR_RIG_R_LEG_3_JNT && bone<=RR_RIG_R_FOOT_2_JNT);
    case RR_AIM_CLOSEST: return rr_rig_is_body_joint(bone);
    default: return 0;
    }
}

static int full_rig_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                          int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3)) {
    if (!cam || !cam->valid || width<1 || height<1 || p->nbones!=RR_CHAM_BONES) return 0;
    int found=0; float best=INFINITY;
    for (int bone=0;bone<RR_CHAM_BONES;++bone) {
        if (!rr_aim_bone_in_group(group,bone)) continue;
        RR_V3 point={p->bone[bone][0],p->bone[bone][1],p->bone[bone][2]};
        float dx=point.x-p->pos.x,dy=point.y-p->pos.y,dz=point.z-p->pos.z,x,y;
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || dx*dx+dy*dy+dz*dz>36.0f ||
            !game_w2s(cam,&point,width,height,&x,&y) || !isfinite(x) || !isfinite(y)) continue;
        float sx=x-width*.5f,sy=y-height*.5f,score=sx*sx+sy*sy;
        if (score>=best || !isfinite(score) || (clear && !clear(cam->pos,point))) continue;
        *out=point; best=score; found=1;
    }
    return found;
}

static int mesh_anchor(const RR_PLAYER* p, int index, RR_V3* out) {
    if (!p->cham_valid || index<0 || index>=12) return 0;
    const RR_CHAM_ANCHOR* a=&rr_cham_anchors[index];
    const float* m=p->cham_skin[a->bone];
    out->x=m[0]*a->p[0]+m[1]*a->p[1]+m[2]*a->p[2]+m[3];
    out->y=m[4]*a->p[0]+m[5]*a->p[1]+m[6]*a->p[2]+m[7];
    out->z=m[8]*a->p[0]+m[9]*a->p[1]+m[10]*a->p[2]+m[11];
    float dx=out->x-p->pos.x,dy=out->y-p->pos.y,dz=out->z-p->pos.z;
    return isfinite(dx) && isfinite(dy) && isfinite(dz) && dx*dx+dy*dy+dz*dz<36.0f;
}

static RR_V3 group_point(const RR_PLAYER* p, int group) {
    RR_V3 joint;
    if (mesh_anchor(p,group,&joint)) return joint;
    int idx = p->bone_group[group];
    if (p->nbones > 0 && p->nbones <= RR_MAX_BONES && idx >= 0 && idx < p->nbones) {
        RR_V3 v = { p->bone[idx][0], p->bone[idx][1], p->bone[idx][2] };
        return v;
    }
    /* When the game has not created the skeleton, estimate from the character
     * origin. These are position estimates, not measured bone transforms. */
    static const float standing[4] = { 1.65f, 1.48f, 1.20f, 0.88f };
    static const float crouched[4] = { 1.00f, 0.90f, 0.72f, 0.48f };
    RR_V3 v = p->pos;
    v.y += p->crouch == 1 ? crouched[group] : standing[group];
    return v;
}

static int limb_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam, int width, int height,
                      RR_V3* out, int (*clear)(RR_V3, RR_V3)) {
    int start=group==5?4:8,found=0; float best=0;
    for (int side=0;side<2;++side) {
        RR_V3 a,b; float x,y;
        if (!mesh_anchor(p,start+side*2,&a) || !mesh_anchor(p,start+side*2+1,&b)) continue;
        RR_V3 mid={(a.x+b.x)*0.5f,(a.y+b.y)*0.5f,(a.z+b.z)*0.5f};
        if (!game_w2s(cam,&mid,width,height,&x,&y)) continue;
        float dx=x-width*0.5f,dy=y-height*0.5f,dist=dx*dx+dy*dy;
        if ((!found || dist<best) && (!clear || clear(cam->pos,mid))) {
            *out=mid;best=dist;found=1;
        }
    }
    return found;
}

int rr_aim_visible_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                         int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3)) {
    if (!p || !out || (clear && !cam)) return 0;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;
    if (p->full_rig) return full_rig_point(p,group,cam,width,height,out,clear);
}

int rr_aim_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                 int width, int height, RR_V3* out) {
    return rr_aim_visible_point(p,group,cam,width,height,out,0);
}