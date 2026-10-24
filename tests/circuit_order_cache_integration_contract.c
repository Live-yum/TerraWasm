/* Internal-owner integration, not public handle/import coverage. Production
 * parser, rules, VM and transactions run unchanged. Scoped create interception
 * adds transparent callback counters; no GNU linker wrapping is required.
 * Link without separate order/rules objects (their production sources below).
 * COPY's production quantum is 256: this does NOT claim yielded partial COPY. */
#define main geometry_fixture_unused_main
#include "circuit_geometry_contract.c"
#undef main
#include "../src/terra_circuit_order_cache.h"
static uint32_t completed_cache_copies;
static int observed_cache_copy(CxOrderCache* cache,TerraVmGateRef* events,uint32_t work){
    int status=cx_order_cache_copy_step(cache,events,work);
    if(status==CX_ORDER_CACHE_HIT)++completed_cache_copies;
    return status;
}
#define cx_order_cache_copy_step observed_cache_copy
#include "../src/terra_circuit_order.c"
#undef cx_order_cache_copy_step

typedef struct Integration {
    Fixture f;
    CxGeneralLamp lamps[2]; CxGeneralGate gates[2];
    CxAction action; uint32_t action_bits,action_rank[2];
    TxTile empty_column[10];
    TerraVmCallbacks original;
    uint32_t begins,commits,rollbacks,trips,evaluations,hits,outputs,reads;
    TerraVmGateRef ordered[2];
} Integration;
static Integration* observing;
static int32_t integration_create(uint32_t,uint32_t,const TerraVmCallbacks*,void*,TerraCircuitVm**);
#define terra_vm_create integration_create
#include "../src/terra_circuit_rules.c"
#undef terra_vm_create

static int32_t observed_begin(void* c){++observing->begins;return observing->original.transaction_begin(c);}
static void observed_commit(void* c){++observing->commits;if(observing->original.transaction_commit)observing->original.transaction_commit(c);}
static void observed_rollback(void* c){++observing->rollbacks;observing->original.transaction_rollback(c);}
static int32_t observed_trip(void* c){++observing->trips;return observing->original.trip_end(c);}
static int32_t observed_evaluate(void* c,TerraVmGateRef e,const TerraCircuitVm* vm,TerraVmGateResult* out){
    ++observing->evaluations;return observing->original.evaluate(c,e,vm,out);
}
static int32_t observed_order(void* c,TerraVmGateRef* events,uint32_t count,uint32_t begin){
    CxWorld* w=c;
    /* A valid old cursor is deliberately present at lookup entry. A cache hit
     * must invalidate it rather than inherit preparation's acceleration state. */
    if(begin&&count==2){w->eval_valid=1;w->eval_group=1;w->eval_offset=4;w->eval_member=2;w->eval_gate=2;}
    uint32_t copies=completed_cache_copies;
    int32_t status=observing->original.order_candidates(c,events,count,begin);
    if(!status&&count==2){
        if(completed_cache_copies!=copies){assert(completed_cache_copies==copies+1u);assert(!w->eval_valid);++observing->hits;}
        memcpy(observing->ordered,events,sizeof(observing->ordered));++observing->outputs;
    }
    return status;
}
static int32_t integration_create(uint32_t nets,uint32_t maximum,const TerraVmCallbacks* original,void* c,TerraCircuitVm** out){
    observing->original=*original;TerraVmCallbacks cb=*original;
    cb.transaction_begin=observed_begin;cb.transaction_commit=observed_commit;
    cb.transaction_rollback=observed_rollback;cb.trip_end=observed_trip;
    cb.evaluate=observed_evaluate;cb.order_candidates=observed_order;
    return terra_vm_create(nets,maximum,&cb,c,out);
}
static void integration_init(Integration* t,uint32_t maximum){
    memset(t,0,sizeof(*t));observing=t;init(&t->f,10,10,0);
    Fixture* f=&t->f;CxWorld* w=&f->owner;f->size=16;
    /* Two real general-lamp records on one red horizontal wire. Input lists
     * the far lamp first; source FIFO must reverse that order. */
    for(uint32_t x=0;x<10;x++){f->columns[x]=f->size;for(uint32_t y=0;y<10;y++){
        uint32_t lamp=y==3&&(x==4||x==6),wire=y==3&&x>=2&&x<=6;
        record(f,lamp,419,0,wire,1);
    }}
    f->columns[10]=w->tile_end=f->size;w->maximum=maximum;w->networks=1;
    w->column=t->empty_column;w->wire_cells=5;w->x=0;
    assert(cx_order_capture_column(w)==0);
    t->action_bits=2;t->action=(CxAction){0,4,0};
    w->action_bits=(uint8_t*)&t->action_bits;w->action_rank=t->action_rank;w->actions=&t->action;
    const uint8_t code[]={128,0,128,1},members[]={1,1};
    assert(cx_bytes_append(w,&w->code,code,sizeof(code)));assert(cx_bytes_append(w,&w->members,members,sizeof(members)));
    for(uint32_t i=0;i<2;i++){
        t->lamps[i].x=i?4:6;t->lamps[i].y=3;t->lamps[i].gate=i+1;t->lamps[i].nets[0]=1;
        t->gates[i].id=i+1;t->gates[i].first=i;t->gates[i].count=1;t->gates[i].style=1;
    }
    w->lamps=t->lamps;w->lamp_count=2;w->general=t->gates;w->general_count=2;
    w->seed_x=2;w->seed_y=3;w->seed_width=w->seed_height=1;
    w->command.kind=TCW_TRIGGER;w->command.mask=1;w->phase=CX_RUN;
    /* Zero-count snapshot memcpy operands still point to valid storage. */
    w->mechs=cx_alloc(w,4);w->mechs_snapshot=cx_alloc(w,4);assert(w->mechs&&w->mechs_snapshot);
    assert(cx_create_vm(w)==0);
}
static void supply_event(Integration* t){
    CxWorld* w=&t->f.owner;TerraCircuitWorldEvent e=w->event;
    assert(e.kind==TCW_READ&&e.source_id==7);assert((uint64_t)e.offset+e.length<=t->f.size);
    /* VM's external-yield protocol must preserve this exact request. */
    for(uint32_t n=0;n<3;n++){assert(terra_vm_step(w->vm,1)==TERRA_VM_MORE);assert(!memcmp(&e,&w->event,sizeof(e)));}
    assert(cx_order_supply_geometry(w,t->f.bytes+e.offset,e.length)==0);
    assert(cx_order_supply_geometry(w,t->f.bytes+e.offset,e.length)==TCW_INVALID);
    memset(&w->event,0,sizeof(w->event));++t->reads;
}
static void start_pulse(Integration* t){observing=t;uint32_t net=0;assert(terra_vm_begin(t->f.owner.vm,&net,1)==TERRA_VM_MORE);}
static void finish_pulse(Integration* t){
    CxWorld* w=&t->f.owner;
    for(uint32_t i=0;i<100000;i++){int s=terra_vm_step(w->vm,1);assert(s>=0);if(w->event.kind)supply_event(t);if(s==TERRA_VM_OK)return;}
    assert(!"integration pulse failed to finish");
}
static void integration_destroy(Integration* t){
    CxWorld* w=&t->f.owner;terra_vm_destroy(w->vm);w->vm=NULL;
    cx_order_close(w);cx_geometry_destroy(w);cx_bytes_free(w,&w->code);cx_bytes_free(w,&w->members);
    cx_free(w,w->general_snapshot);cx_free(w,w->mechs);cx_free(w,w->mechs_snapshot);finish(&t->f);
}
static void dynamic_miss_hit(uint32_t maximum,int expect_cache){
    Integration t;integration_init(&t,maximum);CxWorld* w=&t.f.owner;
    assert((order_cache(w)!=NULL)==expect_cache);
    start_pulse(&t);finish_pulse(&t);
    assert(t.outputs==1&&!t.hits&&t.reads>0&&t.evaluations==2);
    assert(t.ordered[0].offset==2&&t.ordered[1].offset==0);
    assert(t.gates[0].frame==1&&t.gates[1].frame==1&&terra_vm_parity(w->vm,0)==1);
    uint32_t reads=t.reads;start_pulse(&t);finish_pulse(&t);
    assert(t.outputs==2&&t.hits==(uint32_t)expect_cache&&t.evaluations==4);
    assert(t.ordered[0].offset==2&&t.ordered[1].offset==0);
    assert(!t.gates[0].frame&&!t.gates[1].frame&&!terra_vm_parity(w->vm,0));
    assert(t.begins==2&&t.commits==2&&!t.rollbacks&&t.trips==6);
    if(expect_cache)assert(t.reads==reads&&!w->event.kind&&!w->order->geometry_loading);
    /* Cancel after real ordering succeeds but before real evaluate runs. This
     * exercises actual VM boundaries, not a fictitious yielded COPY phase. */
    uint32_t outputs=t.outputs;start_pulse(&t);
    for(uint32_t i=0;t.outputs==outputs;i++){assert(i<100000);assert(terra_vm_step(w->vm,1)==TERRA_VM_MORE);if(w->event.kind)supply_event(&t);}
    cx_order_cancel(w);cx_geometry_cancel(w);assert(terra_vm_cancel(w->vm)==0);
    assert(t.rollbacks==1&&t.commits==2&&!terra_vm_parity(w->vm,0));
    assert(!t.gates[0].frame&&!t.gates[1].frame&&!w->eval_valid);
    start_pulse(&t);finish_pulse(&t);assert(t.gates[0].frame==1&&t.gates[1].frame==1);
    integration_destroy(&t);
}
static void pending_cancel(void){
    Integration t;integration_init(&t,160u*1024u*1024u);CxWorld* w=&t.f.owner;start_pulse(&t);
    for(uint32_t i=0;!w->event.kind;i++){assert(i<100000);assert(terra_vm_step(w->vm,1)==TERRA_VM_MORE);}
    TerraCircuitWorldEvent stale=w->event;assert(stale.kind==TCW_READ&&!t.trips);
    cx_order_cancel(w);cx_geometry_cancel(w);assert(terra_vm_cancel(w->vm)==0);memset(&w->event,0,sizeof(w->event));
    assert(cx_order_supply_geometry(w,t.f.bytes+stale.offset,stale.length)==TCW_INVALID);
    assert(t.rollbacks==1&&!t.commits&&!terra_vm_parity(w->vm,0));
    start_pulse(&t);finish_pulse(&t);assert(t.outputs==1&&t.trips==3);
    integration_destroy(&t);
}
int main(void){
    uint32_t baseline=tx_native_heap_used();
    dynamic_miss_hit(160u*1024u*1024u,CX_ORDER_CACHE_ENABLED!=0);
    dynamic_miss_hit(1024u*1024u,0);pending_cancel();
    assert(tx_native_heap_used()==baseline);
    puts("real general parser/evaluate/VM order-cache integration passed (internal owner)");return 0;
}
