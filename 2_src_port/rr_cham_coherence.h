/* Brief hold for coordinated, abrupt upper-body pose changes. */
#ifndef RR_CHAM_COHERENCE_H
#define RR_CHAM_COHERENCE_H
#include <math.h>
#include "rr_cham_types.h"
static int rr_cham_coherence_hold(const float (*old)[12],const float (*cur)[12],
                                  float old_yaw,float cur_yaw,unsigned now,
                                  unsigned age,unsigned* since,int* pending) {
    const int bones[3]={RR_RIG_M_HEAD_1_JNT,RR_RIG_L_ARM_1_JNT,RR_RIG_R_ARM_1_JNT};
    float yaw=fabsf(cur_yaw-old_yaw);
    if (!isfinite(yaw) || age>=100) { *pending=0; return 0; }
    yaw=fmodf(yaw,360.0f); if (yaw>180.0f) yaw=360.0f-yaw;
    if (yaw>8.0f) { *pending=0; return 0; }
    int changed=0;
    for (int b=0;b<3;++b) {
        float sum=0;
        for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
            int i=row*4+col; float v=cur[bones[b]][i]-old[bones[b]][i]; sum+=v*v;
        }
        if (!isfinite(sum)) { *pending=0; return 0; }
        if (sum>.16f) ++changed;
    }
    if (changed<2) { *pending=0; return 0; }
    if (!*pending) { *pending=1; *since=now; return 1; }
    if (now-*since<40) return 1;
    *pending=0; return 0;
}
#endif
