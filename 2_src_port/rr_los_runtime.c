/* rr_los_runtime.c - Capture moving bodies and trace their current shapes.
 * Unverified or unsupported geometry produces UNKNOWN rather than a clear ray.
 */
#include "rr_los_capture.h"
#include <math.h>
#include <string.h>

/* Local transforms belong to the shape and are composed with the body pose. */
static int runtime_box(const RR_LOS_RUNTIME* r,const RR_LOS_RUNTIME_BODY* body,RR_LOS_BOX* out) {
    if(body->id<0||body->id>=r->box_count||body->id>=r->box_local_count)return 0;
    const RR_LOS_RUNTIME_BOX* b=&r->boxes[body->id];
    memset(out,0,sizeof(*out));out->width=b->width;out->height=b->height;out->length=b->length;
    out->hit=b->hit;out->surface=b->surface;memcpy(out->pos,body->pos,sizeof(out->pos));memcpy(out->rot,body->rot,sizeof(out->rot));
    int local=r->box_local_ids[body->id];
    if(local<0)return 1;
    if(local>=r->local_count)return 0;
    const RR_LOS_LOCAL_TRANSFORM* t=&r->locals[local];
    float x=body->rot[0],y=body->rot[1],z=body->rot[2],w=body->rot[3];
    float norm=x*x+y*y+z*z+w*w;
    if(!isfinite(norm)||norm<0.25f||norm>2.25f)return 0;
    norm=1.0f/sqrtf(norm);x*=norm;y*=norm;z*=norm;w*=norm;
    float px=t->pos[0],py=t->pos[1],pz=t->pos[2];
    if(!isfinite(px)||!isfinite(py)||!isfinite(pz))return 0;
    float tx=2*(y*pz-z*py),ty=2*(z*px-x*pz),tz=2*(x*py-y*px);
    out->pos[0]+=px+w*tx+(y*tz-z*ty);
    out->pos[1]+=py+w*ty+(z*tx-x*tz);
    out->pos[2]+=pz+w*tz+(x*ty-y*tx);
    float a=t->rot[0],bq=t->rot[1],c=t->rot[2],d=t->rot[3];
    out->rot[0]=w*a+x*d+y*c-z*bq;out->rot[1]=w*bq-x*c+y*d+z*a;
    out->rot[2]=w*c+x*bq-y*a+z*d;out->rot[3]=w*d-x*a-y*bq-z*c;
    return 1;
}

static int body_ignored(const RR_LOS_RUNTIME_BODY* b) {
    /* Players are handled by the chosen target point rather than as map
       occluders. IgnoreQueries and DisabledShots match game body flags. */
    return !b->enabled || ((b->type&30)==4) || (b->type&32) || (b->type&128);
}
/* A conservative height-field envelope: every traversed cell must sit fully
 * below the ray. Ambiguous intersections remain unknown. Both possible array
 * index orders are included until the game's terrain layout is corroborated. */
static int terrain_trace(const RR_LOS_NODE* node,const RR_LOS_RUNTIME_BODY* b,const RR_LOS_TERRAIN* t,RR_V3 a,RR_V3 z) {
    if(!t||!t->heights||t->nx<2||t->nz<2||!isfinite(t->size_x)||!isfinite(t->size_z)||t->size_x<=0||t->size_z<=0)return RR_LOS_UNKNOWN;
    float ox=b->pos[0]+t->offset_min[0],oz=b->pos[2]+t->offset_min[2];
    if(!isfinite(ox)||!isfinite(oz)||fabsf(node->min[0]-ox)>1||fabsf(node->min[2]-oz)>1||
       fabsf(node->max[0]-(ox+t->size_x))>1||fabsf(node->max[2]-(oz+t->size_z))>1)return RR_LOS_UNKNOWN;
    float dx=z.x-a.x,dz=z.z-a.z,dy=z.y-a.y,enter=0,exit=1;
    float base[2]={a.x,a.z},dir[2]={dx,dz},lo[2]={ox,oz},hi[2]={ox+t->size_x,oz+t->size_z};
    for(int k=0;k<2;++k) {
        if(fabsf(dir[k])<1e-8f){if(base[k]<lo[k]||base[k]>hi[k])return RR_LOS_CLEAR;continue;}
        float u=(lo[k]-base[k])/dir[k],v=(hi[k]-base[k])/dir[k];if(u>v){float q=u;u=v;v=q;}
        if(u>enter)enter=u;if(v<exit)exit=v;if(enter>exit)return RR_LOS_CLEAR;
    }
    if(enter<0)enter=0;if(exit>1)exit=1;if(enter>exit)return RR_LOS_CLEAR;
    float sx=t->size_x/(t->nx-1),sz=t->size_z/(t->nz-1);
    float px=a.x+enter*dx,pz=a.z+enter*dz;
    int ix=(int)floorf((px-ox)/sx),iz=(int)floorf((pz-oz)/sz);
    if(ix<0)ix=0;if(iz<0)iz=0;if(ix>=t->nx-1)ix=t->nx-2;if(iz>=t->nz-1)iz=t->nz-2;
    int stepx=dx>0?1:(dx<0?-1:0),stepz=dz>0?1:(dz<0?-1:0);
    float cur=enter;
    for(int steps=0;steps<t->nx+t->nz+8 && cur<=exit+1e-6f;++steps) {
        if(ix<0||ix>=t->nx-1||iz<0||iz>=t->nz-1)return cur>=exit-1e-5f?RR_LOS_CLEAR:RR_LOS_UNKNOWN;
        float next=exit;
        if(stepx){float edge=ox+(stepx>0?ix+1:ix)*sx,tt=(edge-a.x)/dx;if(tt>=cur-1e-6f && tt<next)next=tt;}
        if(stepz){float edge=oz+(stepz>0?iz+1:iz)*sz,tt=(edge-a.z)/dz;if(tt>=cur-1e-6f && tt<next)next=tt;}
        float maxh=-3.4e38f;
        for(int j=0;j<2;++j)for(int i=0;i<2;++i) {
            float h1=t->heights[(size_t)(iz+j)*t->nx+ix+i],h2=t->heights[(size_t)(ix+i)*t->nz+iz+j];
            if(!isfinite(h1)||!isfinite(h2))return RR_LOS_UNKNOWN;
            if(h1>maxh)maxh=h1;if(h2>maxh)maxh=h2;
        }
        float y0=a.y+cur*dy,y1=a.y+next*dy;
        if(!isfinite(y0)||!isfinite(y1)||fminf(y0,y1)<=b->pos[1]+maxh+0.1f)return RR_LOS_UNKNOWN;
        if(next>=exit-1e-7f)return RR_LOS_CLEAR;
        int advance_x=0,advance_z=0;
        if(stepx){float edge=ox+(stepx>0?ix+1:ix)*sx,tt=(edge-a.x)/dx;advance_x=fabsf(tt-next)<1e-5f;}
        if(stepz){float edge=oz+(stepz>0?iz+1:iz)*sz,tt=(edge-a.z)/dz;advance_z=fabsf(tt-next)<1e-5f;}
        if(!advance_x&&!advance_z)return RR_LOS_UNKNOWN;
        if(advance_x)ix+=stepx;if(advance_z)iz+=stepz;cur=next;
    }
    return RR_LOS_UNKNOWN;
}
int rr_los_runtime_trace(const RR_LOS_RUNTIME* r,RR_V3 start,RR_V3 end) {
    if(!r||!r->static_nodes||!r->static_bodies||!r->dynamic_items||!r->dynamic_active||!r->dynamic_bodies||
       r->static_root < -1||r->static_root>=r->static_node_count)return RR_LOS_UNKNOWN;
    if(!isfinite(start.x)||!isfinite(start.y)||!isfinite(start.z)||
       !isfinite(end.x)||!isfinite(end.y)||!isfinite(end.z))return RR_LOS_UNKNOWN;
    int stack[256],n=0,visited=0,uncertain=0;
    if(r->static_root>=0)stack[n++]=r->static_root;
    while(n) {
        int index=stack[--n];if(index<0||index>=r->static_node_count||++visited>r->static_node_count)return RR_LOS_UNKNOWN;
        const RR_LOS_NODE* node=&r->static_nodes[index];int overlap=rr_los_aabb_segment(start,end,node->min,node->max);
        if(overlap==RR_LOS_UNKNOWN)return RR_LOS_UNKNOWN;
        if(overlap==RR_LOS_CLEAR)continue;
        if(node->left<0 && node->right<0) {
            if(node->body<0||node->body>=r->static_body_count){uncertain=1;continue;}
            const RR_LOS_RUNTIME_BODY* b=&r->static_bodies[node->body];
            if(b->fields[1]!=index){uncertain=1;continue;} /* SpatialStructId at 0x0c */
            if(body_ignored(b))continue;
            if(b->shape==2) {
                RR_LOS_BOX box;if(!runtime_box(r,b,&box)){uncertain=1;continue;}
                if(box.hit&64)continue;
                int hit=rr_los_box_segment(start,end,&box);
                if(hit==RR_LOS_BLOCKED)return hit;
                if(hit==RR_LOS_UNKNOWN)uncertain=1;
            } else if(b->shape==7 && b->id>=0 && b->id<r->terrain_count) {
                int hit=terrain_trace(node,b,&r->terrains[b->id],start,end);
                if(hit==RR_LOS_UNKNOWN)uncertain=1;
            } else {
                /* Terrain, capsules, hulls and compound shapes require
                   per-shape handling before they can prove a clear ray. */
                uncertain=1;
            }
        } else {
            if(node->left<0||node->right<0||n>253)return RR_LOS_UNKNOWN;
            stack[n++]=node->left;stack[n++]=node->right;
        }
    }
    for(int i=0;i<r->dynamic_item_count;++i) {
        const RR_LOS_GRID_ITEM* item=&r->dynamic_items[i];if(!r->dynamic_active[i])continue;
        int overlap=rr_los_aabb_segment(start,end,item->min,item->max);
        if(overlap==RR_LOS_UNKNOWN)return RR_LOS_UNKNOWN;
        if(overlap==RR_LOS_CLEAR)continue;
        if(item->body<0||item->body>=r->dynamic_body_count){uncertain=1;continue;}
        const RR_LOS_RUNTIME_BODY* b=&r->dynamic_bodies[item->body];
        if(b->fields[1]!=i){uncertain=1;continue;}
        if(!body_ignored(b))uncertain=1;
    }
    return uncertain?RR_LOS_UNKNOWN:RR_LOS_CLEAR;
}
