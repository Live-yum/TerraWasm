/* Lazy parity rules. No RV32I instruction decoding is present in this runtime:
 * CPU programs execute only by pulsing the imported world wiring. */
#include "terra_circuit_world_internal.h"
#include <limits.h>
#include <string.h>

static void random_seed(CxWorld* w,int32_t seed){
    int32_t sub=seed==INT32_MIN?INT32_MAX:(seed<0?-seed:seed);int32_t mj=161803398-sub;w->rng[55]=mj;int32_t mk=1;
    for(int32_t i=1;i<55;i++){int32_t ii=(21*i)%55;w->rng[ii]=mk;mk=(int32_t)((uint32_t)mj-(uint32_t)mk);if(mk<0)mk=(int32_t)((uint32_t)mk+(uint32_t)INT32_MAX);mj=w->rng[ii];}
    for(int k=1;k<5;k++)for(int i=1;i<56;i++){w->rng[i]=(int32_t)((uint32_t)w->rng[i]-(uint32_t)w->rng[1+(i+30)%55]);if(w->rng[i]<0)w->rng[i]=(int32_t)((uint32_t)w->rng[i]+(uint32_t)INT32_MAX);}
    w->rng_i=0;w->rng_j=21;
}
static uint32_t random_next(CxWorld* w,uint32_t maximum){
    uint32_t i=w->rng_i+1,j=w->rng_j+1;if(i>=56)i=1;if(j>=56)j=1;int32_t value=(int32_t)((uint32_t)w->rng[i]-(uint32_t)w->rng[j]);
    if(value==INT32_MAX)--value;if(value<0)value=(int32_t)((uint32_t)value+(uint32_t)INT32_MAX);w->rng[i]=value;w->rng_i=i;w->rng_j=j;
    return (uint32_t)((double)value*(1.0/(double)INT32_MAX)*(double)maximum);
}
static uint32_t record_end(CxWorld* w,uint32_t at){uint8_t h=cx_bytes_get(&w->code,at++);if(h&128u){cx_var_read(&w->code,&at);return at;}uint32_t n=(h&7u)+((h>>3)&7u);while(n--)cx_var_read(&w->code,&at);return at;}
static uint32_t member_gate(CxWorld*,const CxAction*,TerraVmGateRef);
int cx_seed_contains(const CxWorld* w,uint32_t x,uint32_t y){return w->vm_trip_index==1u&&x>=w->seed_x&&x-w->seed_x<w->seed_width&&y>=w->seed_y&&y-w->seed_y<w->seed_height;}
static int32_t next_candidate(void* context,uint32_t net,uint32_t* cursor,TerraVmGateRef* out){
    CxWorld* w=(CxWorld*)context;uint32_t group=net+1u,index=cx_action_index(w,group);if(index==CX_NONE)return 0;CxAction* a=w->actions+index;
    if(*cursor>=a->length)return 0;uint32_t at=a->code+*cursor;out->group=group;out->offset=*cursor;*cursor=record_end(w,at)-a->code;
    if(w->vm_trip_index==1u&&w->seed_lamps&&w->command.kind==TCW_TRIGGER){
        uint8_t h=cx_bytes_get(&w->code,at++);int skipped=0;
        if(h&128u){uint32_t id=cx_var_read(&w->code,&at);if(id>=w->lamp_count)return -1;CxGeneralLamp* l=w->lamps+id;skipped=cx_seed_contains(w,l->x,l->y);}
        else{uint32_t gate=member_gate(w,a,*out);for(uint32_t i=0;i<w->point_count;i++){CxInputPoint* p=w->points+i;if(p->value)continue;for(uint32_t j=0;j<w->binding_count;j++){CxBinding* b=w->bindings+j;if(b->x==p->x&&b->y==p->y&&b->tile.type==419u&&b->tile.frame_x==36&&b->gate==gate){skipped=1;break;}}if(skipped)break;}}
        if(skipped){out->group=CX_NONE;out->offset=0;}
    }return 1;
}
static uint32_t override_at(CxWorld* w,uint32_t gate,uint32_t x,uint32_t y){
    if(gate)x=y=0;uint32_t lo=0,hi=w->override_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;CxOverride* o=w->overrides+m;if(o->gate<gate||(o->gate==gate&&(o->x<x||(o->x==x&&o->y<y))))lo=m+1;else hi=m;}return lo;
}
int cx_override_value(CxWorld* w,uint32_t gate,uint32_t x,uint32_t y){if(gate)x=y=0;uint32_t i=override_at(w,gate,x,y);CxOverride* o=i<w->override_count?w->overrides+i:NULL;return o&&o->gate==gate&&o->x==x&&o->y==y&&o->value;}
int cx_override_reserve(CxWorld* w,uint32_t count){
    if(count<=w->override_capacity)return TCW_OK;if((uint64_t)count*sizeof(CxOverride)>UINT32_MAX)return TCW_MEMORY;CxOverride* p=(CxOverride*)cx_alloc(w,count*sizeof(CxOverride));if(!p)return TCW_MEMORY;if(w->override_count)memcpy(p,w->overrides,w->override_count*sizeof(CxOverride));cx_free(w,w->overrides);w->overrides=p;w->override_capacity=count;return TCW_OK;
}
int cx_set_override(CxWorld* w,uint32_t gate,uint32_t x,uint32_t y,uint32_t value){
    if(gate)x=y=0;uint32_t at=override_at(w,gate,x,y);if(at<w->override_count){CxOverride* o=w->overrides+at;if(o->gate==gate&&o->x==x&&o->y==y){o->value=value?1u:0u;return TCW_OK;}}
    if(!value)return TCW_OK;if(w->override_count==UINT32_MAX)return TCW_MEMORY;
    int s=cx_override_reserve(w,w->override_count+1u);if(s)return s;memmove(w->overrides+at+1u,w->overrides+at,(w->override_count-at)*sizeof(CxOverride));w->overrides[at]=(CxOverride){gate,x,y,1u};++w->override_count;return TCW_OK;
}
int cx_general_lamp_value(CxWorld* w,const CxGeneralLamp* l){
    int value=l->initial^cx_override_value(w,0,l->x,l->y);for(uint32_t c=0;c<4;c++)if(l->nets[c])value^=terra_vm_parity(w->vm,l->nets[c]-1u);return value;
}
static uint32_t member_gate(CxWorld* w,const CxAction* a,TerraVmGateRef ref){
    uint32_t at=a->code,member=a->members,gate=0;
    if(w->eval_valid&&w->eval_group==ref.group&&w->eval_offset<=ref.offset){at=a->code+w->eval_offset;member=w->eval_member;gate=w->eval_gate;}
    uint32_t target=a->code+ref.offset;
    while(at<target){gate+=cx_var_read(&w->members,&member);at=record_end(w,at);}
    gate+=cx_var_read(&w->members,&member);
    w->eval_group=ref.group;w->eval_offset=record_end(w,target)-a->code;w->eval_member=member;w->eval_gate=gate;w->eval_valid=1;return gate;
}
static int32_t evaluate(void* context,TerraVmGateRef event,const TerraCircuitVm* vm,TerraVmGateResult* out){
    CxWorld* w=(CxWorld*)context;if(event.group==CX_NONE)return 0;uint32_t index=cx_action_index(w,event.group);if(index==CX_NONE)return -1;CxAction* a=w->actions+index;
    if(event.offset>=a->length)return -1;uint32_t at=a->code+event.offset;uint8_t h=cx_bytes_get(&w->code,at++);uint32_t gate=member_gate(w,a,event);
    out->identity=(TerraVmGateRef){0,gate};out->count=0;
    if(h&128u){
        uint32_t id=cx_var_read(&w->code,&at);if(id>=w->lamp_count)return -1;uint32_t lo=0,hi=w->general_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(w->general[m].id<w->lamps[id].gate)lo=m+1;else hi=m;}if(lo>=w->general_count||w->general[lo].id!=w->lamps[id].gate)return -1;CxGeneralGate* g=w->general+lo;uint32_t on=0;
        for(uint32_t i=0;i<g->count;i++)on+=(uint32_t)cx_general_lamp_value(w,w->lamps+g->first+i);
        int next=0;switch(g->style){case 0:next=on==g->count;break;case 1:next=on>0;break;case 2:next=on!=g->count;break;case 3:next=on==0;break;case 4:next=on==1;break;case 5:next=on!=1;break;default:return 0;}
        int old_on=g->frame==1,old_faulty=g->frame==2,remove_faulty=!g->faulty&&old_faulty,faulty_hit=g->faulty&&(h&1u);
        if(next==old_on&&!remove_faulty&&!faulty_hit)return 0;
        g->frame=g->faulty?2u:(uint8_t)next;int fire=!g->faulty||faulty_hit;
        if(faulty_hit)fire=random_next(w,g->count)<on;if(remove_faulty)fire=0;
        if(!fire)return 0;for(uint32_t c=0;c<4;c++)if(g->nets[c])out->nets[out->count++]=g->nets[c]-1u;return 1;
    }
    uint32_t nc=h&7u,no=(h>>3)&7u;int on=((h>>6)&1u)^cx_override_value(w,gate,0,0);
    for(uint32_t i=0;i<nc;i++){uint32_t net=cx_var_read(&w->code,&at);if(!net||net>w->networks)return -1;on^=terra_vm_parity(vm,net-1u);}
    /* Terraria still consumes UnifiedRandom.Next(1) for a deterministic single
     * ordinary lamp under a faulty lamp. This preserves later random gates. */
    random_next(w,1u);
    for(uint32_t i=0;i<no;i++){uint32_t net=cx_var_read(&w->code,&at);if(!net||net>w->networks)return -1;out->nets[out->count++]=net-1u;}
    return on;
}
static void snapshot(CxWorld* w){
    memcpy(w->rng_saved,w->rng,sizeof(w->rng));w->rng_saved_i=w->rng_i;w->rng_saved_j=w->rng_j;w->ticks_saved=w->ticks;
    for(uint32_t i=0;i<w->general_count;i++)w->general_snapshot[i]=w->general[i].frame;
    for(uint32_t i=0;i<w->pixel_count;i++)w->pixel_snapshot[i]=w->pixels[i].state;
    for(uint32_t i=0;i<w->device_count;i++){w->devices[i].initial=w->devices[i].tile;w->devices[i].saved_cooldown=w->devices[i].cooldown;}
    memcpy(w->mechs_snapshot,w->mechs,w->mech_count*4u);w->mech_saved_count=w->mech_count;
    if(w->override_count)memcpy(w->override_snapshot,w->overrides,w->override_count*sizeof(CxOverride));w->override_saved_count=w->override_count;
}
static void transaction_rollback(void* context){
    CxWorld* w=(CxWorld*)context;memcpy(w->rng,w->rng_saved,sizeof(w->rng));w->rng_i=w->rng_saved_i;w->rng_j=w->rng_saved_j;w->ticks=w->ticks_saved;
    for(uint32_t i=0;i<w->general_count;i++)w->general[i].frame=w->general_snapshot[i];
    for(uint32_t i=0;i<w->pixel_count;i++){w->pixels[i].state=w->pixel_snapshot[i];w->pixels[i].hit_h=w->pixels[i].hit_v=w->pixels[i].marked=0;}
    for(uint32_t i=0;i<w->device_count;i++){w->devices[i].tile=w->devices[i].initial;w->devices[i].cooldown=w->devices[i].saved_cooldown;}
    memcpy(w->mechs,w->mechs_snapshot,w->mech_saved_count*4u);w->mech_count=w->mech_saved_count;w->pixel_touched_count=0;w->eval_valid=0;
    if(w->override_saved_count)memcpy(w->overrides,w->override_snapshot,w->override_saved_count*sizeof(CxOverride));w->override_count=w->override_saved_count;
}
static int32_t transaction_begin(void* context){
    CxWorld* w=(CxWorld*)context;w->eval_valid=0;w->vm_trip_index=0;
    if(!w->operation_active)snapshot(w);
    if(w->interaction_pending){w->interaction_pending=0;if(cx_interact(w)<0)return -1;}
    if(w->seed_lamps&&w->command.kind==TCW_TRIGGER)for(uint32_t i=0;i<w->point_count;i++){CxInputPoint* p=w->points+i;if(p->value||!cx_inside_wiring(w,p->x,p->y))continue;for(uint32_t j=0;j<w->binding_count;j++){CxBinding* b=w->bindings+j;if(b->x!=p->x||b->y!=p->y||!cx_lazy_toggle(&b->tile))continue;uint32_t wires=cx_wire_mask(&b->tile)&w->command.mask;if(__builtin_popcount(wires)&1u){uint32_t value=(uint32_t)cx_override_value(w,b->gate,b->x,b->y)^1u;if(cx_set_override(w,b->gate,b->x,b->y,value)<0)return -1;}break;}}
    return 0;
}
int cx_operation_begin(CxWorld* w){
    if(w->operation_active)return TCW_STATE;
    if(w->override_capacity>w->override_snapshot_capacity){CxOverride* p=(CxOverride*)cx_alloc(w,w->override_capacity*sizeof(CxOverride));if(!p)return TCW_MEMORY;cx_free(w,w->override_snapshot);w->override_snapshot=p;w->override_snapshot_capacity=w->override_capacity;}
    uint32_t available=cx_vm_available(w);if(!available)return TCW_MEMORY;
    int s=terra_vm_set_budget(w->vm,available);if(s<0)return TCW_MEMORY;
    s=terra_vm_batch_begin(w->vm);if(s<0)return s==TERRA_VM_OOM||s==TERRA_VM_LIMIT?TCW_MEMORY:TCW_STATE;
    snapshot(w);w->operation_active=1;return TCW_OK;
}
void cx_operation_commit(CxWorld* w){if(w->operation_active){terra_vm_batch_commit(w->vm);w->operation_active=0;}w->interaction_pending=0;}
void cx_operation_rollback(CxWorld* w){
    if(w->operation_active){terra_vm_batch_cancel(w->vm);transaction_rollback(w);w->operation_active=0;}
    else terra_vm_cancel(w->vm);
    w->interaction_pending=0;w->eval_valid=0;
}
int cx_create_vm(CxWorld* w){
    w->general_snapshot=(uint8_t*)cx_alloc(w,w->general_count?w->general_count:1u);if(!w->general_snapshot)return TCW_MEMORY;
    random_seed(w,(int32_t)w->world->worldId);TerraVmCallbacks cb;memset(&cb,0,sizeof(cb));cb.next_candidate=next_candidate;cb.evaluate=evaluate;cb.transaction_begin=transaction_begin;cb.transaction_rollback=transaction_rollback;
    cb.trip_begin=cx_trip_begin;cb.net_hit=cx_net_hit;cb.trip_end=cx_trip_end;cb.wave_begin=cx_wave_begin;cb.wave_end=cx_wave_end;
    uint32_t maximum=cx_vm_available(w);if(!maximum)return TCW_MEMORY;
    int s=terra_vm_create(w->networks,maximum,&cb,w,&w->vm);return s<0?cx_fail(w,TCW_MEMORY,"native wiring VM does not fit the remaining memory budget"):TCW_OK;
}
uint32_t cx_true_lamp(CxWorld* w,const CxBinding* b){
    int on=b->tile.frame_x==18;
    if(b->tile.type==33u)on=b->tile.frame_x==0;
    on^=cx_override_value(w,b->gate,b->x,b->y);
    for(uint32_t c=0;c<4;c++)if(b->nets[c])on^=terra_vm_parity(w->vm,b->nets[c]-1u);return on!=0;
}
int cx_lazy_toggle(const TxTile* t){if(!t->active)return 0;if(t->type>=255u&&t->type<=268u)return !t->actuator;switch(t->type){case 419:return t->frame_x!=36;case 4:case 33:case 49:case 174:case 372:case 646:return 1;case 421:case 422:return !t->actuator;default:return 0;}}
void cx_current_tile(CxWorld* w,uint32_t x,uint32_t y,uint32_t gate,TxTile* t,const uint32_t* nets){
    int flip=0;for(uint32_t c=0;c<4;c++)if(nets[c])flip^=terra_vm_parity(w->vm,nets[c]-1u);
    if(cx_lazy_toggle(t))flip^=cx_override_value(w,gate,x,y);
    if(t->active&&t->type==419u&&t->frame_x!=36){if(flip)t->frame_x=t->frame_x==18?0:18;}
    else if(t->active&&t->type==420u){uint32_t lo=0,hi=w->general_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(w->general[m].id<gate)lo=m+1;else hi=m;}if(lo<w->general_count&&w->general[lo].id==gate)t->frame_x=(int16_t)(18*w->general[lo].frame);}
    else if(t->active&&(t->type==445u||t->type==0xfffeu)){uint32_t i=cx_pixel_find(w,x,y);if(i!=CX_NONE){CxPixel* p=w->pixels+i;t->frame_x=(int16_t)(18u*(p->custom?p->state&3u:p->state));if(p->custom)t->frame_y=(int16_t)(18u*((p->state>>2)&3u));}}
    else if(t->active&&(t->type==132u||t->type==136u||t->type==144u||t->type==411u)){CxDevice* d=cx_device_find(w,x,y);if(d){t->frame_x=d->tile.frame_x;t->frame_y=d->tile.frame_y;}}
    else if(flip&&t->active){if(t->type>=255u&&t->type<=268u&&!t->actuator)t->type=(uint16_t)(t->type>=262u?t->type-7u:t->type+7u);else switch(t->type){case 33:case 49:case 174:case 372:case 646:t->frame_x=(int16_t)(t->frame_x==0?18:0);break;case 4:t->frame_x=(int16_t)(t->frame_x>=66?t->frame_x-66:t->frame_x+66);break;case 421:if(!t->actuator)t->type=422;break;case 422:if(!t->actuator)t->type=421;break;default:break;}}
    if(t->actuator||t->type==130u||t->type==131u){CxDevice* d=cx_device_find(w,x,y);if(d){t->inactive=d->tile.inactive;if(t->type==130u||t->type==131u)t->type=d->tile.type;}}
}
