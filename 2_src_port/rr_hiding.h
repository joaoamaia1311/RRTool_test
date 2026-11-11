/* Read-only hiding evidence. Voice IDs remain candidates until corroborated
 * by the current hiding object's occupant. Missing reads never mean empty. */
#ifndef RR_HIDING_H
#define RR_HIDING_H
#include <string.h>
#include <math.h>
#include "rr_game.h"
#include "rr_unity.h"
#include "rr_mem.h"
#include "robrun_sdk_offsets.h"
#define RR_MAX_HIDE_OBJECTS 512
typedef struct {
    int32_t id;
    RR_V3 pos;                 /* object origin, used for distance/radar */
    RR_V3 anchor;              /* validated world interaction point or fallback */
    int has_interaction, occupant_known, occupant;
} RR_HIDE_OBJECT;
typedef struct {
    int id, voice_known, voice_hide_id;
    uint64_t description, model, data, identity;
    char user[128], name[RR_NAME_MAX]; /* user string stays inside reader */
} RR_HIDE_SLOT;
typedef struct {
    int nobjects, nslots, slots_known, voice_known, voice_participants;
    RR_HIDE_OBJECT objects[RR_MAX_HIDE_OBJECTS];
    RR_HIDE_SLOT slots[RR_MAX_PLAYERS];
} RR_HIDE_SNAPSHOT;
static int rr_hide_position_valid(RR_V3 p) {
    return isfinite(p.x)&&isfinite(p.y)&&isfinite(p.z)&&fabsf(p.x)<=500000&&fabsf(p.y)<=500000&&fabsf(p.z)<=500000;
}
/* Location InteractPoint was observed in world space on the current map.
 * Reject local-space/default/implausible points instead of transforming guesses. */
static RR_V3 rr_hide_anchor(RR_V3 origin,RR_V3 interaction,int readable,int* used) {
    float x=interaction.x-origin.x,y=interaction.y-origin.y,z=interaction.z-origin.z;
    *used=readable&&rr_hide_position_valid(interaction)&&x*x+y*y+z*z<=16.0f;
    if(*used)return interaction;
    origin.y+=1.5f;return origin;
}
/* Unlike u_dict(), distinguish a successfully read empty dictionary from a
 * failed read. Check the entry-array capacity before walking its used slots. */
static int rr_hide_dictionary(uint64_t dict,int* count,uint64_t* entries) {
    int n;uint64_t array,capacity;
    if(!u_is(dict,"Dictionary`2") || !rr_read(dict+U_DICT_COUNT,&n,4) || n<0 || n>4096)return 0;
    *count=n;*entries=0;if(!n)return 1;
    if(!rr_read(dict+U_DICT_ENTRIES,&array,8)||!PL(array)||!rr_read(array+U_ARR_LEN,&capacity,8)||capacity<(uint64_t)n||capacity>8192)return 0;
    *entries=array+U_ARR_DATA;return 1;
}
static int rr_hide_object_read(uint64_t object,int id,RR_HIDE_OBJECT* out) {
    uint64_t desc;int desc_id;RR_V3 interaction={0};
    memset(out,0,sizeof(*out));out->id=id;
    if(id<0||!u_is(object,"HideObjectModel")||!rr_read(object+RR_OFF_HIDE_DESC,&desc,8)||!u_is(desc,"LocationHideObjectDescription")||
       !rr_read(desc+RR_OFF_HIDE_DESC_ID,&desc_id,4)||desc_id!=id||!rr_read(desc+RR_OFF_HIDE_DESC_POS,&out->pos,sizeof(out->pos))||!rr_hide_position_valid(out->pos))return 0;
    int readable=rr_read(desc+RR_OFF_HIDE_DESC_INTERACT,&interaction,sizeof(interaction));
    out->anchor=rr_hide_anchor(out->pos,interaction,readable,&out->has_interaction);
    out->occupant_known=rr_read(object+RR_OFF_HIDE_OCCUPANT,&out->occupant,4)&&out->occupant>=-1;
    return 1;
}
static int rr_hide_objects_read(uint64_t world,RR_HIDE_OBJECT* out,int max) {
    uint64_t entries;int n,count=0;
    if(!u_is(world,"WorldEntitiesModel")||!rr_hide_dictionary(rr_u64(world+RR_OFF_WEM_HIDE_OBJECTS),&n,&entries))return -1;
    for(int i=0;i<n;++i) {
        unsigned char row[24];int hash,id;uint64_t object;
        if(!rr_read(entries+(uint64_t)i*24,row,24))return -1;
        memcpy(&hash,row,4);if(hash<0)continue;
        memcpy(&id,row+8,4);memcpy(&object,row+16,8);
        RR_HIDE_OBJECT item;if(!rr_hide_object_read(object,id,&item))continue;
        if(count>=max)return -1;out[count++]=item;
    }
    return count;
}
static int rr_hide_string(uint64_t object,char* out,size_t cap) {
    int n=0;out[0]=0;
    return u_is(object,"String")&&rr_read(object+U_STR_LEN,&n,4)&&n>0&&n<64&&u_string(object,out,cap)>0;
}
static uint64_t rr_hide_identity(const char* user) {
    uint64_t hash=14695981039346656037ULL;
    for(const unsigned char* p=(const unsigned char*)user;*p;++p){hash^=*p;hash*=1099511628211ULL;}
    return hash?hash:1;
}
static int rr_hide_slots_read(uint64_t battle_model,RR_HIDE_SLOT* out,int max) {
    uint64_t model=rr_u64(battle_model+RR_OFF_BATTLE_PLAYER_SLOTS),entries;int n,count=0;
    if(!u_is(model,"BattleCharacterSlotsModel")||!rr_hide_dictionary(rr_u64(model+RR_OFF_SLOTS_DICTIONARY),&n,&entries))return -1;
    for(int i=0;i<n;++i) {
        unsigned char row[24];int hash,id;uint64_t key,slot;
        if(!rr_read(entries+(uint64_t)i*24,row,24))return -1;
        memcpy(&hash,row,4);if(hash<0)continue;
        memcpy(&key,row+8,8);memcpy(&slot,row+16,8);
        if(!u_is(key,"BattleCharacterSlotDescription")||!u_is(slot,"BattleCharacterSlotModel")||rr_u64(slot+RR_OFF_SLOT_DESC)!=key)return -1;
        uint64_t data=rr_u64(slot+RR_OFF_SLOT_DATA);
        if(!u_is(data,"BattleCharacterSlotData"))return -1;
        uint64_t value=rr_u64(data+RR_OFF_SLOT_ENTITY_ID);
        if((!u_is(value,"BattleEntityIdData")&&!u_is(value,"IntData"))||!rr_read(value+0x18,&id,4)||id<-1)return -1;
        if(id<0)continue;
        if(count>=max)return -1;
        for(int j=0;j<count;++j)if(out[j].id==id||out[j].description==key)return -1;
        RR_HIDE_SLOT* item=&out[count++];memset(item,0,sizeof(*item));
        item->id=id;item->description=key;item->model=slot;item->data=data;item->voice_hide_id=-1;
        value=rr_u64(data+RR_OFF_SLOT_USER_ID);
        if(rr_hide_string(rr_u64(value+0x18),item->user,sizeof(item->user)))item->identity=rr_hide_identity(item->user);
        value=rr_u64(data+RR_OFF_SLOT_NICKNAME);u_string(rr_u64(value+0x18),item->name,sizeof(item->name));
    }
    return count;
}
/* ConcurrentDictionary<string, reference> layout used by this snapshot.
 * The dictionary table contains a bucket array at +0x10. These native/generic
 * layout assumptions need verification for a new runtime. Bound the walk,
 * reject duplicate nodes and recheck the table after reading.
 */
static int rr_hide_voice_read(uint64_t battle_model,RR_HIDE_SNAPSHOT* out) {
    for(int s=0;s<out->nslots;++s){out->slots[s].voice_known=0;out->slots[s].voice_hide_id=-1;}
    out->voice_participants=0;
    uint64_t room=rr_u64(battle_model+RR_OFF_BATTLE_VOICE_ROOM);
    uint64_t participants=u_is(room,"VoiceChatBattleRoomModel")?rr_u64(room+RR_OFF_VOICE_ROOM_PARTICIPANTS):0;
    uint64_t map=u_is(participants,"VoiceChatParticipantsModel")?rr_u64(participants+RR_OFF_VOICE_STRING_MAP):0;
    if(!u_is(map,"ConcurrentDictionary`2"))return 0;
    uint64_t tables=rr_u64(map+0x10),buckets=rr_u64(tables+0x10),capacity;
    if(!u_is(tables,"Tables")||!u_is(buckets,"Node[]")||!rr_read(buckets+U_ARR_LEN,&capacity,8)||!capacity||capacity>4096)return 0;
    uint64_t heads[4096],visited[RR_MAX_PLAYERS*2];int nvisited=0;
    int found[RR_MAX_PLAYERS]={0},hide_values[RR_MAX_PLAYERS]={0};
    if(!rr_read(buckets+U_ARR_DATA,heads,(size_t)capacity*8))return 0;
    for(unsigned i=0;i<capacity;++i)for(uint64_t node=heads[i];node;) {
        if(!u_is(node,"Node")||nvisited>=RR_MAX_PLAYERS*2)return 0;
        for(int j=0;j<nvisited;++j)if(visited[j]==node)return 0;
        visited[nvisited++]=node;
        uint64_t fields[3];if(!rr_read(node+0x10,fields,sizeof(fields)))return 0;
        uint64_t p=fields[1];char key[128],user[128];int hide;
        if(!u_is(p,"VoiceChatBattleParticipantModel")||!rr_hide_string(fields[0],key,sizeof(key))||
           !rr_hide_string(rr_u64(p+RR_OFF_VOICE_USER_ID),user,sizeof(user))||strcmp(key,user)||!rr_read(p+RR_OFF_VOICE_HIDE_ID,&hide,4)||hide<-1)return 0;
        ++out->voice_participants;
        uint64_t description=rr_u64(p+RR_OFF_VOICE_SLOT);
        for(int s=0;s<out->nslots;++s) {
            RR_HIDE_SLOT* slot=&out->slots[s];
            if(slot->description!=description||!slot->identity||strcmp(slot->user,user))continue;
            if(found[s]++)return 0;
            uint64_t data=rr_u64(slot->model+RR_OFF_SLOT_DATA),value=rr_u64(data+RR_OFF_SLOT_ENTITY_ID);
            int id,again;
            if(data!=slot->data||!rr_read(value+0x18,&id,4)||id!=slot->id||
               !rr_read(p+RR_OFF_VOICE_HIDE_ID,&again,4)||again!=hide||rr_u64(node+0x18)!=p)return 0;
            hide_values[s]=hide;
        }
        node=fields[2];
    }
    if(rr_u64(map+0x10)!=tables||rr_u64(tables+0x10)!=buckets||rr_u64(battle_model+RR_OFF_BATTLE_VOICE_ROOM)!=room)return 0;
    for(int s=0;s<out->nslots;++s)if(found[s]){out->slots[s].voice_known=1;out->slots[s].voice_hide_id=hide_values[s];}
    return 1;
}
static int rr_hide_read(uint64_t battle_model,uint64_t world,RR_HIDE_SNAPSHOT* out) {
    memset(out,0,sizeof(*out));
    int n=rr_hide_objects_read(world,out->objects,RR_MAX_HIDE_OBJECTS);if(n<0)return 0;out->nobjects=n;
    if(!u_is(battle_model,"BattleModel"))return 1;
    uint64_t battle=rr_u64(battle_model+RR_OFF_BATTLE_ENTITIES);
    if(!u_is(battle,"BattleEntitiesModel")||rr_u64(battle+RR_OFF_BATTLE_WORLD)!=world)return 1;
    n=rr_hide_slots_read(battle_model,out->slots,RR_MAX_PLAYERS);
    if(n>=0) {
        out->nslots=n;out->slots_known=1;out->voice_known=rr_hide_voice_read(battle_model,out);
        if(!out->voice_known)for(int i=0;i<n;++i)out->slots[i].voice_known=0;
    }
    if(rr_u64(battle_model+RR_OFF_BATTLE_ENTITIES)!=battle||rr_u64(battle+RR_OFF_BATTLE_WORLD)!=world)return 0;
    return 1;
}
#endif
