/* rr_game.h - Snapshot types and reader API for ROB RUN.
 *
 * rr_game.c resolves world and camera objects, publishes a validated roster,
 * and samples positions, rigs and world markers. The renderer and aim worker
 * consume caller-owned snapshots instead of traversing live objects themselves.
 *
 * Managed field offsets are in robrun_offsets.h and robrun_sdk_offsets.h.
 * Native Unity layouts and generic payload offsets are separate assumptions
 * that must be checked when the game build changes. Cached heap addresses
 * are hints that require validation in the current process.
 */
#ifndef RR_GAME_H
#define RR_GAME_H

#include <stdint.h>
#include <stddef.h>
#include "rr_cham_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RR_MAX_PLAYERS 320
#define RR_NAME_MAX    40

typedef struct { float x, y, z; } RR_V3;
typedef struct { float x, y, z, w; } RR_Q4;

#define RR_MAX_BONES RR_CHAM_BONES
enum { RR_REL_CLAN = 1, RR_REL_POLICE = 2 };
enum { RR_LOCAL_CONFIRMED = 1, RR_LOCAL_ESTIMATED = 2 };

/* A player in both groups must pass both switches. */
static inline int rr_relationship_allowed(unsigned flags, int clan, int police) {
    unsigned allowed = (clan ? RR_REL_CLAN : 0) | (police ? RR_REL_POLICE : 0);
    return (flags & ~allowed) == 0;
}

typedef struct RR_PLAYER {
    float    bone[RR_MAX_BONES][3];  /* reconstructed skeleton joints, world space */
    int      nbones;
    int      bone_group[4];          /* head, neck, chest and pelvis indices */
    int      mesh_bones;             /* joints use live mesh transforms */
    int      full_rig;               /* renderer-order palette; unavailable optional joints are NaN */
    int      cham_valid;             /* skinned shared-body matrices available */
    uint32_t cham_sample_at;         /* timeGetTime of last complete pose */
    uint64_t cham_rig;               /* identity of the mapped rig's root */
    float    cham_skin[RR_CHAM_BONES][12];
    int      inveh;                  /* seated in a vehicle (entity id valid) */
    int      vehicle_id;             /* replicated vehicle entity ID, or -1  */
    int      occupants;              /* players observed in this vehicle     */
    int      occupants_known;        /* seat data supplied the count          */
    int      vehicle_rendered;       /* position came from the live marker     */
    int      vehicle_cham_valid;     /* vehicle pose available for its hull */
    float    vehicle_pose[12];       /* vehicle pivot to world, row-major 3x4 */
    char     veh[20];                /* vehicle type name when seated        */
    uint64_t addr;                 /* BattleCharacterModel*                 */
    int32_t  id;
    RR_V3    pos;                  /* feet                                  */
    RR_V3    head;                 /* Resolved head joint or estimated head position. */
    float    dist;                 /* metres from the camera                */
    float    rot_yaw;              /* yaw, degrees                          */
    int      dead;
    int      local;                /* RR_LOCAL_* or zero for a remote player */
    unsigned relationships;        /* RR_REL_* relative to the local player  */
    int      valid;
    int      hidden_in_object;    /* 1 confirmed, 2 inferred from last sighting */
    int      is_vehicle;           /* 1 car, 2 helicopter, never a player target */
    int      is_loot;              /* 1 loose, 2 node, 3 container, 4 door, 5 dropped, 6 vehicle loot */
    int      loot_rarity;          /* 0 none, 1 common .. 6 exotic; -1 unknown */
    int      health;               /* -1 unavailable; otherwise raw replicated value */
    int      armor;                /* -1 unknown, 0 none, 1 has armour       */
    int      crouch;               /* -1 unknown, 0 standing, 1 crouched     */
    char     role[20];             /* PlayerRoleDescription.Name            */
    char     weapon[20];           /* WeaponDescription.Name (dp9_m1, ...)   */
    char     name[RR_NAME_MAX];
} RR_PLAYER;

typedef struct RR_CAMERA {
    RR_V3 pos;
    RR_Q4 rot;                     /* x,y,z,w                               */
    RR_V3 fwd;
    RR_V3 right, up;               /* model-space basis (+0x48 / +0x54)     */
    float fov;                     /* degrees                               */
    int   valid;
    uint64_t model_source;         /* camera model used for this snapshot   */
} RR_CAMERA;

/* Resolve the module, classes and current instances; start background
 * discovery if needed. Return 1 when the initial scan finds characters.
 */
int  game_init(void);
/* Read-only diagnostic for the optional UnityEntry discovery path. */
int game_entry_probe(uint64_t* world, uint64_t* battle, uint64_t* camera);
typedef struct {
    unsigned attempts, success, failed, reused_mesh, reused_cham, missing_rig, held_spikes;
    unsigned quat_samples, neck_reversals, left_reversals, right_reversals;
    unsigned pair_samples, pair_changes;
    unsigned both_arms_rest, rest_reversions;
    unsigned rotation_run_return[6];
    unsigned mapping_joints, mapping_ambiguous;
    unsigned ambiguous_by_bone[RR_CHAM_BONES];
    unsigned named_joints, mismatched_joints, mismatch_by_bone[RR_CHAM_BONES];
    unsigned change_mask[8];
    unsigned standing_samples, seated_samples, seated_holds, seated_root_bypass;
    unsigned arm_branch_holds[2];
} RR_POSE_DIAG;
void game_pose_diag_enable(void);
RR_POSE_DIAG game_pose_diag_results(void);
int game_pose_diag_init(uint64_t world_hint);

/* Explicit recalibration (menu / --probe). */
int  game_calibrate(void);

/* Fill out[] with at most max snapshot entries and *cam with the view.
 * Entries can represent players or enabled world markers. Return the entry
 * count. The caller owns the output buffers; cache publication uses a lock.
 */
int  game_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam);
int  game_refresh_render_camera(RR_CAMERA* cam);
int  game_discovery_active(void);
#ifdef RR_PORT_DIAGNOSTICS
int  game_is_aiming(void);
int  game_shot_event(void);
#endif

/* Human-readable state for the overlay footer and the probe. */
void game_status(char* out, size_t n);

/* Loot vacuum, driven from the addresses game_snapshot's loot entries were
 * validated against.
 *
 *   mode 0 = move each loot position onto the local player (proximity pickup)
 *   mode 1 = set the entry's taken / empty flag
 *   apply 0 = resolve and count only, writing nothing
 *
 * Returns the number of cached entries considered, and reports how many were
 * writable (or would be, with apply 0) and how many were skipped because the
 * cache had aged out. `local_pos_out` may be NULL; it receives the position
 * mode 0 would move loot to (zero when the local player is unknown).
 *
 * Server authority is not modelled here: this changes the client's copy of the
 * loot state, and whether the server accepts any of it depends on the game. */
int  rr_loot_vacuum(int mode, int apply, RR_V3* local_pos_out,
                    int* out_ok, int* out_skipped);

/* World-space -> screen. Uses the camera basis, Unity's left-handed +Z
 * forward convention, vertical FOV in degrees. 1 = on screen. */
int  game_w2s(const RR_CAMERA* cam, const RR_V3* world, int w, int h,
              float* sx, float* sy);
void game_w2s_many(const RR_CAMERA* cam, const RR_V3* world, int count, int w, int h, float* xy);

/* One resolved loot entry, addresses included.
 *
 * These are the objects the ESP itself followed to obtain the name it draws, so
 * dumping them shows what the game actually holds - not a re-resolution that
 * might land somewhere else. `obj` is the node (LootNodeModel), `desc` its
 * description (LocationLootNodeDescription), `loot` the loot description the
 * description's delegate produces, and `inv` the InventoryItemDescription whose
 * name field is the "cash_1" string. */
typedef struct RR_LOOT_ENT {
    uint64_t obj, desc, loot, inv;
    uint64_t pos_addr, flag_addr;
    int32_t  id;
    int      kind, rarity;
    RR_V3    pos;
    char     name[64];
} RR_LOOT_ENT;

/* Copy the published loot cache. Returns the number of entries (0 when the
 * discovery worker has not published anything yet - call game_init() first and
 * poll, the lists appear only once the area is streamed in). */
int rr_loot_snapshot(RR_LOOT_ENT* out, int max);

/* What the loot refresh saw, so "no nodes" can be told apart from "the
 * dictionary is empty" and "the world is wrong". */
typedef struct RR_LOOT_DEBUG {
    uint64_t world;          /* WorldEntitiesModel the cache was built against */
    uint64_t dict_nodes;     /* world+0xB0 - LootNodeModel entries             */
    uint64_t dict_loose;     /* world+0xA0 - RandomLootObjectModel entries     */
    int entries_nodes;       /* its _count, -1 when unreadable                 */
    int entries_loose;
    int cached;              /* entries that survived validation              */
    /* Drop counts distinguish an observed empty/taken node from an invalid
     * read or chain. Invalid reads do not establish that loot is absent.
     */
    int nodes_seen, nodes_empty, nodes_bad;
    int loose_seen, loose_empty, loose_bad;
} RR_LOOT_DEBUG;

/* Rebuild the loot cache on the calling thread.
 *
 * cache_loot() is otherwise reached only from the discovery worker's 2-second
 * timer and only when the overlay's show_loot / show_loot_nodes flags are set,
 * so a diagnostic process can wait forever for a list it never asked for. This
 * drives it directly. Returns the number of published entries. */
int rr_loot_refresh(RR_LOOT_DEBUG* dbg);

/* Address of the local player's BattleCharacterModel, 0 until the roster is
 * published. */
uint64_t rr_local_player(void);

/* Forget every anchor that belongs to the process we were attached to. Call
 * after re-attaching to a relaunched game, before game_init(). */
void game_reattach_reset(void);

/* Offsets discovered at runtime (exposed for the probe printout). */int  game_prim_offset(void);
int  game_dict_offset(void);
int  game_instance_mode(void);
uint64_t game_world_addr(void);
void game_scene_debug(uint64_t* battle, uint64_t* updater, int* scanning);
uint64_t game_lens_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* RR_GAME_H */







