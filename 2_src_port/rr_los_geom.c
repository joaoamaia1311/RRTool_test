/* rr_los_geom.c - Segment intersection tests against captured physics geometry.
 * This file operates on local data; it does not read target-process memory.
 */
#include "rr_los_geom.h"
#include <math.h>

typedef char assert_node_size[sizeof(RR_LOS_NODE)==40?1:-1];
typedef char assert_body_size[sizeof(RR_LOS_BODY)==16?1:-1];
typedef char assert_box_size[sizeof(RR_LOS_BOX)==56?1:-1];
typedef char assert_tri_size[sizeof(RR_LOS_TRI)==52?1:-1];
typedef char assert_capsule_size[sizeof(RR_LOS_CAPSULE)==64?1:-1];
typedef char assert_hull_size[sizeof(RR_LOS_HULL)==48?1:-1];

static RR_V3 sub(RR_V3 a,RR_V3 b) { RR_V3 v={a.x-b.x,a.y-b.y,a.z-b.z};return v; }
static float dot(RR_V3 a,RR_V3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static RR_V3 cross(RR_V3 a,RR_V3 b) { RR_V3 v={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};return v; }
static RR_V3 plus(RR_V3 a,RR_V3 b) { RR_V3 v={a.x+b.x,a.y+b.y,a.z+b.z};return v; }
static RR_V3 scale(RR_V3 a,float t) { RR_V3 v={a.x*t,a.y*t,a.z*t};return v; }
static float clamp01(float a) { return a<0?0:(a>1?1:a); }
static int finite3(RR_V3 a) { return isfinite(a.x)&&isfinite(a.y)&&isfinite(a.z); }
static RR_V3 arr(const float v[3]) { RR_V3 a={v[0],v[1],v[2]};return a; }

/* Transform a world point to the shape's local frame. */
static int local_point(RR_V3 v,const float position[3],const float rotation[4],RR_V3* out) {
    float x=rotation[0],y=rotation[1],z=rotation[2],w=rotation[3];
    float n=x*x+y*y+z*z+w*w;
    if(!finite3(v)||!finite3(arr(position))||!isfinite(n)||n<0.25f||n>2.25f)return 0;
    n=1.0f/sqrtf(n);x*=-n;y*=-n;z*=-n;w*=n;
    RR_V3 p=sub(v,arr(position)),q={x,y,z},t=cross(q,p);
    t.x*=2;t.y*=2;t.z*=2;RR_V3 s=cross(q,t);
    out->x=p.x+w*t.x+s.x;out->y=p.y+w*t.y+s.y;out->z=p.z+w*t.z+s.z;
    return finite3(*out);
}

int rr_los_aabb_segment(RR_V3 start,RR_V3 end,const float lo[3],const float hi[3]) {
    if(!finite3(start)||!finite3(end))return RR_LOS_UNKNOWN;
    float a[3]={start.x,start.y,start.z},b[3]={end.x,end.y,end.z},tmin=0,tmax=1;
    for(int i=0;i<3;++i) {
        if(!isfinite(lo[i])||!isfinite(hi[i])||lo[i]>hi[i])return RR_LOS_UNKNOWN;
        float d=b[i]-a[i];
        if(fabsf(d)<1e-8f) { if(a[i]<lo[i]||a[i]>hi[i])return RR_LOS_CLEAR; continue; }
        float x=(lo[i]-a[i])/d,y=(hi[i]-a[i])/d;
        if(x>y){float t=x;x=y;y=t;}
        if(x>tmin)tmin=x;if(y<tmax)tmax=y;
        if(tmax<tmin)return RR_LOS_CLEAR;
    }
    return RR_LOS_BLOCKED;
}

int rr_los_box_segment(RR_V3 start,RR_V3 end,const RR_LOS_BOX* box) {
    RR_V3 a,b;
    if(!box || !local_point(start,box->pos,box->rot,&a)||!local_point(end,box->pos,box->rot,&b)||
       !isfinite(box->width)||!isfinite(box->height)||!isfinite(box->length)||
       box->width<=0||box->height<=0||box->length<=0)return RR_LOS_UNKNOWN;
    float lo[3]={-box->width*.5f,-box->height*.5f,-box->length*.5f};
    float hi[3]={-lo[0],-lo[1],-lo[2]};
    return rr_los_aabb_segment(a,b,lo,hi);
}

int rr_los_triangle_segment(RR_V3 start,RR_V3 end,const RR_LOS_TRI* t) {
    if(!t || !finite3(start)||!finite3(end)||!finite3(arr(t->a))||!finite3(arr(t->b))||!finite3(arr(t->c)))return RR_LOS_UNKNOWN;
    RR_V3 d=sub(end,start),e1=sub(arr(t->b),arr(t->a)),e2=sub(arr(t->c),arr(t->a)),h=cross(d,e2);
    float det=dot(e1,h); if(fabsf(det)<1e-8f)return RR_LOS_CLEAR;
    float inv=1.0f/det;RR_V3 s=sub(start,arr(t->a));float u=inv*dot(s,h);
    if(u<0||u>1)return RR_LOS_CLEAR;
    RR_V3 q=cross(s,e1);float v=inv*dot(d,q);if(v<0||u+v>1)return RR_LOS_CLEAR;
    float tval=inv*dot(e2,q);return tval>=0.0001f && tval<=0.9999f?RR_LOS_BLOCKED:RR_LOS_CLEAR;
}

int rr_los_capsule_segment(RR_V3 start,RR_V3 end,const RR_LOS_CAPSULE* c) {
    if(!c||!finite3(start)||!finite3(end)||!finite3(arr(c->pos))||!finite3(arr(c->up))||
       !isfinite(c->length)||!isfinite(c->radius)||c->length<0||c->radius<=0)return RR_LOS_UNKNOWN;
    RR_V3 axis=arr(c->up);float axis_length=sqrtf(dot(axis,axis));
    if(axis_length<0.5f||axis_length>1.5f)return RR_LOS_UNKNOWN;
    axis=scale(axis,c->length/(2*axis_length));
    RR_V3 p=sub(arr(c->pos),axis),q=plus(arr(c->pos),axis),d1=sub(end,start),d2=sub(q,p),r=sub(start,p);
    float a=dot(d1,d1),e=dot(d2,d2),f=dot(d2,r),s=0,t=0;
    if(a<1e-10f)return RR_LOS_UNKNOWN;
    if(e<1e-10f)s=clamp01(-dot(d1,r)/a);
    else {
        float v=dot(d1,d2),c0=dot(d1,r),den=a*e-v*v;
        s=fabsf(den)>1e-10f?clamp01((v*f-c0*e)/den):0;
        t=(v*s+f)/e;
        if(t<0){t=0;s=clamp01(-c0/a);}else if(t>1){t=1;s=clamp01((v-c0)/a);}
    }
    RR_V3 near1=plus(start,scale(d1,s)),near2=plus(p,scale(d2,t)),diff=sub(near1,near2);
    return dot(diff,diff)<=c->radius*c->radius?RR_LOS_BLOCKED:RR_LOS_CLEAR;
}

int rr_los_hull_segment(RR_V3 start,RR_V3 end,const RR_LOS_HULL* h,const RR_LOS_HULL_DATA* hull) {
    RR_V3 a,b;
    if(!h||!hull||!hull->planes||hull->count<4||hull->count>1024||
       !local_point(start,h->pos,h->rot,&a)||!local_point(end,h->pos,h->rot,&b))return RR_LOS_UNKNOWN;
    RR_V3 dir=sub(b,a); float lo=0,hi=1;
    for(int i=0;i<hull->count;++i) {
        const RR_LOS_PLANE* p=&hull->planes[i]; RR_V3 n=arr(p->normal);
        if(!finite3(n)||!isfinite(p->distance)||dot(n,n)<0.01f)return RR_LOS_UNKNOWN;
        float num=p->distance-dot(n,a),den=dot(n,dir);
        if(fabsf(den)<1e-8f){if(num<0)return RR_LOS_CLEAR;continue;}
        float t=num/den;
        if(den>0){if(t<hi)hi=t;}else{if(t>lo)lo=t;}
        if(lo>hi)return RR_LOS_CLEAR;
    }
    return hi>=0.0001f && lo<=0.9999f?RR_LOS_BLOCKED:RR_LOS_CLEAR;
}

int rr_los_static_trace(const RR_LOS_STATIC* s,RR_V3 start,RR_V3 end) {
    if(!s||!s->nodes||!s->bodies||s->root < -1||s->root>=s->node_count||!finite3(start)||!finite3(end))return RR_LOS_UNKNOWN;
    RR_V3 diff=sub(end,start);if(dot(diff,diff)<0.01f)return RR_LOS_UNKNOWN;
    if(s->root==-1)return RR_LOS_CLEAR; /* A captured tree with an empty root. */
    int stack[256],n=0,uncertain=0,visited=0;stack[n++]=s->root;
    while(n) {
        int index=stack[--n];
        if(index<0||index>=s->node_count||++visited>s->node_count)return RR_LOS_UNKNOWN;
        const RR_LOS_NODE* node=&s->nodes[index];
        int intersect=rr_los_aabb_segment(start,end,node->min,node->max);
        if(intersect==RR_LOS_UNKNOWN)return RR_LOS_UNKNOWN;
        if(intersect==RR_LOS_CLEAR)continue;
        if(node->left<0 && node->right<0) {
            if(node->body<0||node->body>=s->body_count){uncertain=1;continue;}
            const RR_LOS_BODY* b=&s->bodies[node->body];
            if(b->tree!=index){uncertain=1;continue;}
            /* IgnoreQueries and DisabledShots are game-defined body flags. */
            if((b->type&32)||(b->type&128))continue;
            int result=RR_LOS_UNKNOWN;
            if(b->shape==0 && b->id>=0 && b->id<s->box_count) {
                const RR_LOS_BOX* v=&s->boxes[b->id]; if(v->hit&64)continue; result=rr_los_box_segment(start,end,v);
            } else if(b->shape==1 && b->id>=0 && b->id<s->triangle_count) {
                const RR_LOS_TRI* v=&s->triangles[b->id]; if(v->hit&64)continue; result=rr_los_triangle_segment(start,end,v);
            } else if(b->shape==2 && b->id>=0 && b->id<s->capsule_count) {
                const RR_LOS_CAPSULE* v=&s->capsules[b->id]; if(v->hit&64)continue;
                result=rr_los_capsule_segment(start,end,v);
            } else if(b->shape==3 && b->id>=0 && b->id<s->hull_count) {
                const RR_LOS_HULL* v=&s->hulls[b->id]; if(v->hit&64)continue;
                if(v->hull>=0&&v->hull<s->hull_data_count)result=rr_los_hull_segment(start,end,v,&s->hull_data[v->hull]);
            }
            if(result==RR_LOS_BLOCKED)return result;
            if(result==RR_LOS_UNKNOWN)uncertain=1;
        } else {
            if(node->left<0||node->right<0||n>253)return RR_LOS_UNKNOWN;
            stack[n++]=node->left;stack[n++]=node->right;
        }
    }
    return uncertain?RR_LOS_UNKNOWN:RR_LOS_CLEAR;
}
