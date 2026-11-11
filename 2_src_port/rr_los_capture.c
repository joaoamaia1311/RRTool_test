/* rr_los_capture.c - Copy bounded static physics data into reader-owned memory.
 * Recheck source identities after reading to reject scene changes mid-capture.
 */
#include "rr_los_capture.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include <stdlib.h>
#include <string.h>

#define LIMIT_NODES 1048576u
#define LIMIT_SHAPES 1048576u
#define LIMIT_HULLS 65536u

static int read_chunked(uint64_t at,void* out,size_t size) {
    if(!PL(at)||!out||!size)return 0;
    for(size_t offset=0;offset<size;) {
        size_t n=size-offset;if(n>0x10000)n=0x10000;
        if(!rr_read(at+offset,(char*)out+offset,n))return 0;
        offset+=n;
    }
    return 1;
}
static int class_has(uint64_t obj,const char* expected) {
    char name[96];return PL(obj) && u_class_name(rr_u64(obj),name,sizeof(name)) && strstr(name,expected)!=NULL;
}
/* Capture a managed array with bounded capacity. */
static void* read_array(uint64_t array,size_t stride,unsigned limit,int* count) {
    uint64_t n=0;*count=0;
    if(!PL(array)||!rr_read(array+U_ARR_LEN,&n,8)||n>limit||n>SIZE_MAX/stride)return NULL;
    if(n==0)return calloc(1,1);
    void* p=malloc((size_t)n*stride);if(!p)return NULL;
    if(!read_chunked(array+U_ARR_DATA,p,(size_t)n*stride)){free(p);return NULL;}
    *count=(int)n;return p;
}
static void* read_list_nodes(uint64_t list,size_t stride,unsigned limit,int* count,uint64_t* array_identity) {
    if(!class_has(list,"PhysicsLinkedListData"))return NULL;
    uint64_t array=rr_u64(list+0x10); if(array_identity)*array_identity=array;
    return read_array(array,stride,limit,count);
}
void rr_los_capture_free(RR_LOS_CAPTURE* c) {
    if(!c)return;
    RR_LOS_STATIC* s=&c->geometry;
    if(s->hull_data)for(int i=0;i<s->hull_data_count;++i)free(s->hull_data[i].planes);
    free(s->hull_data);free(s->hulls);free(s->capsules);free(s->triangles);free(s->boxes);free(s->bodies);free(s->nodes);free(c);
}
RR_LOS_CAPTURE* rr_los_capture_static(uint64_t data) {
    if(!class_has(data,"PhysicsData"))return NULL;
    uint64_t tree=rr_u64(data+0x18),shapes=rr_u64(data+0x20),bodies=rr_u64(data+0x28);
    if(!class_has(tree,"BVHTreeData") || !class_has(shapes,"PhysicsStaticShapesData"))return NULL;
    RR_LOS_CAPTURE* c=(RR_LOS_CAPTURE*)calloc(1,sizeof(*c));if(!c)return NULL;
    RR_LOS_STATIC* s=&c->geometry;c->physics_data=data;c->tree_identity=tree;
    uint64_t tree_array=rr_u64(tree+0x10),shape_list[5],shape_array[5]={0},body_array=0;
    s->root=rr_i32(tree+0x18);
    s->nodes=(RR_LOS_NODE*)read_array(tree_array,sizeof(*s->nodes),LIMIT_NODES,&s->node_count);
    s->bodies=(RR_LOS_BODY*)read_list_nodes(bodies,sizeof(*s->bodies),LIMIT_SHAPES,&s->body_count,&body_array);
    for(int i=0;i<5;++i)shape_list[i]=rr_u64(shapes+0x10+(uint64_t)i*8);
    s->boxes=(RR_LOS_BOX*)read_list_nodes(shape_list[0],sizeof(*s->boxes),LIMIT_SHAPES,&s->box_count,&shape_array[0]);
    s->triangles=(RR_LOS_TRI*)read_list_nodes(shape_list[1],sizeof(*s->triangles),LIMIT_SHAPES,&s->triangle_count,&shape_array[1]);
    s->capsules=(RR_LOS_CAPSULE*)read_list_nodes(shape_list[2],sizeof(*s->capsules),LIMIT_SHAPES,&s->capsule_count,&shape_array[2]);
    s->hulls=(RR_LOS_HULL*)read_list_nodes(shape_list[3],sizeof(*s->hulls),LIMIT_SHAPES,&s->hull_count,&shape_array[3]);
    int count=0;unsigned char* raw=(unsigned char*)read_list_nodes(shape_list[4],96,LIMIT_HULLS,&count,&shape_array[4]);
    if(!s->nodes||!s->bodies||!s->boxes||!s->triangles||!s->capsules||!s->hulls||!raw||
       s->root < -1||s->root>=s->node_count){free(raw);rr_los_capture_free(c);return NULL;}
    s->hull_data=(RR_LOS_HULL_DATA*)calloc(count?(size_t)count:1,sizeof(*s->hull_data));s->hull_data_count=count;
    if(!s->hull_data){free(raw);rr_los_capture_free(c);return NULL;}
    for(int i=0;i<count;++i) {
        const unsigned char* h=raw+(size_t)i*96;uint64_t planes=0;int n=0,faces=0;
        memcpy(&faces,h+8,4);memcpy(&planes,h+0x40,8);memcpy(&n,h+0x48,4);
        if(!PL(planes)||n<4||n>1024||faces!=n)continue;
        RR_LOS_PLANE* p=(RR_LOS_PLANE*)malloc((size_t)n*sizeof(*p));if(!p)continue;
        if(!read_chunked(planes,p,(size_t)n*sizeof(*p))){free(p);continue;}
        s->hull_data[i].planes=p;s->hull_data[i].count=n;
    }
    free(raw);
    /* Reject a scene or backing array that changed during capture. */
    if(rr_u64(data+0x18)!=tree || rr_u64(data+0x20)!=shapes || rr_u64(data+0x28)!=bodies ||
       rr_u64(tree+0x10)!=tree_array || rr_i32(tree+0x18)!=s->root || rr_u64(bodies+0x10)!=body_array) {
        rr_los_capture_free(c);return NULL;
    }
    for(int i=0;i<5;++i) if(rr_u64(shapes+0x10+(uint64_t)i*8)!=shape_list[i]||rr_u64(shape_list[i]+0x10)!=shape_array[i]) {
        rr_los_capture_free(c);return NULL;
    }
    return c;
}

typedef char runtime_body_size[sizeof(RR_LOS_RUNTIME_BODY)==204?1:-1];
typedef char runtime_item_size[sizeof(RR_LOS_GRID_ITEM)==40?1:-1];
typedef char runtime_box_size[sizeof(RR_LOS_RUNTIME_BOX)==28?1:-1];
typedef char runtime_local_size[sizeof(RR_LOS_LOCAL_TRANSFORM)==28?1:-1];

void rr_los_runtime_free(RR_LOS_RUNTIME* r) {
    if(!r)return;
    if(r->terrains)for(int i=0;i<r->terrain_count;++i)free(r->terrains[i].heights);
    free(r->terrains);
    free(r->static_nodes);free(r->static_bodies);free(r->boxes);free(r->box_local_ids);
    free(r->locals);free(r->dynamic_items);free(r->dynamic_active);free(r->dynamic_bodies);free(r);
}
RR_LOS_RUNTIME* rr_los_capture_runtime(uint64_t data) {
    if(!class_has(data,"PhysicsData"))return NULL;
    uint64_t tree=rr_u64(data+0x38),shapes=rr_u64(data+0x40),bodies=rr_u64(data+0x48);
    uint64_t grid=rr_u64(data+0x58),dynbodies=rr_u64(data+0x68);
    if(!class_has(tree,"BVHTreeData")||!class_has(shapes,"PhysicsShapesData")||!class_has(grid,"PhysicsRegularGridData"))return NULL;
    RR_LOS_RUNTIME* r=(RR_LOS_RUNTIME*)calloc(1,sizeof(*r));if(!r)return NULL;
    uint64_t tree_array=rr_u64(tree+0x10),ba=0,da=0,ia=0,boxa=0,localids=0,locala=0,terraina=0;
    r->static_root=rr_i32(tree+0x18);
    r->static_nodes=(RR_LOS_NODE*)read_array(tree_array,sizeof(*r->static_nodes),LIMIT_NODES,&r->static_node_count);
    r->static_bodies=(RR_LOS_RUNTIME_BODY*)read_list_nodes(bodies,sizeof(*r->static_bodies),LIMIT_SHAPES,&r->static_body_count,&ba);
    r->dynamic_bodies=(RR_LOS_RUNTIME_BODY*)read_list_nodes(dynbodies,sizeof(*r->dynamic_bodies),LIMIT_SHAPES,&r->dynamic_body_count,&da);
    uint64_t items=rr_u64(grid+0x38);
    r->dynamic_items=(RR_LOS_GRID_ITEM*)read_list_nodes(items,sizeof(*r->dynamic_items),LIMIT_SHAPES,&r->dynamic_item_count,&ia);
    uint64_t itemlinks=PL(items)?rr_u64(items+0x18):0, next_array=PL(itemlinks)?rr_u64(itemlinks+0x20):0;
    int root=PL(itemlinks)?rr_i32(itemlinks+0x10):-1, active_count=PL(itemlinks)?rr_i32(itemlinks+0x28):-1, next_count=0;
    int* next=(int*)read_array(next_array,sizeof(int),LIMIT_SHAPES,&next_count);
    if(!class_has(itemlinks,"PhysicsLinkedListLinksData") || !r->dynamic_items || !next ||
       active_count<0 || active_count>r->dynamic_item_count || next_count<r->dynamic_item_count) { free(next);goto fail; }
    r->dynamic_active=(uint8_t*)calloc(r->dynamic_item_count?r->dynamic_item_count:1,1);
    if(!r->dynamic_active){free(next);goto fail;}
    int cursor=root,walked=0;
    while(cursor>=0 && cursor<r->dynamic_item_count && !r->dynamic_active[cursor] && walked<=active_count) {
        r->dynamic_active[cursor]=1;++walked;cursor=next[cursor];
    }
    free(next);
    if(cursor!=-1 || walked!=active_count)goto fail;
    uint64_t boxlist=rr_u64(shapes+0x20),locallist=rr_u64(shapes+0x60);
    if(!class_has(boxlist,"PhysicsShapeLinkedListData"))goto fail;
    boxa=rr_u64(boxlist+0x10);localids=rr_u64(boxlist+0x18);
    r->boxes=(RR_LOS_RUNTIME_BOX*)read_array(boxa,sizeof(*r->boxes),LIMIT_SHAPES,&r->box_count);
    r->box_local_ids=(int*)read_array(localids,sizeof(int),LIMIT_SHAPES,&r->box_local_count);
    r->locals=(RR_LOS_LOCAL_TRANSFORM*)read_list_nodes(locallist,sizeof(*r->locals),LIMIT_SHAPES,&r->local_count,&locala);
    if(!r->static_nodes||!r->static_bodies)goto fail;
    uint64_t terrainlist=rr_u64(shapes+0x48);int terrain_count=0;
    unsigned char* rawterrain=(unsigned char*)read_list_nodes(terrainlist,96,65536,&terrain_count,&terraina);
    if(!rawterrain)goto fail;
    r->terrains=(RR_LOS_TERRAIN*)calloc(terrain_count?terrain_count:1,sizeof(*r->terrains));r->terrain_count=terrain_count;
    if(!r->terrains){free(rawterrain);goto fail;}
    uint8_t* terrain_needed=(uint8_t*)calloc(terrain_count?terrain_count:1,1);
    if(!terrain_needed){free(rawterrain);goto fail;}
    for(int i=0;i<r->static_node_count;++i) {
        RR_LOS_NODE* node=&r->static_nodes[i];
        if(node->left>=0||node->right>=0||node->body<0||node->body>=r->static_body_count)continue;
        RR_LOS_RUNTIME_BODY* b=&r->static_bodies[node->body];
        if(b->enabled&&b->fields[1]==i&&b->shape==7&&b->id>=0&&b->id<terrain_count)terrain_needed[b->id]=1;
    }
    for(int i=0;i<terrain_count;++i) {
        if(!terrain_needed[i])continue;
        const unsigned char* p=rawterrain+(size_t)i*96;RR_LOS_TERRAIN* t=&r->terrains[i];uint64_t heights=0;int count=0;
        memcpy(&t->size_x,p,4);memcpy(&t->size_z,p+4,4);memcpy(&t->nx,p+8,4);memcpy(&t->nz,p+12,4);memcpy(&heights,p+0x18,8);
        memcpy(t->offset_min,p+0x40,12);memcpy(t->offset_max,p+0x4c,12);
        if(t->nx<2||t->nz<2||t->nx>8192||t->nz>8192||(uint64_t)t->nx*t->nz>16777216u)continue;
        t->heights=(float*)read_array(heights,sizeof(float),16777216u,&count);
        if(count!=t->nx*t->nz){free(t->heights);t->heights=NULL;}
    }
    free(terrain_needed);free(rawterrain);
    if(!r->static_nodes||!r->static_bodies||!r->dynamic_bodies||!r->dynamic_items||!r->boxes||!r->box_local_ids||!r->locals||
       r->static_root < -1||r->static_root>=r->static_node_count||r->box_local_count<r->box_count)goto fail;
    if(rr_u64(data+0x38)!=tree||rr_u64(data+0x40)!=shapes||rr_u64(data+0x48)!=bodies||
       rr_u64(data+0x58)!=grid||rr_u64(data+0x68)!=dynbodies||rr_u64(tree+0x10)!=tree_array||
       rr_i32(tree+0x18)!=r->static_root||rr_u64(bodies+0x10)!=ba||rr_u64(dynbodies+0x10)!=da||
       rr_u64(grid+0x38)!=items||rr_u64(items+0x10)!=ia||rr_u64(items+0x18)!=itemlinks||
       rr_u64(itemlinks+0x20)!=next_array||rr_i32(itemlinks+0x10)!=root||rr_i32(itemlinks+0x28)!=active_count||rr_u64(shapes+0x20)!=boxlist||
       rr_u64(boxlist+0x10)!=boxa||rr_u64(boxlist+0x18)!=localids||rr_u64(shapes+0x60)!=locallist||rr_u64(locallist+0x10)!=locala||
       rr_u64(shapes+0x48)!=terrainlist||rr_u64(terrainlist+0x10)!=terraina)goto fail;
    return r;
fail: rr_los_runtime_free(r);return NULL;
}
