/* Differential execution against the frozen f9 FIFO implementation.
 * Both owners use independent order state; only generated immutable bytes match. */
#define main geometry_slice_contract_main
#include "circuit_geometry_contract.c"
#undef main
/* Adapt candidate decoding only inside this included ordering test TU. The
 * independently linked production decoder remains available below for the
 * gate-ordinal/read-resume contract. No linker-specific wrapping is required. */
static int test_order_candidate_step(CxWorld*,TerraVmGateRef,CxOrderCandidate*);
#define cx_order_candidate_step test_order_candidate_step
#include "../src/terra_circuit_order.c"
#undef cx_order_candidate_step
int reference_cx_order_capture_column(CxWorld*);
void reference_cx_order_close(CxWorld*);
int32_t reference_cx_order_candidates(void*,TerraVmGateRef*,uint32_t,uint32_t);
uint32_t reference_cx_order_rank(CxWorld*,uint32_t,uint32_t,uint32_t);
/* Candidate decoding is outside this slice; both paths get identical physical
 * lamp coordinates/group identities, while real rank/FIFO code runs unchanged. */
uint32_t reference_cx_order_event_rank(CxWorld* w,TerraVmGateRef e){return reference_cx_order_rank(w,e.offset/w->height,e.offset%w->height,e.group);}
static int duplicate_groups,batch_standard_adapter;
static int test_order_candidate_step(CxWorld* w,TerraVmGateRef e,CxOrderCandidate* out){memset(out,0,sizeof(*out));out->parsed=out->valid=1;out->direct=!batch_standard_adapter;out->x=e.offset/w->height;out->y=e.offset%w->height;for(uint32_t c=0;c<4;c++)out->nets[c]=duplicate_groups&&c==1u?1u:c+1u;return 0;}
static int32_t no_candidate(void* c,uint32_t n,uint32_t* p,TerraVmGateRef* e){(void)c;(void)n;(void)p;(void)e;return 0;}
static int32_t no_evaluate(void* c,TerraVmGateRef e,const TerraCircuitVm* v,TerraVmGateResult* o){(void)c;(void)e;(void)v;(void)o;return 0;}
static void make_grid(Fixture* f,TxTile* grid,uint32_t seed){
    f->size=16;f->important[420u>>3]|=1u<<(420u&7u);
    for(uint32_t x=0;x<f->width;x++){f->columns[x]=f->size;
        for(uint32_t y=0;y<f->height;y++){
            TxTile* t=grid+x*f->height+y;memset(t,0,sizeof(*t));
            seed=seed*1664525u+1013904223u;
            uint32_t mask=x>=2&&x+2<f->width&&y>=2&&y+2<f->height?((seed>>24)|1u)&15u:0;
            t->wire_red=mask&1u;t->wire_blue=(mask>>1)&1u;t->wire_green=(mask>>2)&1u;t->wire_yellow=(mask>>3)&1u;
            if(mask&&y%3u==0){t->active=1;t->type=419;t->frame_x=18;}
            /* Isolated directional junctions surrounded by ordinary wire. */
            else if(mask&&x%4u==0&&y%4u==0){t->active=1;t->type=424;t->frame_x=(int16_t)((seed%3u)*18u);}
            if(x==2&&y==2){t->active=1;t->type=420;}
            record(f,t->active,t->type,t->frame_x,mask,1);
        }
    }
    f->columns[f->width]=f->size;f->owner.tile_end=f->size;
}
static void prepare_owner(CxWorld* w,Fixture* f,TxTile* grid,int old,int buffered){
    *w=f->owner;w->geometry=NULL;w->order=NULL;w->phase=CX_COUNT;w->bytes=w->peak=0;w->wire_cells=0;w->gates=1;
    for(uint32_t i=0;i<f->width*f->height;i++)w->wire_cells+=cx_wire_mask(grid+i)!=0;
    w->column_gates=calloc(f->width+1u,4);assert(w->column_gates);for(uint32_t x=3;x<=f->width;x++)w->column_gates[x]=1;
    w->gate_at_y=calloc(f->height,4);w->ids=calloc(f->height*4u,4);w->checkpoint_count=(f->width+63u)/64u;w->checkpoint_index=calloc(w->checkpoint_count,sizeof(CxCheckpoint));assert(w->gate_at_y&&w->ids&&w->checkpoint_index);
    for(uint32_t i=0;i<w->checkpoint_count;i++){w->checkpoint_index[i].column=i*64u;w->checkpoint_index[i].source_offset=f->columns[i*64u];}
    if(buffered)w->world_source=0;
    for(uint32_t x=0;x<f->width;x++){w->x=x;w->column=grid+x*f->height;memset(w->gate_at_y,0,f->height*4u);if(x==2)w->gate_at_y[2]=1;
        for(uint32_t y=0;y<f->height;y++)for(uint32_t c=0;c<4;c++)w->ids[y*4u+c]=cx_wire_mask(w->column+y)&(1u<<c)?(duplicate_groups&&c==1u?1u:c+1u):0;
        assert((old?reference_cx_order_capture_column(w):cx_order_capture_column(w))==0);
    }
    TerraVmCallbacks cb={0};cb.next_candidate=no_candidate;cb.evaluate=no_evaluate;
    assert(terra_vm_create(4,cx_vm_available(w),&cb,NULL,&w->vm)==0);w->phase=CX_RUN;
    w->seed_x=2;w->seed_y=2;w->seed_width=2;w->seed_height=2;w->command.kind=TCW_TRIGGER;w->command.mask=15;
}
static uint32_t drive(CxWorld* w,Fixture* f,TerraVmGateRef* events,uint32_t count,int old,int restart){
    uint32_t reads=0;int begin=1;
    for(uint32_t i=0;i<2000000u;i++){
        int s=old?reference_cx_order_candidates(w,events,count,begin):cx_order_candidates(w,events,count,begin);begin=0;assert(s>=0);
        if(!old&&restart&&i==37){cx_geometry_cancel(w);memset(&w->event,0,sizeof(w->event));begin=1;restart=0;continue;}
        if(!s)return reads;
        if(s==TERRA_VM_ORDER_EXTERNAL_YIELD){assert(!old);TerraCircuitWorldEvent request=w->event;assert(request.kind==TCW_READ&&request.source_id==7);
            /* Repeating a pending callback must not advance its FIFO substep. */
            for(uint32_t n=0;n<3;n++){assert(cx_order_candidates(w,events,count,0)==TERRA_VM_ORDER_EXTERNAL_YIELD);assert(!memcmp(&request,&w->event,sizeof(request)));}
            assert(cx_order_supply_geometry(w,f->bytes+request.offset,request.length)==0);memset(&w->event,0,sizeof(w->event));++reads;
        }
    }
    assert(!"bounded differential driver exhausted");return 0;
}
static void dispose_owner(CxWorld* w,int old){terra_vm_destroy(w->vm);w->vm=NULL;if(old)reference_cx_order_close(w);else cx_order_close(w);cx_geometry_destroy(w);assert(w->bytes==0);free(w->column_gates);free(w->ids);free(w->gate_at_y);free(w->checkpoint_index);}
static void compare(uint32_t seed,int buffered,int gate_origin,int restart){
    Fixture f;init(&f,13,14,0);TxTile* grid=calloc(f.width*f.height,sizeof(TxTile));assert(grid);make_grid(&f,grid,seed);
    if(buffered){f.world.file=f.bytes;f.world.file_len=f.size;}
    CxWorld a,b;prepare_owner(&a,&f,grid,1,buffered);prepare_owner(&b,&f,grid,0,buffered);
    if(gate_origin)a.vm_source_gate=b.vm_source_gate=1;
    if(duplicate_groups)a.command.mask=b.command.mask=2;
    TerraVmGateRef original[1024],actual[1024];uint32_t count=0;
    for(uint32_t x=0;x<f.width;x++)for(uint32_t y=0;y<f.height;y++)if(grid[x*f.height+y].active&&grid[x*f.height+y].type==419u)
        for(uint32_t c=0;c<4;c++)if(cx_wire_mask(grid+x*f.height+y)&(1u<<c))original[count++]=(TerraVmGateRef){c+1u,x*f.height+y};
    assert(count>2&&count<1024);for(uint32_t i=0;i<count/2;i++){TerraVmGateRef e=original[i];original[i]=original[count-1u-i];original[count-1u-i]=e;}memcpy(actual,original,count*sizeof(*actual));
    TxWorld snapshot=f.world;uint32_t source_hash=fingerprint(&f);
    drive(&a,&f,original,count,1,0);uint32_t reads=drive(&b,&f,actual,count,0,restart);
    assert(!memcmp(original,actual,count*sizeof(*actual)));assert(!memcmp(&snapshot,&f.world,sizeof(snapshot)));assert(source_hash==fingerprint(&f));assert(buffered?!reads:reads>0);assert(!b.order->groups&&!b.order->group_capacity);
    dispose_owner(&a,1);dispose_owner(&b,0);free(grid);finish(&f);
}
static void cache_cost(void){
    Fixture f;init(&f,70,32,0);f.world.file=f.bytes;f.world.file_len=f.size;f.owner.world_source=0;
    CxCheckpoint cp[2]={{0}};cp[0].column=0;cp[0].source_offset=f.columns[0];cp[1].column=64;cp[1].source_offset=f.columns[64];f.owner.checkpoint_index=cp;
    assert(cx_geometry_create(&f.owner)==0);CxGeometryStats s;
    /* At most 64 columns are replayed on a buffered miss; never per neighbour. */
    for(uint32_t x=61;x<=63;x++){uint32_t before;assert(cx_geometry_stats(&f.owner,&s)==0);before=(uint32_t)s.decoded_records;
        int rc=cx_geometry_begin(&f.owner,x);while(rc!=CX_GEOMETRY_READY){CxGeometryRead req;rc=cx_geometry_step(&f.owner,7,&req);assert(rc==CX_GEOMETRY_MORE||rc==CX_GEOMETRY_READY);}
        assert(cx_geometry_stats(&f.owner,&s)==0);assert(s.decoded_records-before<=(uint64_t)64*f.height);
    }
    uint64_t decoded=s.decoded_records;
    for(uint32_t i=0;i<1000;i++){uint8_t b;assert(cx_geometry_get(&f.owner,61+i%3u,i%f.height,&b)==0);}
    assert(cx_geometry_stats(&f.owner,&s)==0&&s.decoded_records==decoded&&s.evictions==0);
    int rc=cx_geometry_begin(&f.owner,69);while(rc!=CX_GEOMETRY_READY){CxGeometryRead req;rc=cx_geometry_step(&f.owner,7,&req);assert(rc>=0&&rc!=CX_GEOMETRY_READ);}
    assert(cx_geometry_stats(&f.owner,&s)==0&&s.evictions==1&&s.decoded_records-decoded<=6u*f.height);
    decoded=s.decoded_records;
    /* Whole-column L2 was deliberately removed for FIFO-only point blocks.
     * Preserve the original exact64H sequential full-column decode bound. */
    uint64_t requests=s.read_requests;
    for(uint32_t x=0;x<64;x++){int status=cx_geometry_begin(&f.owner,x);while(status!=CX_GEOMETRY_READY){CxGeometryRead request;status=cx_geometry_step(&f.owner,7,&request);assert(status>=0&&status!=CX_GEOMETRY_READ);}}
    assert(cx_geometry_stats(&f.owner,&s)==0&&s.decoded_records-decoded==64u*f.height&&s.read_requests==requests);finish(&f);
}
static void sparse_growth_failure(void){
    Fixture f;init(&f,8,8,0);TxTile* grid=calloc(f.width*f.height,sizeof(TxTile));assert(grid);make_grid(&f,grid,91);
    CxWorld w;prepare_owner(&w,&f,grid,0,0);CxOrder* o=w.order;
    for(uint32_t i=0;i<128;i++){OrderVisit* v=visit(&w,i,i+3u,1);assert(v);v->value=(uint8_t)(i%5u);}
    assert(o->visit_capacity==256u&&o->visit_count==128u);OrderVisit* old=o->visits;
    TerraVmStats vm;terra_vm_stats(w.vm,&vm);uint32_t retained=w.bytes;
    /* New table payload alone fits this headroom, but its real overhead does
     * not. Releasing/subtracting the old table before admission would pass. */
    w.maximum=w.bytes+vm.allocated_bytes+512u*sizeof(OrderVisit);
    assert(!visit(&w,999u,1000u,1));assert(o->visits==old&&o->visit_count==128&&o->visit_capacity==256&&w.bytes==retained);
    for(uint32_t i=0;i<128;i++){OrderVisit* v=visit(&w,i,i+3u,0);assert(v&&v->value==i%5u);}
    w.maximum=160u*1024u*1024u;assert(visit(&w,999u,1000u,1));assert(o->visit_capacity==512&&o->visit_count==129);
    for(uint32_t i=0;i<256;i++)assert(enqueue(&w,i,i+1u,i%4u)==0);
    OrderPoint* queue=o->queue;uint32_t count=o->length,head=o->head;retained=w.bytes;
    terra_vm_stats(w.vm,&vm);w.maximum=w.bytes+vm.allocated_bytes;
    assert(enqueue(&w,900,901,2)<0);assert(o->queue==queue&&o->length==count&&o->head==head&&w.bytes==retained);
    for(uint32_t i=0;i<256;i++)assert(queue[i].x==i&&queue[i].y==i+1u&&queue[i].direction==i%4u);
    dispose_owner(&w,0);free(grid);finish(&f);
}
static void gate_ordinals(void){
    Fixture f;init(&f,8,600,0);f.important[420u>>3]|=1u<<(420u&7u);f.size=16;
    for(uint32_t x=0;x<f.width;x++){f.columns[x]=f.size;
        if(x==2){record(&f,1,420,0,0,2);record(&f,0,0,0,0,296);record(&f,1,420,0,0,2);record(&f,0,0,0,0,300);}
        else if(x==6){record(&f,0,0,0,0,599);record(&f,1,420,0,1,1);}
        else record(&f,0,0,0,0,600);
    }
    f.columns[f.width]=f.size;f.owner.tile_end=f.size;
    uint32_t prefix[]={0,0,0,4,4,4,4,5,5};CxWorld* w=&f.owner;w->column_gates=prefix;w->gates=5;
    assert(cx_geometry_create(w)==0);w->order=cx_alloc(w,sizeof(CxOrder));assert(w->order);memset(w->order,0,sizeof(CxOrder));
    const uint32_t ys[]={0,1,298,299,599};
    for(uint32_t gate=1;gate<=5;gate++){uint32_t x=99,y=99,steps=0;
        for(;;){int status=cx_order_gate_position_step(w,gate,&x,&y);++steps;
            if(!status)break;assert(status==1||status==TERRA_VM_ORDER_EXTERNAL_YIELD);
            if(status==TERRA_VM_ORDER_EXTERNAL_YIELD){TerraCircuitWorldEvent e=w->event;assert(cx_order_supply_geometry(w,f.bytes+e.offset,e.length)==0);memset(&w->event,0,sizeof(w->event));}
            assert(steps<10000);
        }
        assert(x==(gate==5?6u:2u)&&y==ys[gate-1u]);uint8_t bits;assert(cx_geometry_get(w,x,y,&bits)==0);assert(bits&128u);assert((bits&15u)==(gate==5?1u:0u));
    }
    uint32_t x,y;assert(cx_order_gate_position_step(w,0,&x,&y)==CX_ORDER_GATE_ABSENT);assert(cx_order_gate_position_step(w,6,&x,&y)==CX_ORDER_GATE_ABSENT);
    uint32_t bits=2,ranks[2]={0};CxAction action={0,5,0};w->networks=1;w->action_bits=(uint8_t*)&bits;w->action_rank=ranks;w->actions=&action;
    uint8_t codes[5]={0},members[5]={1,1,1,1,1};assert(cx_bytes_append(w,&w->code,codes,5));assert(cx_bytes_append(w,&w->members,members,5));
    for(uint32_t offset=2;offset<5;offset++){
        cx_geometry_destroy(w);assert(cx_geometry_create(w)==0);w->eval_valid=0;CxOrderCandidate candidate={0};TerraVmGateRef event={1,offset};uint32_t reads=0,more=0;
        for(uint32_t steps=0;;steps++){
            int status=cx_order_candidate_step(w,event,&candidate);assert(candidate.member_records==offset+1u);
            if(!status)break;assert(status==1||status==2);assert(steps<10000);if(status==1)++more;
            if(status==2){TerraCircuitWorldEvent e=w->event;assert(cx_order_supply_geometry(w,f.bytes+e.offset,e.length)==0);memset(&w->event,0,sizeof(w->event));++reads;}
        }
        assert(reads>0&&more>0&&candidate.valid&&candidate.y==ys[offset]-2u);
    }
    cx_bytes_free(w,&w->code);cx_bytes_free(w,&w->members);
    cx_order_close(w);cx_geometry_destroy(w);assert(w->bytes==0);finish(&f);
}
static void raw_compiler_axis(void){
    Fixture f;init(&f,1,2,0);f.size=16;f.columns[0]=16;
    /* Known-version reader accepts raw65534 without a framed bitmap entry. */
    byte(&f,35);byte(&f,2);word(&f,65534u);record(&f,0,0,0,0,1);f.columns[1]=f.owner.tile_end=f.size;
    assert(cx_geometry_create(&f.owner)==0);int status=cx_geometry_begin(&f.owner,0);
    while(status!=CX_GEOMETRY_READY){CxGeometryRead r;status=cx_geometry_step(&f.owner,1,&r);assert(status>=0);if(status==CX_GEOMETRY_READ){assert(cx_geometry_supply(&f.owner,&r,f.bytes+r.offset,r.length)==0);}}
    uint8_t word,route;assert(cx_geometry_get_connectivity(&f.owner,0,0,&word,&route)==0);assert(word==1&&route==1);finish(&f);
}
/* Prototype group construction/visitor checks, no reordered event array. */
static void batch_group_contract(void){
    Fixture f;init(&f,130,14,0);TxTile* grid=calloc(f.width*f.height,sizeof(TxTile));assert(grid);make_grid(&f,grid,17);CxWorld w;prepare_owner(&w,&f,grid,0,0);
    TerraVmGateRef events[]={{1,100u*14u+9u},{1,6u*14u+6u},{1,66u*14u+6u},{1,6u*14u+6u}},saved[4];memcpy(saved,events,sizeof(events));
    batch_standard_adapter=1;int begin=1;CxOrder* o=w.order;
    for(uint32_t steps=0;o->scan<4u||begin;steps++){
        int status=cx_order_candidates(&w,events,4,begin);begin=0;assert(status==1||status==2);assert(steps<100000);
        if(status==2){TerraCircuitWorldEvent e=w.event;assert(cx_order_supply_geometry(&w,f.bytes+e.offset,e.length)==0);memset(&w.event,0,sizeof(w.event));}
    }
    assert(!memcmp(events,saved,sizeof(events)));uint32_t groups=0;
    for(uint32_t i=0;i<o->group_capacity;i++)if(o->groups[i].used){OrderGroup* g=o->groups+i;++groups;if(g->checkpoint==0){assert(g->max_x==6&&g->max_y==6);}else{assert(g->checkpoint==1&&g->max_x==100&&g->max_y==9);}}
    assert(groups==2);for(uint32_t i=0;i<4;i++){assert(o->items[i].event.group==saved[i].group&&o->items[i].event.offset==saved[i].offset);assert(o->items[i].rank==UINT32_MAX-1u&&o->items[i].colour==4);}
    assert(batch_candidate(&w,6,6,0,99,65)==1);assert(batch_candidate(&w,6,6,0,99,65)==0);assert(o->items[1].colour==4&&o->items[3].colour==4);
    assert(batch_candidate(&w,6,6,0,1,65)==1);assert(batch_candidate(&w,6,6,0,1,65)==0);
    assert(batch_candidate(&w,6,6,1,1,65)==1);assert(batch_candidate(&w,6,6,1,1,65)==0);assert(o->items[1].colour==0&&o->items[3].colour==0);
    assert(!memcmp(events,saved,sizeof(events)));cx_order_cancel(&w);assert(!o->batch_active&&!o->visitor_active);
    /* Distinct groups, not candidate-head capacity, control allocation. */
    uint32_t capacity=o->group_capacity;assert(capacity==8&&o->group_count==2);
    assert(candidate_group(&w,2)&&candidate_group(&w,3));OrderGroup* old=o->groups;
    TerraVmStats vm;terra_vm_stats(w.vm,&vm);uint32_t retained=w.bytes;
    w.maximum=w.bytes+vm.allocated_bytes;assert(!candidate_group(&w,4));
    assert(o->groups==old&&o->group_count==4&&o->group_capacity==capacity&&w.bytes==retained);
    w.maximum=160u*1024u*1024u;assert(candidate_group(&w,4));assert(o->group_count==5&&o->group_capacity==16);
    assert(order_begin(&w,events,4)==0&&o->group_count==0);
    batch_standard_adapter=0;dispose_owner(&w,0);free(grid);finish(&f);
}
/* Complete real batch -> FIFO -> stable sort, compared with f9. Synthetic
 * connected red rectangle has exactly one compiler label and final network. */
static void batch_pipeline_contract(int buffered){
    Fixture f;init(&f,12,12,0);TxTile* grid=calloc(f.width*f.height,sizeof(*grid));assert(grid);
    f.size=16;f.important[420u>>3]|=1u<<(420u&7u);
    for(uint32_t x=0;x<f.width;x++){f.columns[x]=f.size;for(uint32_t y=0;y<f.height;y++){
        TxTile* t=grid+x*f.height+y;t->wire_red=x>=2&&x+2<f.width&&y>=2&&y+2<f.height;
        if(t->wire_red&&y==6){t->active=1;t->type=419;t->frame_x=18;}
        if(x==2&&y==2){t->active=1;t->type=420;}
        record(&f,t->active,t->type,t->frame_x,t->wire_red,1);
    }}
    f.columns[f.width]=f.size;f.owner.tile_end=f.size;if(buffered){f.world.file=f.bytes;f.world.file_len=f.size;}
    CxWorld a,b;prepare_owner(&a,&f,grid,1,buffered);prepare_owner(&b,&f,grid,0,buffered);
    uint8_t map[]={2},checkpoint[]={12,0,12,0,12,0,12,0};
    assert(cx_bytes_append(&b,&b.map,map,sizeof(map)));assert(cx_bytes_append(&b,&b.checkpoints,checkpoint,sizeof(checkpoint)));
    b.labels_count=b.networks=1;b.checkpoint_index[0].length=sizeof(checkpoint);
    TerraVmGateRef original[]={{1,9*12+6},{1,5*12+6},{1,7*12+6},{1,5*12+6}},actual[4],before[4];
    memcpy(actual,original,sizeof(actual));memcpy(before,original,sizeof(before));
    TxWorld owner=f.world;uint32_t source=fingerprint(&f);batch_standard_adapter=1;
    drive(&a,&f,original,4,1,0);uint32_t reads=drive(&b,&f,actual,4,0,0);batch_standard_adapter=0;
    assert(!memcmp(original,actual,sizeof(actual)));assert(memcmp(before,actual,sizeof(actual)));
    assert(b.order->group_count==1&&b.order->group_capacity==8&&!b.order->batch_active&&!b.order->visitor_active);
    assert(!memcmp(&owner,&f.world,sizeof(owner))&&source==fingerprint(&f));assert(buffered?!reads:reads>0);
    cx_bytes_free(&b,&b.map);cx_bytes_free(&b,&b.checkpoints);dispose_owner(&a,1);dispose_owner(&b,0);free(grid);finish(&f);
}
int main(void){uint32_t baseline=tx_native_heap_used();for(uint32_t seed=1;seed<=12;seed++){compare(seed,0,seed%2u,seed%3u==0);compare(seed,1,seed%2u,seed%3u==0);}duplicate_groups=1;compare(37,0,0,0);compare(37,1,0,0);duplicate_groups=0;cache_cost();sparse_growth_failure();gate_ordinals();raw_compiler_axis();batch_group_contract();batch_pipeline_contract(0);batch_pipeline_contract(1);assert(tx_native_heap_used()==baseline);puts("bounded geometry ordering equals frozen FIFO oracle across stream/buffered/yield/restart profiles");return 0;}
