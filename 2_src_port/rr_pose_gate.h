/* rr_pose_gate.h - Confirm abrupt branch-pose changes over a bounded interval.
 * Quaternions use x,y,z,w order; tip positions use metres and times use
 * milliseconds. The hold function returns 1 to retain the accepted pose.
 */
#ifndef RR_POSE_GATE_H
#define RR_POSE_GATE_H
#include <math.h>
#include <string.h>
typedef struct {
    float accepted[4][4], candidate[4][4];
    float accepted_tip[3], candidate_tip[3];
    unsigned first, since;
    int ready, pending;
} RR_POSE_GATE;
static float rr_pose_similarity(const float* a,const float* b) {
    float dot=0,aa=0,bb=0;
    for(int i=0;i<4;++i) { dot+=a[i]*b[i]; aa+=a[i]*a[i]; bb+=b[i]*b[i]; }
    if(!isfinite(dot) || aa<.25f || bb<.25f)return 0;
    return fabsf(dot)/sqrtf(aa*bb);
}
/* Intermediate animation poses can persist for several reader samples (up to
 * 45 ms in a captured idle rig). Confirm an abrupt change across 48 ms, while
 * bounding fast continuous action to 64 ms. Small pose motion stays immediate. */
static int rr_pose_gate_hold(RR_POSE_GATE* state,const float q[4][4],int joints,const float tip[3],unsigned now,int reset) {
    if(reset) memset(state,0,sizeof(*state));
    if(joints<1 || joints>4)return state->ready;
    int abrupt=0,returned=1,consistent=1;
    float accepted_distance=0,candidate_distance=0;
    for(int i=0;i<3;++i) {
        if(!isfinite(tip[i]))return state->ready;
        float a=tip[i]-state->accepted_tip[i],b=tip[i]-state->candidate_tip[i];
        accepted_distance+=a*a;candidate_distance+=b*b;
    }
    if(accepted_distance>.0009f)abrupt=1; /* three centimetres */
    if(accepted_distance>.000225f)returned=0;
    if(candidate_distance>.000225f)consistent=0;
    for(int i=0;i<joints;++i) {
        float norm=0; for(int k=0;k<4;++k)norm+=q[i][k]*q[i][k];
        if(!isfinite(norm) || norm<.25f || norm>2.25f)return state->ready;
        float similarity=rr_pose_similarity(q[i],state->accepted[i]);
        if(similarity<.998630f)abrupt=1; /* six degrees */
        if(similarity<.999657f)returned=0; /* three degrees */
        if(rr_pose_similarity(q[i],state->candidate[i])<.999657f)consistent=0;
    }
    if(state->ready && (abrupt || state->pending) && !returned) {
        if(!state->pending) {
            state->pending=1; state->first=state->since=now;
            memcpy(state->candidate,q,sizeof(state->candidate));memcpy(state->candidate_tip,tip,sizeof(state->candidate_tip)); return 1;
        }
        if(!consistent) { memcpy(state->candidate,q,sizeof(state->candidate));memcpy(state->candidate_tip,tip,sizeof(state->candidate_tip));state->since=now; }
        if(now-state->first<64 && now-state->since<48)return 1;
    }
    memcpy(state->accepted,q,sizeof(state->accepted));state->ready=1;state->pending=0;
    memcpy(state->accepted_tip,tip,sizeof(state->accepted_tip));
    return 0;
}
#endif
