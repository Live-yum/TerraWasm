/* H versus frozen f9: real geometry/FIFO/stable sort, synthetic candidate
 * coordinates and network IDs. This is not parser or public-handle coverage.
 * Link tests/order_reference.c exactly as the existing geometry-order target. */
#define main empty_colour_geometry_fixture_main
#include "circuit_geometry_contract.c"
#undef main
static TxTile* empty_grid;
static int shared_red_blue;
static int empty_candidate(CxWorld*,TerraVmGateRef,CxOrderCandidate*);
#define cx_order_candidate_step empty_candidate
#include "../src/terra_circuit_order.c"
#undef cx_order_candidate_step
_Static_assert(offsetof(CxOrder,eligible_mask)==offsetof(CxOrder,current_geometry)+1u,"eligibility uses geometry padding");
_Static_assert(offsetof(CxOrder,eligible_mask)<offsetof(CxOrder,seed_scan),"eligibility precedes aligned seed cursor");
#if defined(__LP64__)
_Static_assert(sizeof(CxOrder)==376u,"H must retain G mandatory order charge on LP64");
#elif defined(__wasm32__)
_Static_assert(sizeof(CxOrder)==320u,"H must retain G mandatory order charge on wasm32");
#endif
int reference_cx_order_capture_column(CxWorld*);
void reference_cx_order_close(CxWorld*);
int32_t reference_cx_order_candidates(void*,TerraVmGateRef*,uint32_t,uint32_t);
uint32_t reference_cx_order_rank(CxWorld*,uint32_t,uint32_t,uint32_t);
uint32_t reference_cx_order_event_rank(CxWorld* w,TerraVmGateRef e){
    return reference_cx_order_rank(w,e.offset/w->height,e.offset%w->height,e.group);
}
static uint32_t network(uint32_t c){return shared_red_blue&&c==1u?1u:c+1u;}
static int empty_candidate(CxWorld* w,TerraVmGateRef e,CxOrderCandidate* out){
    memset(out,0,sizeof(*out));out->parsed=out->valid=out->direct=1;
    out->x=e.offset/w->height;out->y=e.offset%w->height;
    uint32_t bits=cx_wire_mask(empty_grid+e.offset);
    for(uint32_t c=0;c<4;c++)if(bits&(1u<<c))out->nets[c]=network(c);
    return 0;
}
static int32_t empty_next(void* c,uint32_t n,uint32_t* p,TerraVmGateRef* e){(void)c;(void)n;(void)p;(void)e;return 0;}
static int32_t empty_eval(void* c,TerraVmGateRef e,const TerraCircuitVm* v,TerraVmGateResult* r){(void)c;(void)e;(void)v;(void)r;return 0;}
static void empty_world(Fixture* f){
    init(f,32,16,0);empty_grid=calloc(f->width*f->height,sizeof(*empty_grid));assert(empty_grid);
    f->size=16;f->important[420u>>3]|=1u<<(420u&7u);
    for(uint32_t x=0;x<f->width;x++){
        f->columns[x]=f->size;
        for(uint32_t y=0;y<f->height;y++){
            TxTile* t=empty_grid+x*f->height+y;
            t->wire_red=x>=2&&x+2<f->width&&y>=2&&y+2<f->height;
            t->wire_blue=t->wire_red;
            t->wire_yellow=(y==2&&x>=2&&x<=12)||(x==10&&y==8);
            if((y==2&&(x==3||x==4||x==8||x==12))||(x==10&&y==8)){
                t->active=1;t->type=419;t->frame_x=18;
            }
            if(x==2&&y==2){t->active=1;t->type=420;}
            record(f,t->active,t->type,t->frame_x,cx_wire_mask(t),1);
        }
    }
    f->columns[f->width]=f->owner.tile_end=f->size;
}
static void empty_owner(CxWorld* w,Fixture* f,int old,int buffered){
    *w=f->owner;w->geometry=NULL;w->order=NULL;w->bytes=w->peak=0;w->phase=CX_COUNT;w->gates=1;w->wire_cells=0;
    w->column_gates=calloc(f->width+1u,4);w->gate_at_y=calloc(f->height,4);w->ids=calloc(f->height*4u,4);
    w->checkpoint_count=1;w->checkpoint_index=calloc(1,sizeof(CxCheckpoint));
    assert(w->column_gates&&w->gate_at_y&&w->ids&&w->checkpoint_index);
    w->checkpoint_index[0].source_offset=f->columns[0];
    for(uint32_t x=3;x<=f->width;x++)w->column_gates[x]=1;
    if(buffered)w->world_source=0;
    for(uint32_t i=0;i<f->width*f->height;i++)w->wire_cells+=cx_wire_mask(empty_grid+i)!=0;
    for(uint32_t x=0;x<f->width;x++){
        w->x=x;w->column=empty_grid+x*f->height;memset(w->gate_at_y,0,f->height*4u);if(x==2)w->gate_at_y[2]=1;
        for(uint32_t y=0;y<f->height;y++)for(uint32_t c=0;c<4;c++)w->ids[y*4u+c]=(cx_wire_mask(w->column+y)&(1u<<c))?network(c):0;
        assert((old?reference_cx_order_capture_column(w):cx_order_capture_column(w))==0);
    }
    TerraVmCallbacks cb={0};cb.next_candidate=empty_next;cb.evaluate=empty_eval;
    assert(terra_vm_create(4,cx_vm_available(w),&cb,NULL,&w->vm)==0);
    w->phase=CX_RUN;w->seed_x=w->seed_y=2;w->seed_width=w->seed_height=1;
    w->command.kind=TCW_TRIGGER;w->command.mask=15;
}
static void empty_dispose(CxWorld* w,int old){
    terra_vm_destroy(w->vm);w->vm=NULL;
    if(old)reference_cx_order_close(w);else cx_order_close(w);
    cx_geometry_destroy(w);assert(!w->bytes);
    free(w->column_gates);free(w->gate_at_y);free(w->ids);free(w->checkpoint_index);
}
typedef struct EmptyTrace {uint32_t colours,reads,cancelled,tail_after_rank;} EmptyTrace;
static EmptyTrace empty_drive(CxWorld* w,Fixture* f,TerraVmGateRef* e,uint32_t count,int old,int cancel){
    EmptyTrace trace={0};uint32_t begin=1;
    for(uint32_t step=0;step<2000000u;step++){
        int s=old?reference_cx_order_candidates(w,e,count,begin):cx_order_candidates(w,e,count,begin);begin=0;assert(s>=0);
        if(!old){CxOrder* o=w->order;
            if(o->phase==2||o->phase==3){trace.colours|=1u<<o->colour;if(o->sequence&&o->phase==3)++trace.tail_after_rank;}
        }
        if(!s)return trace;
        if(s==TERRA_VM_ORDER_EXTERNAL_YIELD){
            assert(!old);TerraCircuitWorldEvent request=w->event;assert(request.kind==TCW_READ&&request.source_id==7);
            for(uint32_t k=0;k<3;k++){assert(cx_order_candidates(w,e,count,0)==TERRA_VM_ORDER_EXTERNAL_YIELD);assert(!memcmp(&request,&w->event,sizeof(request)));}
            if(cancel&&!trace.cancelled&&(w->order->phase==2||w->order->phase==3)){
                cx_order_cancel(w);cx_geometry_cancel(w);memset(&w->event,0,sizeof(w->event));
                assert(cx_order_supply_geometry(w,f->bytes+request.offset,request.length)<0);
                trace.cancelled=1;begin=1;continue;
            }
            assert(cx_order_supply_geometry(w,f->bytes+request.offset,request.length)==0);memset(&w->event,0,sizeof(w->event));++trace.reads;
        }
    }
    assert(!"empty-colour differential driver exhausted");return trace;
}
enum { YELLOW, MASKED_FIRST, SEED_EXCLUDED, GATE_SEED, UNREACHABLE, ALL_INVALID, MULTICOLOUR };
static void empty_case(uint32_t kind,int buffered,int cancel){
    Fixture f;shared_red_blue=kind==MASKED_FIRST;empty_world(&f);
    if(buffered){f.world.file=f.bytes;f.world.file_len=f.size;}
    CxWorld a,b;empty_owner(&a,&f,1,buffered);empty_owner(&b,&f,0,buffered);
    TerraVmGateRef input[5]={{4,12*16+2},{4,4*16+2},{4,8*16+2},{4,4*16+2},{99,8*16+2}};
    uint32_t expected=8;
    if(kind==MASKED_FIRST){for(uint32_t i=0;i<4;i++)input[i].group=1;a.command.mask=b.command.mask=2;expected=0;}
    if(kind==SEED_EXCLUDED){a.seed_width=b.seed_width=3;for(uint32_t i=0;i<4;i++)input[i].offset=(3u+i%2u)*16u+2u;expected=0;}
    if(kind==GATE_SEED){a.seed_x=b.seed_x=12;a.seed_y=b.seed_y=2;a.seed_width=b.seed_width=1;a.vm_source_gate=b.vm_source_gate=1;a.command.mask=b.command.mask=0;}
    if(kind==UNREACHABLE){input[0].offset=10*16+8;input[1].offset=4*16+2;input[2]=input[0];input[3]=input[1];}
    if(kind==ALL_INVALID){for(uint32_t i=0;i<5;i++)input[i].group=99;expected=0;}
    if(kind==MULTICOLOUR){input[0].group=4;input[1].group=1;input[2].group=2;input[3].group=4;expected=11;}
    TerraVmGateRef oracle[5],actual[5];memcpy(oracle,input,sizeof(input));memcpy(actual,input,sizeof(input));
    uint32_t hash=fingerprint(&f);TxWorld saved=f.world;
    empty_drive(&a,&f,oracle,5,1,0);EmptyTrace trace=empty_drive(&b,&f,actual,5,0,cancel);
    assert(!memcmp(oracle,actual,sizeof(actual)));assert(trace.colours==expected);assert(b.order->eligible_mask==expected);
    assert(!memcmp(&saved,&f.world,sizeof(saved))&&hash==fingerprint(&f));
    assert(buffered?!trace.reads:trace.reads>0);if(cancel)assert(trace.cancelled);
    if(!expected)assert(!memcmp(input,actual,sizeof(input)));
    if(kind==UNREACHABLE){assert(actual[0].offset==4*16+2&&actual[1].offset==4*16+2);assert(trace.tail_after_rank>20);assert(!b.order->length&&!b.order->visiting);}
    if(kind==YELLOW||kind==GATE_SEED){assert(actual[0].offset==4*16+2&&actual[1].offset==4*16+2);assert(actual[4].group==99);}
    if(kind==YELLOW){
        /* A second, different exact key on the same owner must not inherit the
         * first order's yellow eligibility bit. This cannot be a result hit. */
        for(uint32_t i=0;i<5;i++)input[i].group=99;
        memcpy(oracle,input,sizeof(input));memcpy(actual,input,sizeof(input));
        empty_drive(&a,&f,oracle,5,1,0);trace=empty_drive(&b,&f,actual,5,0,0);
        assert(!memcmp(oracle,actual,sizeof(actual))&&!memcmp(input,actual,sizeof(input)));
        assert(!trace.colours&&!b.order->eligible_mask);
    }
    empty_dispose(&a,1);empty_dispose(&b,0);free(empty_grid);empty_grid=NULL;finish(&f);
}
int main(void){
    uint32_t baseline=tx_native_heap_used();
    for(uint32_t kind=YELLOW;kind<=MULTICOLOUR;kind++){empty_case(kind,0,kind==YELLOW);empty_case(kind,1,0);}
    assert(tx_native_heap_used()==baseline);
    puts("empty-colour H equals frozen f9 for yellow, masked-first, seed/gate, duplicates, unreachable, invalid and pending-cancel cases");return 0;
}
