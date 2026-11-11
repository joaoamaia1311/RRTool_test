/* rr_los.c - Background line-of-sight snapshot management.
 * Capture static and moving geometry, publish it under an SRW lock, and return
 * UNKNOWN while captures are unavailable, stale or tied to a different world.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "rr_los.h"
#include "rr_los_capture.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include "menu_bridge.h"

static SRWLOCK g_lock=SRWLOCK_INIT;
static RR_LOS_CAPTURE* g_static;
static RR_LOS_RUNTIME* g_runtime;
static uint64_t g_world_id,g_data_id;
static DWORD g_static_time,g_runtime_time;
static volatile LONG g_running;
static HANDLE g_thread;

static void clear_locked(void) {
    rr_los_capture_free(g_static);rr_los_runtime_free(g_runtime);
    g_static=NULL;g_runtime=NULL;g_world_id=g_data_id=0;g_static_time=g_runtime_time=0;
}
static int source(uint64_t* world_out,uint64_t* data_out) {
    uint64_t world=game_world_addr(),battle=0,current_battle=0;
    game_scene_debug(&battle,NULL,NULL);
    if(!PL(world)||!u_is(battle,"BattleEntitiesModel")||rr_u64(battle+0x60)!=world)return 0;
    uint64_t scene=rr_u64(battle+0x10); /* BattleEntitiesModel._physicsSceneModel */
    if(!u_is(scene,"PhysicsSceneModel"))return 0;
    uint64_t data=rr_u64(scene+0x10);
    if(!u_is(data,"PhysicsData"))return 0;
    game_scene_debug(&current_battle,NULL,NULL);
    if(world!=game_world_addr()||battle!=current_battle)return 0;
    *world_out=world;*data_out=data;return 1;
}
static DWORD WINAPI los_worker(LPVOID unused) {
    (void)unused;DWORD last_static=0,last_runtime=0;
    while(InterlockedCompareExchange(&g_running,0,0)) {
        BR_Config* cfg=br_get();
        if(!cfg->aa_on||!cfg->aa_los_on) { InterlockedExchange((volatile LONG*)&cfg->telem_los_state,0);Sleep(100);continue; }
        uint64_t world=0,data=0;
        if(!source(&world,&data)) { InterlockedExchange((volatile LONG*)&cfg->telem_los_state,1);Sleep(100);continue; }
        uint64_t current_tree=rr_u64(data+0x18);
        int current_root=PL(current_tree)?rr_i32(current_tree+0x18):-1;
        RR_LOS_CAPTURE* retired=NULL;
        AcquireSRWLockExclusive(&g_lock);
        if(world!=g_world_id||data!=g_data_id) { clear_locked();g_world_id=world;g_data_id=data;last_static=last_runtime=0; }
        if(g_static && (g_static->tree_identity!=current_tree || g_static->geometry.root!=current_root)) {
            retired=g_static;g_static=NULL;g_static_time=0;last_static=0;
        }
        int has_static=g_static!=NULL,has_runtime=g_runtime!=NULL;
        ReleaseSRWLockExclusive(&g_lock);
        rr_los_capture_free(retired);
        DWORD now=GetTickCount();
        if((!has_static && (!last_static || now-last_static>=1000)) || (has_static && now-last_static>=3000)) {
            last_static=now;RR_LOS_CAPTURE* candidate=rr_los_capture_static(data);
            uint64_t w2=0,d2=0;
            if(candidate && source(&w2,&d2) && world==w2 && data==d2) {
                AcquireSRWLockExclusive(&g_lock);
                if(g_world_id==world && g_data_id==data) {
                    RR_LOS_CAPTURE* old=g_static;g_static=candidate;candidate=old;g_static_time=GetTickCount();
                }
                ReleaseSRWLockExclusive(&g_lock);
            }
            rr_los_capture_free(candidate);
        }
        now=GetTickCount();
        if((!has_runtime && (!last_runtime || now-last_runtime>=500)) || (has_runtime && now-last_runtime>=200)) {
            last_runtime=now;RR_LOS_RUNTIME* candidate=rr_los_capture_runtime(data);
            uint64_t w2=0,d2=0;
            if(candidate && source(&w2,&d2) && world==w2 && data==d2) {
                AcquireSRWLockExclusive(&g_lock);
                if(g_world_id==world && g_data_id==data) {
                    RR_LOS_RUNTIME* old=g_runtime;g_runtime=candidate;candidate=old;g_runtime_time=GetTickCount();
                }
                ReleaseSRWLockExclusive(&g_lock);
            }
            rr_los_runtime_free(candidate);
        }
        AcquireSRWLockShared(&g_lock);
        now=GetTickCount();
        int ready=g_static && g_runtime && g_world_id==world && now-g_static_time<5000 && now-g_runtime_time<600;
        ReleaseSRWLockShared(&g_lock);
        InterlockedExchange((volatile LONG*)&cfg->telem_los_state,ready?2:1);
        Sleep(40);
    }
    AcquireSRWLockExclusive(&g_lock);clear_locked();ReleaseSRWLockExclusive(&g_lock);
    return 0;
}
int rr_los_start(void) {
    if(InterlockedCompareExchange(&g_running,1,0)!=0)return 1;
    g_thread=CreateThread(NULL,0,los_worker,NULL,0,NULL);
    if(!g_thread){InterlockedExchange(&g_running,0);return 0;}return 1;
}
void rr_los_stop(void) {
    InterlockedExchange(&g_running,0);
    if(g_thread){WaitForSingleObject(g_thread,5000);CloseHandle(g_thread);g_thread=NULL;}
}
int rr_los_check(RR_V3 start,RR_V3 end) {
    int result=RR_LOS_UNKNOWN;
    AcquireSRWLockShared(&g_lock);
    DWORD now=GetTickCount();uint64_t world=game_world_addr();
    if(world && world==g_world_id && g_static && g_runtime && now-g_static_time<5000 && now-g_runtime_time<600) {
        int a=rr_los_static_trace(&g_static->geometry,start,end);
        int b=a==RR_LOS_BLOCKED ? RR_LOS_BLOCKED : rr_los_runtime_trace(g_runtime,start,end);
        if(b==RR_LOS_BLOCKED)result=RR_LOS_BLOCKED;
        else if(a==RR_LOS_CLEAR && b==RR_LOS_CLEAR)result=RR_LOS_CLEAR;
    }
    ReleaseSRWLockShared(&g_lock);return result;
}
