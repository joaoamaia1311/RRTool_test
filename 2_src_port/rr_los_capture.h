/* rr_los_capture.h - Owned static/runtime captures and their cleanup functions. */
#ifndef RR_LOS_CAPTURE_H
#define RR_LOS_CAPTURE_H
#include <stdint.h>
#include "rr_los_geom.h"

/* The capture is a private read-only copy; the original game is never modified. */
typedef struct { RR_LOS_STATIC geometry; uint64_t physics_data,tree_identity; } RR_LOS_CAPTURE;
RR_LOS_CAPTURE* rr_los_capture_static(uint64_t physics_data);
void rr_los_capture_free(RR_LOS_CAPTURE* capture);

typedef struct { int type; uint8_t enabled,pad0[3]; int fields[8]; uint8_t shape,pad1[3]; int id; float pos[3],rot[4]; uint8_t tail[128]; } RR_LOS_RUNTIME_BODY;
typedef struct { int links; float min[3],max[3]; int body; uint8_t used,pad[3]; int next; } RR_LOS_GRID_ITEM;
typedef struct { float width,height,length; uint8_t surface,pad[3]; int hit; float restitution,friction; } RR_LOS_RUNTIME_BOX;
typedef struct { float pos[3],rot[4]; } RR_LOS_LOCAL_TRANSFORM;
typedef struct { float size_x,size_z; int nx,nz; float* heights; float offset_min[3],offset_max[3]; } RR_LOS_TERRAIN;
typedef struct {
    RR_LOS_NODE* static_nodes; int static_node_count,static_root;
    RR_LOS_RUNTIME_BODY* static_bodies; int static_body_count;
    RR_LOS_RUNTIME_BOX* boxes; int box_count;
    int* box_local_ids; int box_local_count;
    RR_LOS_LOCAL_TRANSFORM* locals; int local_count;
    RR_LOS_TERRAIN* terrains; int terrain_count;
    RR_LOS_GRID_ITEM* dynamic_items; int dynamic_item_count;
    uint8_t* dynamic_active;
    RR_LOS_RUNTIME_BODY* dynamic_bodies; int dynamic_body_count;
} RR_LOS_RUNTIME;
RR_LOS_RUNTIME* rr_los_capture_runtime(uint64_t physics_data);
void rr_los_runtime_free(RR_LOS_RUNTIME* runtime);
int rr_los_runtime_trace(const RR_LOS_RUNTIME* runtime, RR_V3 start, RR_V3 end);
#endif
