/* rr_draw.c - Render ROB RUN snapshots with the gpu.cpp drawing interface.
 *
 * The frame includes player labels, boxes, radar, world markers, vehicle hulls
 * and a skinned reference body. Full-rig poses are filtered in world space
 * before projection. When a rig is unavailable, estimated body points keep
 * basic markers visible. rr_game.c supplies snapshots; this module draws them.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "gpu.h"
#include "menu_bridge.h"
#include "rr_game.h"
#include "rr_draw.h"
#include "rr_cham_model.h"
#include "rr_body_joints.h"
#include "rr_vehicle_cham_model.h"

static RGBA slot_color(int i) {
    BR_Config* b = br_get();
    if (i < 0 || i >= BR_COLOR_COUNT) return RRGB(255, 255, 255);
    const unsigned char* c = b->color[i].rgb;
    return RARGB(255, c[0], c[1], c[2]);
}

/* Map replicated role names to marker colors for cops, robbers and civilians. */
static RGBA role_color(const char* role) {
    if (strstr(role, "cop")) return RRGB(90, 170, 255);
    if (strstr(role, "robber")) return RRGB(255, 86, 86);
    if (strstr(role, "hubber") || strstr(role, "civilian")) return RRGB(220, 220, 220);
    if (strstr(role, "citizen")) return RRGB(200, 230, 200);
    return 0;
}

static RGBA player_color(const RR_PLAYER* p) {
    if (p->relationships & RR_REL_CLAN) return RRGB(218, 153, 255);
    if (p->relationships & RR_REL_POLICE) return RRGB(86, 255, 246);
    if (p->dead) return slot_color(5);
    RGBA color = role_color(p->role);
    return color ? color : slot_color(0);
}

static void corner_box(float x, float y, float w, float h, RGBA col, float stroke) {
    float cx = w * 0.28f;
    float cy = h * 0.28f;
    if (cx < 3) cx = 3;
    if (cy < 3) cy = 3;
    float l = x, t = y, r = x + w, b = y + h;
    gpu_pen(col, stroke);
    r_line(l, t, l + cx, t);
    r_line(l, t, l, t + cy);
    r_line(r, t, r - cx, t);
    r_line(r, t, r, t + cy);
    r_line(l, b, l + cx, b);
    r_line(l, b, l, b - cy);
    r_line(r, b, r - cx, b);
    r_line(r, b, r, b - cy);
}

typedef struct {
    uint64_t object, rig;
    int32_t id;
    uint32_t sampled_at;
    uint32_t last_used;
    int crouch, dead, inveh, full_rig;
    RR_V3 root;
    RR_V3 diag_delta, diag_bone;
    int diag_have_delta, diag_have_bone;
    RR_V3 diag_shape[3], diag_shape_older[3];
    int diag_shape_count;
    RR_V3 anchors[RR_MAX_BONES + 2]; /* feet, head, then skeleton points */
    float skin[RR_CHAM_BONES][12];
} CHAM_VISUAL_POSE;
static int g_cham_diag_enabled;
static RR_CHAM_DIAG g_cham_diag;
void cham_diag_enable(void) { memset(&g_cham_diag,0,sizeof(g_cham_diag)); g_cham_diag_enabled=1; }
RR_CHAM_DIAG cham_diag_results(void) { return g_cham_diag; }

static const CHAM_VISUAL_POSE* cham_visual_pose(const RR_PLAYER* p) {
    static CHAM_VISUAL_POSE poses[RR_MAX_PLAYERS];
    static uint32_t serial;
    ++serial;
    CHAM_VISUAL_POSE* previous = &poses[0];
    for (int i = 0; i < RR_MAX_PLAYERS; ++i) {
        CHAM_VISUAL_POSE* candidate = &poses[i];
        if (candidate->object == p->addr && candidate->id == p->id && candidate->rig == p->cham_rig) {
            previous = candidate;
            break;
        }
        if (serial - candidate->last_used > serial - previous->last_used) previous = candidate;
    }
    previous->last_used = serial;
    int same = previous->object == p->addr && previous->id == p->id && previous->rig == p->cham_rig &&
               previous->crouch == p->crouch && previous->dead == p->dead && previous->inveh == p->inveh &&
               previous->full_rig == p->full_rig;
    uint32_t elapsed = p->cham_sample_at - previous->sampled_at;
    if (same && elapsed == 0) {
        if (g_cham_diag_enabled) ++g_cham_diag.repeated;
        return previous;
    }

    const float* first_bone = p->cham_skin[0];
    /* A skin matrix includes the inverse bind transform. Its translation
       moves as the torso rotates, even when the character root is stationary. */
    RR_V3 root = p->mesh_bones ? p->pos :
        (RR_V3){first_bone[3], first_bone[7], first_bone[11]};
    float dx = root.x - previous->root.x;
    float dy = root.y - previous->root.y;
    float dz = root.z - previous->root.z;
    int reset = !same || elapsed >= 100 || dx*dx + dy*dy + dz*dz > 0.64f;
    if (g_cham_diag_enabled) {
        ++g_cham_diag.fresh;
        int identity = previous->object == p->addr && previous->id == p->id && previous->rig == p->cham_rig;
        if (!identity) ++g_cham_diag.initial;
        else {
            if (!same) ++g_cham_diag.state;
            if (elapsed >= 100) ++g_cham_diag.gap;
            if (dx*dx+dy*dy+dz*dz > 0.64f) ++g_cham_diag.jump;
            if (elapsed > g_cham_diag.max_gap_ms) g_cham_diag.max_gap_ms=elapsed;
            float jump=sqrtf(dx*dx+dy*dy+dz*dz);
            if (jump > g_cham_diag.max_jump_m) g_cham_diag.max_jump_m=jump;
            RR_V3 bone={p->cham_skin[RR_RIG_M_HEAD_1_JNT][3]-root.x,p->cham_skin[RR_RIG_M_HEAD_1_JNT][7]-root.y,p->cham_skin[RR_RIG_M_HEAD_1_JNT][11]-root.z};
            if (same && elapsed>0 && elapsed<100) {
                if (jump>.01f) {
                    ++g_cham_diag.moving_samples;
                    float prev_len=sqrtf(previous->diag_delta.x*previous->diag_delta.x + previous->diag_delta.y*previous->diag_delta.y + previous->diag_delta.z*previous->diag_delta.z);
                    float dot=dx*previous->diag_delta.x+dy*previous->diag_delta.y+dz*previous->diag_delta.z;
                    if (previous->diag_have_delta && prev_len>.01f && dot<-.5f*jump*prev_len) ++g_cham_diag.reversals;
                }
                if (previous->diag_have_bone) {
                    float bx=bone.x-previous->diag_bone.x, by=bone.y-previous->diag_bone.y, bz=bone.z-previous->diag_bone.z;
                    float delta=sqrtf(bx*bx+by*by+bz*bz);
                    if (delta>.15f) ++g_cham_diag.bone_jumps;
                    if (delta>g_cham_diag.max_bone_delta_m) g_cham_diag.max_bone_delta_m=delta;
                }
                const int indices[3]={RR_RIG_M_HEAD_1_JNT,RR_RIG_L_ARM_2_JNT,RR_RIG_R_ARM_2_JNT};
                for (int q=0;q<3;++q) {
                    const float* mat=p->cham_skin[indices[q]];
                    RR_V3 now_shape={mat[3]-root.x,mat[7]-root.y,mat[11]-root.z};
                    RR_V3 last=previous->diag_shape[q], older=previous->diag_shape_older[q];
                    float dx1=now_shape.x-last.x,dy1=now_shape.y-last.y,dz1=now_shape.z-last.z;
                    float dx2=now_shape.x-older.x,dy2=now_shape.y-older.y,dz2=now_shape.z-older.z;
                    if (previous->diag_shape_count>=2 && dx1*dx1+dy1*dy1+dz1*dz1>.0225f && dx2*dx2+dy2*dy2+dz2*dz2<.0064f) {
                        if (q==0) ++g_cham_diag.head_spikes;
                        else if (q==1) ++g_cham_diag.left_spikes;
                        else ++g_cham_diag.right_spikes;
                    }
                    previous->diag_shape_older[q]=last;
                    previous->diag_shape[q]=now_shape;
                }
                if (previous->diag_shape_count<2) ++previous->diag_shape_count;
            } else {
                previous->diag_shape_count=0;
            }
            previous->diag_delta=(RR_V3){dx,dy,dz}; previous->diag_have_delta=same && elapsed>0 && elapsed<100;
            previous->diag_bone=bone; previous->diag_have_bone=1;
        }
        if (!identity) { previous->diag_have_delta=0; previous->diag_have_bone=0; previous->diag_shape_count=0; }
    }
    /* Keep the filtered rig close to the current pose while damping small
     * animation jitter. A long filter makes moving targets visibly trail. */
    float blend = (float)elapsed / ((float)elapsed + 3.0f);
    if (reset) {
        memcpy(previous->skin, p->cham_skin, sizeof(previous->skin));
    } else {
        /* Carry the entire pose with the live root immediately. Filter only
         * the remaining bone motion so the body stays with the runner. */
        for (int bone = 0; bone < RR_CHAM_BONES; ++bone) {
            float* smoothed = previous->skin[bone];
            const float* live = p->cham_skin[bone];
            if(!isfinite(live[0]) || !isfinite(smoothed[0])) {
                memcpy(smoothed,live,sizeof(previous->skin[bone]));continue;
            }
            /* Keep the root and torso responsive; damp arm/hand jitter and
             * damp the legs separately while seated in a moving vehicle. */
            float bone_blend=blend;
            if (bone>=RR_RIG_L_ARM_0_JNT && bone<=RR_RIG_R_ARMTWIST_JNT)
                bone_blend=(float)elapsed/((float)elapsed+20.0f);
            else if (p->inveh && bone>=RR_RIG_L_LEG_1_JNT && bone<=RR_RIG_R_FOOT_2_JNT)
                bone_blend=(float)elapsed/((float)elapsed+14.0f);
            smoothed[3] += dx;
            smoothed[7] += dy;
            smoothed[11] += dz;
            for (int component = 0; component < 12; ++component)
                smoothed[component] += (live[component] - smoothed[component]) * bone_blend;
        }
    }
    /* Project markers from the same filtered world pose as the mesh. Keep
       camera motion immediate by applying this before screen projection. */
    for (int i = 0; i < RR_MAX_BONES + 2; ++i) {
        if (p->full_rig && i>=2) {
            int bone=i-2;
            if(!isfinite(p->bone[bone][0]) || !isfinite(p->bone[bone][1]) || !isfinite(p->bone[bone][2])) {
                previous->anchors[i]=(RR_V3){NAN,NAN,NAN};continue;
            }
            const float* m=previous->skin[bone];
            const float* a=rr_cham_bones[bone].anchor;
            previous->anchors[i]=(RR_V3){
                m[0]*a[0]+m[1]*a[1]+m[2]*a[2]+m[3],
                m[4]*a[0]+m[5]*a[1]+m[6]*a[2]+m[7],
                m[8]*a[0]+m[9]*a[1]+m[10]*a[2]+m[11]};
            continue;
        }
        RR_V3 live;
        if (i == 0) live = p->pos;
        else if (i == 1) live = p->head;
        else memcpy(&live, p->bone[i - 2], sizeof(live));
        RR_V3* anchor = &previous->anchors[i];
        if (reset) *anchor = live;
        else {
            anchor->x += dx + (live.x - anchor->x - dx) * blend;
            anchor->y += dy + (live.y - anchor->y - dy) * blend;
            anchor->z += dz + (live.z - anchor->z - dz) * blend;
        }
    }
    previous->object = p->addr;
    previous->id = p->id;
    previous->rig = p->cham_rig;
    previous->sampled_at = p->cham_sample_at;
    previous->crouch = p->crouch;
    previous->dead = p->dead;
    previous->inveh = p->inveh;
    previous->full_rig = p->full_rig;
    previous->root = root;
    return previous;
}
void cham_diag_sample(const RR_PLAYER* player) {
    if (player && player->valid && !player->local && !player->is_vehicle && !player->is_loot &&
        player->cham_valid && player->cham_rig && player->cham_sample_at) (void)cham_visual_pose(player);
}

/* Render the offline shared game mesh, skinned to this character's live rig.
 * Clothing and body variations may differ from this shared reference mesh. */
static int mesh_highlight(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh, RGBA color) {
    if (!p->cham_valid) return 0;
    const float (*skin)[12] = p->cham_skin;
    if (p->cham_rig && p->cham_sample_at) skin = cham_visual_pose(p)->skin;
    float points[RR_CHAM_VERTICES*2];
    RR_V3 positions[RR_CHAM_VERTICES];
    for (int i=0;i<RR_CHAM_VERTICES;++i) {
        const RR_CHAM_VERTEX* v=&rr_cham_vertices[i]; RR_V3 world={0,0,0}; float total=0;
        for (int k=0;k<4;++k) {
            float w=v->weight[k]; int bone=v->bone[k];
            if (w<=0.0001f || bone>=RR_CHAM_BONES) continue;
            const float* m=skin[bone];
            world.x+=w*(m[0]*v->p[0]+m[1]*v->p[1]+m[2]*v->p[2]+m[3]);
            world.y+=w*(m[4]*v->p[0]+m[5]*v->p[1]+m[6]*v->p[2]+m[7]);
            world.z+=w*(m[8]*v->p[0]+m[9]*v->p[1]+m[10]*v->p[2]+m[11]);
            total+=w;
        }
        positions[i].x=positions[i].y=positions[i].z=NAN;
        if (total<0.9f || total>1.1f || !isfinite(world.x) || !isfinite(world.y) || !isfinite(world.z)) continue;
        float dx=world.x-p->pos.x,dy=world.y-p->pos.y,dz=world.z-p->pos.z;
        if (dx*dx+dy*dy+dz*dz>36.0f) continue;
        positions[i]=world;
    }
    game_w2s_many(cam,positions,RR_CHAM_VERTICES,vw,vh,points);
    return r_fill_triangles(points,RR_CHAM_VERTICES,rr_cham_triangles,RR_CHAM_TRIANGLES,
                            (color&0x00FFFFFFu)|0x66000000u)>0;
}

/* Draw anatomical parent links. Attachment helpers can be parked at the
 * character origin and must not appear as body skeleton segments. */
static void draw_full_skeleton(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    RR_V3 world[RR_CHAM_BONES]; float xy[RR_CHAM_BONES*2];
    memcpy(world,p->bone,sizeof(world));
    game_w2s_many(cam,world,RR_CHAM_BONES,vw,vh,xy);
    for (int b=0;b<RR_CHAM_BONES;++b) {
        int parent=rr_cham_bones[b].parent;
        if (parent<0 || parent>=RR_CHAM_BONES) continue;
        if(!rr_rig_is_body_joint(b) || !rr_rig_is_body_joint(parent))continue;
        float x=xy[b*2],y=xy[b*2+1],px=xy[parent*2],py=xy[parent*2+1];
        if (isfinite(x) && isfinite(y) && isfinite(px) && isfinite(py)) r_line(px,py,x,y);
    }
}

/* Build short labels without variadic formatting in the draw loop. */
static void fmt_num(char* out, size_t n, unsigned v, const char* suffix) {
    char tmp[16];
    int k = 0;
    size_t o = 0;
    if (!n) return;
    do { tmp[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && k < 15);
    while (k > 0 && o + 1 < n) out[o++] = tmp[--k];
    while (suffix && *suffix && o + 1 < n) out[o++] = *suffix++;
    out[o] = 0;
}
static void fmt_copy(char* out, size_t n, const char* a, const char* b) {
    size_t o = 0;
    if (!n) return;
    if (a) while (*a && o + 1 < n) out[o++] = *a++;
    if (b) while (*b && o + 1 < n) out[o++] = *b++;
    out[o] = 0;
}

static int world_marker_visible(const RR_PLAYER* p, const BR_Config* b) {
    return ((p->is_loot == 1 && b->show_loot) || (p->is_loot == 2 && b->show_loot_nodes))
               ? br_loot_rarity_visible(b, p->loot_rarity)
               : (p->is_loot == 3 && b->show_containers) ||
           (p->is_loot == 4 && b->show_doors) ||
           (p->is_loot == 5 && b->show_dropped_loot) ||
           (p->is_loot == 6 && b->show_vehicle_loot);
}

static void draw_radar(const RR_PLAYER* arr, int n, const RR_CAMERA* cam,
                       int vw, int vh, const BR_Config* b) {
    const RR_PLAYER* local = NULL;
    for (int i = 0; i < n; ++i) {
        if (arr[i].valid && arr[i].local == RR_LOCAL_CONFIRMED) {
            local = &arr[i];
            break;
        }
    }
    /* Match the centre of ROB RUN's top-left minimap at 1920x1080; scale the
     * HUD coordinates with height so the overlay follows the game client. */
    float width = fminf((float)vh * (210.0f / 1080.0f), (float)vw * .25f);
    float height = fminf((float)vh * (180.0f / 1080.0f), (float)vw * .25f);
    if (!local || width < 70.0f || height < 70.0f || b->radar_range <= 0) return;
    float fx = cam->fwd.x, fz = cam->fwd.z;
    float length = hypotf(fx, fz);
    if (!(length > 0.01f) || !isfinite(length)) {
        fx = sinf(local->rot_yaw);
        fz = cosf(local->rot_yaw);
    } else {
        fx /= length;
        fz /= length;
    }
    float cx = (float)vh * (148.0f / 1080.0f);
    float cy = (float)vh * (130.0f / 1080.0f);
    float left = cx - width * .5f, top = cy - height * .5f;
    float plot_x = width * .5f - 6.0f, plot_y = height * .5f - 6.0f;
    float range = (float)b->radar_range;
    /* The transparent centre leaves the game's map and icons visible. */
    r_rect(left, top, width, height, RARGB(180, 230, 224, 245), 1.0f);

    for (int i = 0; i < n; ++i) {
        const RR_PLAYER* p = &arr[i];
        if (!p->valid || p->local) continue;
        if (p->is_vehicle && (!b->radar_world || !b->show_vehicle_markers)) continue;
        if (p->is_loot) {
            if (!b->radar_world || !world_marker_visible(p, b)) continue;
        } else if (!p->is_vehicle && !rr_relationship_allowed(p->relationships, b->show_clan, b->show_police)) continue;
        float dx = p->pos.x - local->pos.x, dz = p->pos.z - local->pos.z;
        float lateral=dx*fz-dz*fx, forward=dx*fx+dz*fz;
        if (!(fabsf(lateral)<=range && fabsf(forward)<=range)) continue;
        float x = cx + lateral*(plot_x/range), y = cy - forward*(plot_y/range);
        if (p->is_vehicle) {
            r_fill_ellipse(x, y, 2.8f, 2.8f, RRGB(255, 214, 110));
            continue;
        }
        if (p->is_loot) {
            RGBA color = p->is_loot == 1 ? RRGB(235, 235, 235) :
                         p->is_loot == 2 ? RRGB(201, 155, 255) :
                         p->is_loot == 3 ? RRGB(255, 190, 92) :
                         p->is_loot == 4 ? RRGB(200, 200, 200) :
                         p->is_loot == 5 ? RRGB(111, 232, 145) : RRGB(105, 178, 255);
            r_fill_ellipse(x, y, 2.3f, 2.3f, color);
            continue;
        }
        RGBA color = player_color(p);
        if (p->dead) r_ellipse(x, y, 3.0f, 3.0f, color, 1.0f);
        else {
            r_fill_ellipse(x, y, 4.5f, 4.5f, RRGB(16, 11, 25));
            r_fill_ellipse(x, y, 3.1f, 3.1f, color);
        }
    }
}

static const RR_VEHICLE_MESH* vehicle_mesh(const RR_PLAYER* p) {
    char key[RR_NAME_MAX];
    size_t length = 0;
    for (const unsigned char* c = (const unsigned char*)p->name; *c && length < sizeof(key) - 1; ++c) {
        if (isalnum(*c)) key[length++] = (char)tolower(*c);
    }
    key[length] = '\0';
    for (size_t i = 0; i < sizeof(rr_vehicle_meshes) / sizeof(rr_vehicle_meshes[0]); ++i) {
        const RR_VEHICLE_MESH* mesh = &rr_vehicle_meshes[i];
        if (strstr(key, mesh->name)) return mesh;
    }
    return NULL;
}

static void draw_vehicle_cham(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    const RR_VEHICLE_MESH* mesh = vehicle_mesh(p);
    if (!mesh) return;
    RR_V3 positions[RR_VEHICLE_MESH_MAX_VERTICES];
    float projected[2 * RR_VEHICLE_MESH_MAX_VERTICES];
    const float* pose = p->vehicle_pose;
    for (int i = 0; i < mesh->vertices; ++i) {
        const short* v = mesh->positions + 3 * i;
        float x = v[0] * 0.001f, y = v[1] * 0.001f, z = v[2] * 0.001f;
        positions[i] = (RR_V3){
            pose[0] * x + pose[1] * y + pose[2] * z + pose[3],
            pose[4] * x + pose[5] * y + pose[6] * z + pose[7],
            pose[8] * x + pose[9] * y + pose[10] * z + pose[11]
        };
    }
    game_w2s_many(cam, positions, mesh->vertices, vw, vh, projected);
    float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    for (int i = 0; i < mesh->vertices; ++i) {
        float x = projected[2 * i], y = projected[2 * i + 1];
        if (!isfinite(x) || !isfinite(y)) continue;
        left = fminf(left, x); right = fmaxf(right, x);
        top = fminf(top, y); bottom = fmaxf(bottom, y);
    }
    if (right < 0 || bottom < 0 || left > vw || top > vh || right - left < 2.0f || bottom - top < 2.0f) return;
    /* A single translucent amber identifies the vehicle independently of the
     * role colors used for its occupants. */
    RGBA color = RARGB(80, 255, 181, 65);
    r_fill_triangles(projected, mesh->vertices, mesh->triangles, mesh->faces, color);
}

static void draw_loot(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    static const char* const rarity_names[] = { "", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Exotic" };
    static const RGBA rarity_colors[] = {
        RRGB(230, 230, 230), RRGB(230, 230, 230), RRGB(111, 232, 145), RRGB(105, 178, 255),
        RRGB(211, 155, 255), RRGB(255, 190, 92), RRGB(255, 113, 149)
    };
    float x, y;
    if (!game_w2s(cam, &p->pos, vw, vh, &x, &y)) return;
    int rarity = p->loot_rarity >= 1 && p->loot_rarity <= 6 ? p->loot_rarity : 0;
    const char* name = p->name[0] ? p->name : "Loot";
    const char* prefix = p->is_loot == 2 ? "Node: " : "";
    char label[RR_NAME_MAX + 48];
    snprintf(label, sizeof(label), "%s%s%s%s (%.0f m)", prefix, name,
             rarity ? " / " : "", rarity_names[rarity], p->dist);
    RGBA color = rarity_colors[rarity];
    if (p->is_loot == 2) r_ellipse(x, y, 4.0f, 4.0f, color, 1.5f);
    else r_fill_ellipse(x, y, 4.0f, 4.0f, color);
    gpu_font(12.0f);
    r_text(x, y + 6.0f, label, color, RT_CENTER | RT_OUTLINE);
}

void draw_esp(const RR_PLAYER* arr, int n, const RR_CAMERA* cam, int vw, int vh) {
    BR_Config* b = br_get();
    b->telem_cham_n=0;
    if (!b->enabled) return;
    /* The screen-space FOV circle does not depend on a player or camera. */
    if (!arr || !cam || !cam->valid || n < 0) n = 0;

    float line_w = (float)(b->line_w < 1 ? 1 : (b->line_w > 4 ? 4 : b->line_w));
    if (vw < 16 || vh < 16) return;
    if (n>RR_MAX_PLAYERS) n=RR_MAX_PLAYERS;

    if ((b->cham_on || b->vehicle_chams) && n>0) {
        /* Always put vehicles behind player chams, regardless of roster order. */
        if (b->vehicle_chams) for (int i=0;i<n;++i) {
            const RR_PLAYER* p=&arr[i];
            if (!p->valid || !p->is_vehicle || !p->vehicle_cham_valid ||
                (b->max_distance>0 && p->dist>(float)b->max_distance/100.0f)) continue;
            draw_vehicle_cham(p, cam, vw, vh);
        }
        gpu_mesh_flush();
        for (int i=0;i<n;++i) {
            const RR_PLAYER* p=&arr[i];
            if (!p->valid || (p->local && !b->show_self) || p->is_loot ||
                (b->max_distance>0 && p->dist>(float)b->max_distance/100.0f)) continue;
            if (p->is_vehicle) continue;
            if (!b->cham_on || !p->cham_valid) continue;
            if (!rr_relationship_allowed(p->relationships, b->show_clan, b->show_police)) continue;
            float fx,fy,hx,hy;
            if (!game_w2s(cam,&p->pos,vw,vh,&fx,&fy) && !game_w2s(cam,&p->head,vw,vh,&hx,&hy)) continue;
            if (mesh_highlight(p,cam,vw,vh,player_color(p))) {
                ++b->telem_cham_n;
            }
        }
        gpu_mesh_flush();
    }

    if (b->show_self) {
        const RR_PLAYER* self = NULL;
        for (int i = 0; i < n; ++i) if (arr[i].valid && arr[i].local) { self = &arr[i]; break; }
        gpu_font(12.0f);
        r_text(12.0f, 12.0f, !self ? "SELF: unavailable" :
               self->hidden_in_object ? "SELF: hiding object detected" : "SELF: no hiding object detected",
               RRGB(227, 215, 252), RT_LEFT | RT_OUTLINE);
    }
    int drawn = 0;
    for (int i = 0; i < n; i++) {
        /* Apply the player drawing budget while walking the mixed snapshot. */
        if (drawn >= RR_MAX_PLAYERS) continue;
        const RR_PLAYER* p = &arr[i];
        if (!p->valid || (p->local && !b->show_self)) continue;
        if (!rr_relationship_allowed(p->relationships, b->show_clan, b->show_police)) continue;
        /* Menu settings are stored in centimetres; positions use metres. */
        if (b->max_distance > 0 && p->dist > (float)b->max_distance / 100.0f) continue;

        if (p->is_loot) {
            int visible = (p->is_loot == 1 && b->show_loot) ||
                          (p->is_loot == 2 && b->show_loot_nodes) ||
                          world_marker_visible(p, b);
            if (visible && (p->is_loot > 2 || br_loot_rarity_visible(b, p->loot_rarity)))
                draw_loot(p, cam, vw, vh);
            continue;
        }
        if (p->is_vehicle) {
            if (b->show_vehicle_markers) {
                float x, y;
                if (game_w2s(cam, &p->pos, vw, vh, &x, &y)) {
                    char label[RR_NAME_MAX + 32];
                    const char* name = p->name[0] ? p->name : (p->is_vehicle == 2 ? "Helicopter" : "Car");
                    if (b->show_vehicle_occupants && p->occupants_known)
                        snprintf(label, sizeof label, "%s [%d] (%.0f m)", name, p->occupants, p->dist);
                    else snprintf(label, sizeof label, "%s (%.0f m)", name, p->dist);
                    RGBA color = RRGB(255, 214, 110);
                    r_fill_ellipse(x, y, 4.0f, 4.0f, color);
                    gpu_font(12.0f); r_text(x, y + 6.0f, label, color, RT_CENTER | RT_OUTLINE);
                }
            }
            continue;
        }

        RR_PLAYER visual;
        if (p->cham_valid && p->cham_rig && p->cham_sample_at && (!p->inveh || p->full_rig)) {
            const CHAM_VISUAL_POSE* pose = cham_visual_pose(p);
            visual = *p;
            visual.pos = pose->anchors[0];
            visual.head = pose->anchors[1];
            memcpy(visual.bone, &pose->anchors[2], sizeof(visual.bone));
            p = &visual;
        }

        float fx = NAN, fy = NAN, hx = NAN, hy = NAN;
        /* Without a usable rig, estimate a seated body at 1.45 m and widen the
         * box to match the vehicle marker.
         */
        RR_V3 top = p->head;
        /* Use the resolved head joint for seated players when available; the
         * fixed-height fallback can place the marker above the vehicle roof.
         */
        if (p->inveh && !p->mesh_bones) { top.x = p->pos.x; top.y = p->pos.y + 1.45f; top.z = p->pos.z; }
        int feet_ok = game_w2s(cam, &p->pos, vw, vh, &fx, &fy);
        int head_ok = game_w2s(cam, &top, vw, vh, &hx, &hy);
        if ((!feet_ok && !head_ok) || !isfinite(fx) || !isfinite(fy) || !isfinite(hx) || !isfinite(hy)) continue;

        float bh = fabsf(fy - hy);
        if (bh < 4.0f) bh = 4.0f;
        if (p->inveh && bh > 420.0f) bh = 420.0f;
        float bw = bh * (p->inveh ? 1.30f : 0.52f) + fabsf(hx - fx);
        float bx = (fx + hx) * 0.5f - bw * 0.5f;
        float by = fminf(hy, fy);
        if (bx < -200 || bx > vw + 200 || by < -200 || by > vh + 200) continue;

        RGBA col = player_color(p);
        if (p->dead) {
            /* downed: dim, but still visible */
            int a = 255 * 60 / 100;
            col = (col & 0x00FFFFFFu) | ((uint32_t)a << 24);
        }


        if (b->show_box) {
            corner_box(bx, by, bw, bh, col, line_w);
        }
        if (b->show_head) {
            /* Clamp the head-dot radius so nearby players do not produce oversized
             * markers.
             */
            float r = bw * 0.11f;
            if (r < 2.0f) r = 2.0f;
            if (r > 9.0f) r = 9.0f;
            r_fill_ellipse(hx, hy, r, r, col);
        }
        if (b->show_skeleton && (!p->inveh || p->full_rig) && !p->hidden_in_object &&
            (b->skel_distance <= 0 || p->dist <= (float)b->skel_distance / 100.0f)) {
            gpu_pen(col, line_w);
            if (p->full_rig && p->nbones == RR_CHAM_BONES) {
                draw_full_skeleton(p,cam,vw,vh);
            } else if (p->nbones > 1) {
                /* spine chain and head from the animated SkeletonModel rig */
                for (int k = 0; k + 1 < p->nbones; k++) {
                    float ax, ay, bx2, by2;
                    if (!game_w2s(cam, (const RR_V3*)p->bone[k], vw, vh, &ax, &ay)) continue;
                    if (!game_w2s(cam, (const RR_V3*)p->bone[k + 1], vw, vh, &bx2, &by2)) continue;
                    r_line(ax, ay, bx2, by2);
                }
                float hxx, hyy;
                if (game_w2s(cam, (const RR_V3*)p->bone[p->nbones - 1], vw, vh, &hxx, &hyy)) {
                    float r = fabsf(fy - hyy) * 0.075f;
                    if (r < 2.5f) r = 2.5f;
                    if (r > 11.0f) r = 11.0f;
                    r_fill_ellipse(hxx, hyy - r * 0.5f, r, r, col);
                }
            } else {
                /* no rig data yet: two-segment torso fallback */
                float mid = by + bh * 0.55f;
                r_line(fx, by + bh * 0.10f, fx, mid);
                r_line(fx, mid, fx, by + bh);
                r_line(fx - bw * 0.30f, mid, fx + bw * 0.30f, mid);
            }
        }        int near_player = (p->dist < 120.0f);
        if (b->show_name && (p->role[0] || p->relationships) && near_player) {
            const char* rt = "CITIZEN";
            if (p->relationships == (RR_REL_CLAN | RR_REL_POLICE)) rt = "CLAN / POLICE";
            else if (p->relationships & RR_REL_CLAN) rt = "CLAN MEMBER";
            else if (p->relationships & RR_REL_POLICE) rt = "POLICE COWORKER";
            else if (strstr(p->role, "cop")) rt = "COP";
            else if (strstr(p->role, "robber")) rt = "ROBBER";
            else if (strstr(p->role, "hubber")) rt = "CIVILIAN";
            else if (!strstr(p->role, "citizen")) rt = 0;
            if (rt) {
                /* Draw a filled role badge: blue for cops, red for robbers, green for citizens. */
                gpu_font(13.0f);
                float tw = r_text_width(rt) + 12.0f;
                float th = 16.0f;
                float cyb = by - 34.0f;
                RGBA bg = (col & 0x00FFFFFFu) | (170u << 24);
                r_fill_ellipse(fx, cyb, tw * 0.5f, th * 0.5f, bg);
                r_text(fx, cyb - 7.0f, rt, RRGB(10, 12, 16), RT_CENTER);
            }
        }
        if (b->show_name) {
            char label[RR_NAME_MAX];
            if (p->local) fmt_copy(label, sizeof(label), "YOU", 0);
            else if (p->name[0]) fmt_copy(label, sizeof(label), p->name, 0);
            else if (p->id) { label[0] = 0x23; fmt_num(label + 1, sizeof(label) - 1, (unsigned)p->id, 0); }
            else label[0] = 0;
            if (label[0]) {
                gpu_font(12.0f);
                r_text(fx, by - 15.0f, label, slot_color(6), RT_CENTER | RT_OUTLINE);
            }
        }
        if (p->hidden_in_object) {
            gpu_font(12.0f);
            r_text(fx, by - 49.0f, p->hidden_in_object == 2 ? "POSSIBLY HIDING" : "HIDING", col, RT_CENTER | RT_OUTLINE);
        }
        if (p->inveh) {
            char vbuf[32];
            if (p->veh[0]) fmt_copy(vbuf, sizeof(vbuf), "IN ", p->veh);
            else fmt_copy(vbuf, sizeof(vbuf), "IN VEHICLE", 0);
            gpu_font(12.0f);
            r_text(fx, by - 49.0f, vbuf, col, RT_CENTER | RT_OUTLINE);
        }
        float label_y = by + bh + 2.0f;
        if (b->show_distance) {
            char buf[32];
            fmt_num(buf, sizeof(buf), (unsigned)(p->dist + 0.5f), " m");
            gpu_font(11.0f);
            r_text(fx, label_y, buf, slot_color(8), RT_CENTER | RT_OUTLINE);
            label_y += 12.0f;
        }
        if (b->show_weapon && p->weapon[0]) {
            /* "dp9_m1" -> "DP9": the replicated id carries a variant suffix */
            char wbuf[24];
            size_t k = 0;
            for (const char* s = p->weapon; *s && k < sizeof(wbuf) - 1; s++) {
                if (*s == '_') break;
                wbuf[k++] = (char)toupper((unsigned char)*s);
            }
            wbuf[k] = 0;
            gpu_font(10.0f);
            r_text(fx, label_y, wbuf, slot_color(7), RT_CENTER | RT_OUTLINE);
            label_y += 11.0f;
        }
        if (b->show_health && p->health >= 0) {
            char health[32];
            snprintf(health, sizeof health, "HP: %d", p->health);
            gpu_font(10.0f);
            r_text(fx, label_y, health, slot_color(6), RT_CENTER | RT_OUTLINE);
            label_y += 11.0f;
        }
        if (b->show_crouch && p->crouch == 1) {
            gpu_font(10.0f);
            r_text(fx, label_y, "CROUCHING", slot_color(6), RT_CENTER | RT_OUTLINE);
            label_y += 11.0f;
        }
        if (b->show_armor && p->armor == 1) {
            gpu_font(9.0f);
            r_text(fx + bw * 0.5f + 3.0f, by + 2.0f, "ARMOR", slot_color(9), RT_LEFT | RT_OUTLINE);
        }
        if (p->dead) {
            gpu_font(10.0f);
            r_text(fx, by + bh * 0.5f, "DOWN", slot_color(5), RT_CENTER | RT_OUTLINE);
        }
        drawn++;
    }

    /* Aim-assist FOV circle: same circle that defines the capture zone. */
    if (b->aa_on && b->aa_show_fov) {
        int r = b->aa_fov_px;
        if (r < 10) r = 10;
        if (r > 1200) r = 1200;
        r_ellipse((float)vw * 0.5f, (float)vh * 0.5f, (float)r, (float)r,
                  slot_color(14), 1.0f);
    }
    if (b->radar_on && n > 0) draw_radar(arr, n, cam, vw, vh, b);

    b->telem_targets = drawn;
}












