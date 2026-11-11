#ifndef RR_BODY_JOINTS_H
#define RR_BODY_JOINTS_H
#include "rr_cham_types.h"
/* Attachment helpers remain available in the rig palette, but are not body
 * joints. A parked weapon helper can sit at the character origin. */
static int rr_rig_is_body_joint(int bone) {
    return bone>=0 && bone<RR_CHAM_BONES &&
           bone!=RR_RIG_WEAPONE1_JNT && bone!=RR_RIG_MAGAZINE1_JNT &&
           bone!=RR_RIG_BAG_JNT && bone!=RR_RIG_L_BAG_JNT && bone!=RR_RIG_R_BAG_JNT;
}
#endif
