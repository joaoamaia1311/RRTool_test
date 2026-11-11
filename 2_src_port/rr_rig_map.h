/* rr_rig_map.h - Check joint ancestry before binding a native rig palette. */
#ifndef RR_RIG_MAP_H
#define RR_RIG_MAP_H
#include "rr_cham_model.h"

/* Native hierarchies can contain several characters. A joint must belong to
 * this character's anchored spine subtree, including intermediate objects. */
static int rr_rig_descendant(const int* parents, int count, int index, int ancestor) {
    for (int depth=0; depth<128 && index>=0 && index<count; ++depth) {
        if (index==ancestor) return 1;
        index=parents[index];
    }
    return 0;
}

/* Check the nearest mapped ancestor, not just the direct native parent:
 * bag joints have an intervening bag_placeholder that is not a skin bone. */
static int rr_rig_parent_valid(int b,const int* map,const int* parents,int count) {
    if(map[b]<0 || map[b]>=count)return 0;
    int expected=rr_cham_bones[b].parent;
    while(expected>=0 && map[expected]<0)expected=rr_cham_bones[expected].parent;
    if(expected<0)return 1;
    int parent=parents[map[b]];
    for(int depth=0;depth<128 && parent>=0 && parent<count;++depth) {
        for(int other=0;other<RR_CHAM_BONES;++other)
            if(map[other]==parent)return other==expected;
        parent=parents[parent];
    }
    return 0;
}
static void rr_rig_prune_optional(int* map,const int* parents,int count) {
    for(int pass=0;pass<RR_CHAM_BONES;++pass) {
        int changed=0;
        for(int b=0;b<RR_CHAM_BONES;++b)
            if(!rr_cham_bones[b].required && map[b]>=0 && !rr_rig_parent_valid(b,map,parents,count)) {
                map[b]=-1;changed=1;
            }
        if(!changed)break;
    }
}
static int rr_rig_mapping_valid(const int* map, const int* parents, int count) {
    for (int b=0; b<RR_CHAM_BONES; ++b) {
        int index=map[b];
        if (index<0) { if(rr_cham_bones[b].required)return 0; continue; }
        if (index>=count) return 0;
        for (int other=0; other<b; ++other) if (map[other]==index) return 0;
        if(!rr_rig_parent_valid(b,map,parents,count))return 0;
    }
    return 1;
}
#endif
