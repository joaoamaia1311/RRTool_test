/* rr_los.h - Lifecycle and three-state line-of-sight query interface. */
#ifndef RR_LOS_H
#define RR_LOS_H
#include "rr_game.h"
#include "rr_los_geom.h"
/* Reader-owned background worker; returns UNKNOWN while snapshots are stale. */
int rr_los_start(void);
void rr_los_stop(void);
int rr_los_check(RR_V3 start, RR_V3 end);
#endif
