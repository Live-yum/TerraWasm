/* Exact source-relative Wiring.HitWire lamp ordering. Sparse, budgeted storage;
 * no width*height tiles, Manhattan approximation, or unbounded synchronous BFS. */
#include "terra_circuit_world_internal.h"
#include "terra_circuit_geometry.h"
#include "terra_circuit_order_replay.h"
#include "terra_circuit_order_cache.h"
#ifndef CX_ORDER_CACHE_ENABLED
#define CX_ORDER_CACHE_ENABLED 1
#endif
#include <limits.h>
#include <string.h>

typedef struct OrderItem { TerraVmGateRef event; uint32_t rank,x,y,colour,next; } OrderItem;
typedef struct OrderGroup { uint32_t used,checkpoint,max_x,max_y; } OrderGroup;
typedef struct OrderPoint { uint32_t x,y,direction; } OrderPoint;
typedef struct OrderVisit { uint32_t x,y; uint8_t value,used; uint16_t reserved; } OrderVisit;
struct CxOrder {
    uint32_t count;
    uint32_t gate_target,gate_column,gate_row,gate_remaining;
    CxOrderReplay* replay;CxOrderCandidate candidate;uint32_t prepare_stage,replay_active;
    uint32_t *candidate_heads,candidate_capacity,candidate_clear;
    uint32_t mark_next,mark_rank;
    OrderGroup* groups;uint32_t group_capacity,group_count,group_clear,group_scan,batch_active;
    uint32_t visitor_active,visitor_next,visitor_x,visitor_y,visitor_colour,visitor_net;
    OrderVisit* visits;uint32_t visit_capacity,visit_count,clear_cursor;
    OrderPoint* queue;uint32_t queue_capacity,head,length;
    OrderPoint current;uint32_t visiting,neighbour;uint8_t current_geometry;
    uint8_t eligible_mask; /* Bits 0..3, stored in existing alignment padding. */
    uint64_t seed_scan;
    uint32_t geometry_loading;
    CxGeometryRead pending;
    uint32_t sx,sy,sw,sh,mask,colour,sequence,phase,scan;
    OrderItem *items,*scratch;uint32_t item_capacity,item_count;
    TerraVmGateRef* events;
    uint32_t run,left,mid,right,a,b,out;
};
/* Borrowed optional storage belongs to geometry and is never separately freed.
 * A compile-time OFF control retains the same reduced geometry partition. */
static CxOrderCache* order_cache(CxWorld* w){
#if CX_ORDER_CACHE_ENABLED
    uint32_t bytes=0;void* arena=cx_geometry_order_cache_arena(w,&bytes);
    return cx_order_cache_for_owner(arena,bytes,w);
#else
    (void)w;return NULL;
#endif
}
static void* order_array(CxWorld* w,uint64_t count,uint32_t size){
    if(count>UINT32_MAX/size){cx_fail(w,TCW_MEMORY,"wire ordering allocation exceeds address space");return NULL;}
    void* p=cx_alloc(w,(uint32_t)(count?count:1u)*size);
    if(p&&w->vm&&terra_vm_set_callback_budget(w->vm,cx_vm_available(w))<0){cx_free(w,p);return NULL;}return p;
}
/* Cache slots are never cell identities. FIFO and visit keys are coordinates. */
static int geometry(CxWorld* w,uint32_t x,uint32_t y,uint8_t* out){
    CxOrder* o=w->order;if(x>=w->width||y>=w->height){*out=0;return 0;}
    int point=o->phase==2u||o->phase==3u;
    /* Pending demand admission must finish even if its pinned L1 is readable. */
    if(!o->geometry_loading){
        if(!point&&cx_geometry_get(w,x,y,out)==TCW_OK)return 0;
        int s=point?cx_geometry_point_begin(w,x,y):cx_geometry_begin(w,x);
        if(s<0)return -1;if(s==CX_GEOMETRY_READY)return point?cx_geometry_point_get(w,x,y,out):cx_geometry_get(w,x,y,out);o->geometry_loading=1;
    }
    CxGeometryRead request;int s=cx_geometry_step(w,256u,&request);if(s<0)return -1;
    if(s==CX_GEOMETRY_READ){o->pending=request;w->read_target=CX_READ_GEOMETRY;cx_emit(w,TCW_READ,request.source,request.offset,request.length,NULL);return TERRA_VM_ORDER_EXTERNAL_YIELD;}
    if(s==CX_GEOMETRY_MORE)return 1;o->geometry_loading=0;
    return point?cx_geometry_point_get(w,x,y,out):cx_geometry_get(w,x,y,out);
}
int cx_order_supply_geometry(CxWorld* w,const uint8_t* data,uint32_t length){
    if(!w||!w->order)return TCW_STATE;CxOrder* o=w->order;
    int s=cx_geometry_supply(w,&o->pending,data,length);if(s==TCW_OK)memset(&o->pending,0,sizeof(o->pending));return s;
}
/* Existing compiler prefix defines active420 numbering, including unwired gates. */
int cx_order_gate_position_step(CxWorld* w,uint32_t gate,uint32_t* x,uint32_t* y){
    CxOrder* o=w->order;if(!o||!gate||gate>w->gates||!w->column_gates)return CX_ORDER_GATE_ABSENT;
    if(o->gate_target!=gate){
        uint32_t lo=0,hi=w->width;
        while(lo<hi){uint32_t mid=lo+(hi-lo)/2u;if(w->column_gates[mid+1u]<gate)lo=mid+1u;else hi=mid;}
        if(lo>=w->width||w->column_gates[lo]>=gate)return CX_ORDER_GATE_ABSENT;
        o->gate_target=gate;o->gate_column=lo;o->gate_row=0;o->gate_remaining=gate-w->column_gates[lo];
    }
    uint8_t word;int status=geometry(w,o->gate_column,o->gate_row,&word);if(status)return status;
    for(uint32_t n=0;n<256u&&o->gate_row<w->height;n++){
        if(cx_geometry_get(w,o->gate_column,o->gate_row,&word)!=TCW_OK)return -1;
        if((word&128u)&&!--o->gate_remaining){*x=o->gate_column;*y=o->gate_row;o->gate_target=0;return 0;}
        ++o->gate_row;
    }
    return o->gate_row<w->height?1:CX_ORDER_GATE_ABSENT;
}
int cx_order_capture_column(CxWorld* w){
    if(w->twld_state)return TCW_OK; /* Explicit mod rule profile remains unchanged. */
    if(!w->order){
        if(w->height>0xffffffu||w->wire_cells>UINT32_MAX/4u)return cx_fail(w,TCW_MEMORY,"wire ordering index exceeds coordinate capacity");
        CxOrder* o=(CxOrder*)cx_alloc(w,sizeof(CxOrder));if(!o)return TCW_MEMORY;memset(o,0,sizeof(*o));w->order=o;
        if(cx_geometry_create(w)!=TCW_OK)return TCW_MEMORY;
#if CX_ORDER_CACHE_ENABLED
        uint32_t cache_bytes=0;void* cache_arena=cx_geometry_order_cache_arena(w,&cache_bytes);
        if(cache_arena&&!cx_order_cache_init(cache_arena,cache_bytes,w))return cx_fail(w,TCW_STATE,"order cache arena contract mismatch");
#endif
    }
    CxOrder* o=w->order;
    for(uint32_t y=0;y<w->height;y++){
        TxTile* t=w->column+y;uint32_t mask=cx_wire_mask(t);
        if(!mask)continue;
        if(o->count>=w->wire_cells)return cx_fail(w,TCW_STATE,"wire ordering index count mismatch");
        ++o->count;

    }
    return TCW_OK;
}
void cx_order_cancel(CxWorld* w){if(!w||!w->order)return;cx_order_cache_cancel(order_cache(w));CxOrder* o=w->order;memset(&o->candidate,0,sizeof(o->candidate));memset(&o->pending,0,sizeof(o->pending));o->prepare_stage=o->replay_active=o->geometry_loading=o->gate_target=o->batch_active=o->visitor_active=0;cx_order_replay_cancel(o->replay);}
void cx_order_close(CxWorld* w){
    CxOrder* o=w->order;if(!o)return;
    cx_order_replay_destroy(w,o->replay);cx_free(w,o->groups);cx_free(w,o->candidate_heads);cx_free(w,o->visits);cx_free(w,o->queue);cx_free(w,o->items);cx_free(w,o->scratch);cx_free(w,o);w->order=NULL;
}
static int enqueue(CxWorld* w,uint32_t x,uint32_t y,uint32_t direction){
    CxOrder* o=w->order;
    if(o->length==o->queue_capacity){uint32_t cap=o->queue_capacity?o->queue_capacity*2u:256u;if(cap<o->queue_capacity)return -1;
        OrderPoint* p=order_array(w,cap,sizeof(*p));if(!p)return -1;
        for(uint32_t i=0;i<o->length;i++)p[i]=o->queue[(o->head+i)%o->queue_capacity];cx_free(w,o->queue);o->queue=p;o->queue_capacity=cap;o->head=0;}
    o->queue[(o->head+o->length)%o->queue_capacity]=(OrderPoint){x,y,direction};++o->length;return 0;
}
static uint32_t visit_hash(uint32_t x,uint32_t y){uint32_t h=x*0x9e3779b1u^y*0x85ebca6bu;return h^(h>>16);}
static OrderVisit* visit(CxWorld* w,uint32_t x,uint32_t y,int create){
    CxOrder* o=w->order;uint32_t slot=0;
    if(o->visit_capacity){slot=visit_hash(x,y)&(o->visit_capacity-1u);while(o->visits[slot].used){if(o->visits[slot].x==x&&o->visits[slot].y==y)return o->visits+slot;slot=(slot+1u)&(o->visit_capacity-1u);}}
    if(!create)return NULL;
    if(!o->visit_capacity||o->visit_count+1u>o->visit_capacity/2u){
        uint32_t cap=o->visit_capacity?o->visit_capacity*2u:256u;if(cap<o->visit_capacity)return NULL;
        /* Old and new payload coexist and are charged before publication. */
        OrderVisit* p=order_array(w,cap,sizeof(*p));if(!p)return NULL;memset(p,0,cap*sizeof(*p));
        for(uint32_t i=0;i<o->visit_capacity;i++)if(o->visits[i].used){uint32_t j=visit_hash(o->visits[i].x,o->visits[i].y)&(cap-1u);while(p[j].used)j=(j+1u)&(cap-1u);p[j]=o->visits[i];}
        cx_free(w,o->visits);o->visits=p;o->visit_capacity=cap;slot=visit_hash(x,y)&(cap-1u);while(p[slot].used)slot=(slot+1u)&(cap-1u);
    }
    o->visits[slot]=(OrderVisit){x,y,0,1,0};++o->visit_count;return o->visits+slot;
}
static int order_begin(CxWorld* w,TerraVmGateRef* events,uint32_t count){
    CxOrder* o=w->order;o->events=events;o->item_count=count;o->phase=8;o->scan=0;o->colour=0;o->sequence=0;o->length=o->head=0;o->visiting=o->neighbour=o->clear_cursor=o->geometry_loading=0;o->seed_scan=0;o->gate_target=0;o->prepare_stage=o->replay_active=o->candidate_clear=o->group_count=o->group_clear=o->group_scan=o->batch_active=o->visitor_active=0;memset(&o->candidate,0,sizeof(o->candidate));cx_order_replay_cancel(o->replay);cx_geometry_cancel(w);memset(&o->pending,0,sizeof(o->pending));
    o->eligible_mask=0;o->sx=w->seed_x;o->sy=w->seed_y;o->sw=w->seed_width;o->sh=w->seed_height;o->mask=w->command.kind==TCW_TRIGGER?w->command.mask:15u;

    if(count>o->item_capacity){
        OrderItem* a=(OrderItem*)order_array(w,count,sizeof(OrderItem));if(!a)return -1;
        OrderItem* b=(OrderItem*)order_array(w,count,sizeof(OrderItem));if(!b){cx_free(w,a);return -1;}
        cx_free(w,o->items);cx_free(w,o->scratch);o->items=a;o->scratch=b;o->item_capacity=count;
    }
    uint32_t capacity=1;while(capacity<(uint64_t)count*2u){if(capacity>UINT32_MAX/2u)return -1;capacity*=2u;}
    if(capacity>o->candidate_capacity){uint32_t* heads=order_array(w,capacity,4);if(!heads)return -1;cx_free(w,o->candidate_heads);o->candidate_heads=heads;o->candidate_capacity=capacity;}
    return 0;
}
static int replay_geometry(void* context,uint32_t x,uint32_t y,uint8_t* word,uint8_t* route){
    CxWorld* w=context;int s=geometry(w,x,y,word);return s?s:cx_geometry_get_connectivity(w,x,y,word,route);
}
static uint32_t candidate_slot(CxOrder* o,uint32_t x,uint32_t y){uint32_t slot=visit_hash(x,y)&(o->candidate_capacity-1u);while(o->candidate_heads[slot]){OrderItem* item=o->items+o->candidate_heads[slot]-1u;if(item->x==x&&item->y==y)break;slot=(slot+1u)&(o->candidate_capacity-1u);}return slot;}
/* Grow only for encountered standard checkpoint groups. Old and new remain
 * charged until the complete replacement is ready; failure preserves old data. */
static OrderGroup* candidate_group(CxWorld* w,uint32_t checkpoint){
    CxOrder* o=w->order;uint32_t slot=0;
    if(o->group_capacity){slot=(checkpoint*0x9e3779b1u)&(o->group_capacity-1u);
        while(o->groups[slot].used){if(o->groups[slot].checkpoint==checkpoint)return o->groups+slot;slot=(slot+1u)&(o->group_capacity-1u);}}
    if(!o->group_capacity||o->group_count+1u>o->group_capacity/2u){
        uint32_t capacity=o->group_capacity?o->group_capacity*2u:8u;if(capacity<o->group_capacity)return NULL;
        OrderGroup* groups=order_array(w,capacity,sizeof(*groups));if(!groups)return NULL;
        memset(groups,0,capacity*sizeof(*groups));
        for(uint32_t i=0;i<o->group_capacity;i++)if(o->groups[i].used){uint32_t j=(o->groups[i].checkpoint*0x9e3779b1u)&(capacity-1u);while(groups[j].used)j=(j+1u)&(capacity-1u);groups[j]=o->groups[i];}
        cx_free(w,o->groups);o->groups=groups;o->group_capacity=capacity;o->group_clear=capacity;
        slot=(checkpoint*0x9e3779b1u)&(capacity-1u);while(groups[slot].used)slot=(slot+1u)&(capacity-1u);
    }
    OrderGroup* group=o->groups+slot;*group=(OrderGroup){1u,checkpoint,0,0};++o->group_count;return group;
}
static int prepare_candidate(CxWorld* w){
    CxOrder* o=w->order;OrderItem* item=o->items+o->scan;
    if(!o->prepare_stage){item->event=o->events[o->scan];item->rank=UINT32_MAX;item->colour=4u;item->next=0;memset(&o->candidate,0,sizeof(o->candidate));o->prepare_stage=1;}
    if(o->prepare_stage==1){int s=cx_order_candidate_step(w,item->event,&o->candidate);if(s)return s;o->prepare_stage=2;}
    CxOrderCandidate* candidate=&o->candidate;
    if(candidate->valid&&o->prepare_stage==2){uint8_t word;int s=geometry(w,candidate->x,candidate->y,&word);if(s)return s;if(!(word&64u))candidate->valid=0;o->prepare_stage=3;}
    if(candidate->valid){
        item->x=candidate->x;item->y=candidate->y;
        if(candidate->direct){
            for(uint32_t c=0;c<4;c++)if(candidate->nets[c]==item->event.group){item->colour=c;break;}
        }else{
            item->rank=UINT32_MAX-1u;uint32_t checkpoint=item->x/CX_CHECK_COLUMNS;
            OrderGroup* group=candidate_group(w,checkpoint);if(!group)return -1;
            if(item->x>group->max_x){group->max_x=item->x;group->max_y=item->y;}
            else if(item->x==group->max_x&&item->y>group->max_y)group->max_y=item->y;
        }
        if(item->rank==UINT32_MAX-1u||item->colour<4u){uint32_t slot=candidate_slot(o,item->x,item->y);item->next=o->candidate_heads[slot];o->candidate_heads[slot]=o->scan+1u;}

    }
    memset(candidate,0,sizeof(*candidate));o->prepare_stage=0;++o->scan;return 0;
}
/* One candidate-chain entry per visitor call. The replay engine holds its
 * committed topology cell until this visitor finishes; original indices stay. */
static int batch_candidate(void* context,uint32_t x,uint32_t y,uint32_t colour,uint32_t net,uint8_t word){
    CxWorld* w=context;CxOrder* o=w->order;if(!(word&64u))return 0;
    if(!o->visitor_active){o->visitor_active=1;o->visitor_x=x;o->visitor_y=y;o->visitor_colour=colour;o->visitor_net=net;o->visitor_next=o->candidate_heads[candidate_slot(o,x,y)];}
    else if(o->visitor_x!=x||o->visitor_y!=y||o->visitor_colour!=colour||o->visitor_net!=net)return -1;
    if(o->visitor_next){uint32_t index=o->visitor_next-1u;if(index>=o->item_count)return -1;OrderItem* item=o->items+index;o->visitor_next=item->next;
        if(item->rank==UINT32_MAX-1u&&item->event.group==net&&colour<item->colour)item->colour=colour;}
    if(o->visitor_next)return 1;o->visitor_active=0;return 0;
}
/* Traversal/reset/merge are chunked. Growth copy/rehash latency remains linear. */
int32_t cx_order_candidates(void* context,TerraVmGateRef* events,uint32_t count,uint32_t begin){
    CxWorld* w=(CxWorld*)context;CxOrder* o=w->order;
    if(!o||count<2u)return 0;
    if(terra_vm_set_callback_budget(w->vm,cx_vm_available(w))<0)return -1;
    if(begin){
        if(order_begin(w,events,count)<0)return -1;
        CxOrderCache* cache=order_cache(w);
        if(cache){
            CxOrderCacheKey key={w->vm_source_gate,o->sx,o->sy,o->sw,o->sh,o->mask,count};
            cx_order_cache_begin(cache,&key);o->phase=10;
        }
    }
    if(o->phase==10){
        int status=cx_order_cache_lookup_step(order_cache(w),events,256u);
        if(status==CX_ORDER_CACHE_MORE)return 1;
        if(status==CX_ORDER_CACHE_HIT){w->eval_valid=0;o->phase=11;}
        else if(status==CX_ORDER_CACHE_MISS)o->phase=8;
        else return -1;
    }
    if(o->phase==11){
        int status=cx_order_cache_copy_step(order_cache(w),events,256u);
        if(status==CX_ORDER_CACHE_MORE)return 1;
        if(status!=CX_ORDER_CACHE_HIT)return -1;
        cx_order_cache_complete(order_cache(w));o->phase=12;return 0;
    }
    if(o->phase==12)return 0;
    if(o->phase==8){
        if(w->vm_source_gate){int s=cx_order_gate_position_step(w,w->vm_source_gate,&o->sx,&o->sy);if(s)return s==CX_ORDER_GATE_ABSENT?-1:s;o->sw=o->sh=1;o->mask=15u;}
        o->phase=0;
    }
    if(o->phase==0){
        if(o->candidate_clear<o->candidate_capacity){uint32_t n=o->candidate_capacity-o->candidate_clear;if(n>256u)n=256u;memset(o->candidate_heads+o->candidate_clear,0,n*4u);o->candidate_clear+=n;return 1;}
        if(o->group_clear<o->group_capacity){uint32_t n=o->group_capacity-o->group_clear;if(n>256u)n=256u;memset(o->groups+o->group_clear,0,n*sizeof(OrderGroup));o->group_clear+=n;return 1;}
        if(o->scan<o->item_count){int s=prepare_candidate(w);return s?s:1;}
        o->phase=9;o->scan=0;
    }
    if(o->phase==9){
        if(o->group_scan<o->group_capacity){OrderGroup* group=o->groups+o->group_scan;
            if(!group->used){++o->group_scan;return 1;}
            int status;if(!o->replay){status=cx_order_replay_create(w,&o->replay);if(status<0)return -1;}
            if(!o->batch_active){status=cx_order_replay_begin_batch(o->replay,group->max_x,group->max_y);if(status<0)return -1;o->batch_active=1;o->visitor_active=0;}
            status=cx_order_replay_step_batch(w,o->replay,256u,replay_geometry,w,batch_candidate,w);if(status)return status;
            o->batch_active=0;++o->group_scan;return 1;
        }
        /* Rank is prep-only standard metadata until every batch is done. */
        if(o->scan<o->item_count){uint32_t end=o->scan+256u;if(end<o->scan||end>o->item_count)end=o->item_count;while(o->scan<end){OrderItem* item=o->items+o->scan++;
            /* First matching colour is fixed before mask filtering. Unreachable
             * eligible targets retain their bit: this is not a tail prune. */
            if(item->colour<4u&&(o->mask&(1u<<item->colour))&&
               !(item->x>=o->sx&&item->x-o->sx<o->sw&&item->y>=o->sy&&item->y-o->sy<o->sh))
                o->eligible_mask=(uint8_t)(o->eligible_mask|(1u<<item->colour));
            item->rank=UINT32_MAX;
        }return 1;}
        o->phase=1;o->scan=0;
    }
    if(o->phase==1){
        /* Only enter here at a fresh colour boundary, never during a READ or
         * active traversal. Empty colours cannot assign any candidate rank. */
        while(o->colour<4u&&!(o->eligible_mask&(1u<<o->colour)))++o->colour;
        if(o->colour==4u){o->phase=4;o->scan=0;return 1;}
        if(o->clear_cursor<o->visit_capacity){uint32_t n=o->visit_capacity-o->clear_cursor;if(n>256u)n=256u;memset(o->visits+o->clear_cursor,0,n*sizeof(*o->visits));o->clear_cursor+=n;return 1;}
        o->visit_count=0;
        o->seed_scan=0;o->phase=2;
    }
    if(o->phase==2){
        uint64_t area=(uint64_t)o->sw*o->sh;
        if(o->seed_scan<area){uint32_t x=o->sx+(uint32_t)(o->seed_scan/o->sh),y=o->sy+(uint32_t)(o->seed_scan%o->sh);uint8_t word;int s=geometry(w,x,y,&word);if(s)return s;
            if(word&(1u<<o->colour)){OrderVisit* v=visit(w,x,y,1);if(!v)return -1;if(enqueue(w,x,y,0)<0)return -1;v->value=4;}
            ++o->seed_scan;return 1;}
        o->phase=3;
    }
    if(o->phase==3){
        if(!o->visiting){
            if(!o->length){++o->colour;o->phase=1;o->clear_cursor=0;return 1;}
            OrderPoint entry=o->queue[o->head];uint8_t word;int s=geometry(w,entry.x,entry.y,&word);if(s)return s;
            o->current=entry;o->current_geometry=word;o->neighbour=0;o->visiting=1;
            o->head=(o->head+1u)%o->queue_capacity;--o->length;
            o->mark_next=0;o->mark_rank=UINT32_MAX;
            if((word&64u)&&!(entry.x>=o->sx&&entry.x-o->sx<o->sw&&entry.y>=o->sy&&entry.y-o->sy<o->sh))o->mark_next=o->candidate_heads[candidate_slot(o,entry.x,entry.y)];
        }
        if(o->mark_next){OrderItem* item=o->items+o->mark_next-1u;o->mark_next=item->next;
            if(item->colour==o->colour&&item->rank==UINT32_MAX){if(o->mark_rank==UINT32_MAX)o->mark_rank=o->sequence++;item->rank=o->mark_rank;}return 1;}
        static const int8_t dx[4]={0,0,1,-1},dy[4]={1,-1,0,0};static const uint8_t routes[3][4]={{0,1,2,3},{3,2,1,0},{2,3,0,1}};
        if(o->neighbour<4u){uint32_t d=o->neighbour,route=(o->current_geometry>>4)&3u;
            if(route&&routes[route-1u][o->current.direction]!=d){++o->neighbour;return 1;}
            int64_t xx=(int64_t)o->current.x+dx[d],yy=(int64_t)o->current.y+dy[d];
            if(xx<2||yy<2||xx>=(int64_t)w->width-2||yy>=(int64_t)w->height-2){++o->neighbour;return 1;}
            uint32_t x=(uint32_t)xx,y=(uint32_t)yy;uint8_t word;int s=geometry(w,x,y,&word);if(s)return s;
            if(word&(1u<<o->colour)){
                OrderVisit* v=visit(w,x,y,0);
                if(v&&v->value)--v->value;
                else{if(!((word>>4)&3u)){v=visit(w,x,y,1);if(!v)return -1;}
                    if(enqueue(w,x,y,d)<0)return -1;if(!((word>>4)&3u))v->value=3;}
            }
            ++o->neighbour;return 1;
        }
        o->visiting=0;return 1;
    }
    if(o->phase==4){o->run=1;o->left=0;o->phase=5;return 1;}
    if(o->phase==5){
        if(o->run>=o->item_count){o->phase=7;o->scan=0;return 1;}
        if(o->left>=o->item_count){OrderItem* swap=o->items;o->items=o->scratch;o->scratch=swap;o->run*=2u;o->left=0;return 1;}
        o->mid=o->left+o->run;if(o->mid>o->item_count)o->mid=o->item_count;o->right=o->mid+o->run;if(o->right>o->item_count)o->right=o->item_count;
        o->a=o->left;o->b=o->mid;o->out=o->left;o->phase=6;
    }
    if(o->phase==6){
        if(o->out<o->right){uint32_t i;if(o->a<o->mid&&(o->b>=o->right||o->items[o->a].rank<=o->items[o->b].rank))i=o->a++;else i=o->b++;o->scratch[o->out++]=o->items[i];return 1;}
        o->left=o->right;o->phase=5;return 1;
    }
    if(o->phase==7){
        if(o->scan<o->item_count){uint32_t i=o->scan++;o->events[i]=o->items[i].event;cx_order_cache_capture_output(order_cache(w),i,o->events[i]);return 1;}
        cx_order_cache_complete(order_cache(w));o->phase=12;return 0;
    }
    return 1;
}
