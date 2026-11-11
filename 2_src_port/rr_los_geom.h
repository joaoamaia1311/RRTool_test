/* rr_los_geom.h - Captured shape layouts and segment-intersection interfaces. */
#ifndef RR_LOS_GEOM_H
#define RR_LOS_GEOM_H
#include <stdint.h>
#include "rr_game.h"

enum { RR_LOS_UNKNOWN=-1, RR_LOS_BLOCKED=0, RR_LOS_CLEAR=1 };
typedef struct { float min[3],max[3]; int parent,left,right,body; } RR_LOS_NODE;
typedef struct { int type,tree; uint8_t shape,pad[3]; int id; } RR_LOS_BODY;
typedef struct { float width,height,length; uint8_t surface,pad[3]; int hit; float restitution,friction; float pos[3],rot[4]; } RR_LOS_BOX;
typedef struct { float a[3],b[3],c[3]; uint8_t surface,pad[3]; int hit; float restitution,friction; } RR_LOS_TRI;
typedef struct { float length,radius; uint8_t surface,pad[3]; int hit; float restitution,friction; float pos[3],rot[4],up[3]; } RR_LOS_CAPSULE;
typedef struct { int hull; float pos[3],rot[4]; uint8_t surface,pad[3]; int hit; float restitution,friction; } RR_LOS_HULL;
typedef struct { float normal[3],distance; } RR_LOS_PLANE;
typedef struct { RR_LOS_PLANE* planes; int count; } RR_LOS_HULL_DATA;
typedef struct {
    RR_LOS_NODE* nodes; int node_count,root;
    RR_LOS_BODY* bodies; int body_count;
    RR_LOS_BOX* boxes; int box_count;
    RR_LOS_TRI* triangles; int triangle_count;
    RR_LOS_CAPSULE* capsules; int capsule_count;
    RR_LOS_HULL* hulls; int hull_count;
    RR_LOS_HULL_DATA* hull_data; int hull_data_count;
} RR_LOS_STATIC;

/* Scene tracing returns CLEAR, BLOCKED or UNKNOWN. The shape/segment
 * helpers below return intersection predicates; consult their implementations.
 */
int rr_los_static_trace(const RR_LOS_STATIC* scene, RR_V3 start, RR_V3 end);
int rr_los_aabb_segment(RR_V3 start, RR_V3 end, const float min[3], const float max[3]);
int rr_los_box_segment(RR_V3 start, RR_V3 end, const RR_LOS_BOX* box);
int rr_los_triangle_segment(RR_V3 start, RR_V3 end, const RR_LOS_TRI* triangle);
int rr_los_capsule_segment(RR_V3 start, RR_V3 end, const RR_LOS_CAPSULE* capsule);
int rr_los_hull_segment(RR_V3 start, RR_V3 end, const RR_LOS_HULL* hull, const RR_LOS_HULL_DATA* data);
#endif
