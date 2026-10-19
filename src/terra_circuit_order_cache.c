#include "terra_circuit_order_cache.h"
#include <string.h>
#define OC_RECORDS 836u
#define OC_MEMBERS 17408u
#define OC_NONE UINT32_MAX
/* No padding bytes participate in key equality. */
typedef struct OcRecord { uint64_t epoch; uint32_t key[7]; } OcRecord;
struct CxOrderCache {
    const void* owner;
    uint64_t epoch;
    uint32_t disabled,state,key[7],tier,first,limit,capacity,base;
    uint32_t snapshot,record,field,member,empty,selected,copied,captured;
    uint32_t victim[3];
    OcRecord records[OC_RECORDS];
    TerraVmGateRef inputs[OC_MEMBERS],outputs[OC_MEMBERS],pending[256];
};
enum { OC_IDLE,OC_SNAPSHOT,OC_SCAN,OC_MISS,OC_HIT,OC_COPIED,OC_UNCOVERED };
_Static_assert(sizeof(CxOrderCache)<=384u*1024u,"order cache arena ceiling");
size_t cx_order_cache_required_bytes(void){return sizeof(CxOrderCache);}
size_t cx_order_cache_alignment(void){return _Alignof(CxOrderCache);}
CxOrderCache* cx_order_cache_init(void* arena,size_t bytes,const void* owner){
    CxOrderCache* c;
    if(!arena||!owner||bytes<sizeof(CxOrderCache)||(uintptr_t)arena%_Alignof(CxOrderCache))return NULL;
    c=(CxOrderCache*)arena;memset(c,0,sizeof(*c));c->owner=owner;c->epoch=1;return c;
}
CxOrderCache* cx_order_cache_for_owner(void* arena,size_t bytes,const void* owner){
    CxOrderCache* c;
    if(!arena||!owner||bytes<sizeof(CxOrderCache)||(uintptr_t)arena%_Alignof(CxOrderCache))return NULL;
    c=(CxOrderCache*)arena;
    return c->owner==owner&&c->epoch!=0?c:NULL;
}
void cx_order_cache_cancel(CxOrderCache* c){
    if(!c)return;
    c->state=OC_IDLE;
    c->victim[0]=c->victim[1]=c->victim[2]=0;
    if(c->epoch==UINT64_MAX)c->disabled=1;else c->epoch++;
}
void cx_order_cache_begin(CxOrderCache* c,const CxOrderCacheKey* k){
    if(!c)return;
    if(c->state!=OC_IDLE)cx_order_cache_cancel(c);
    if(!k||c->disabled){c->state=OC_UNCOVERED;return;}
    c->key[0]=k->source_gate;c->key[1]=k->sx;c->key[2]=k->sy;
    c->key[3]=k->sw;c->key[4]=k->sh;c->key[5]=k->mask;c->key[6]=k->count;
    if(k->count>256){c->state=OC_UNCOVERED;return;}
    c->tier=k->count<=16?0u:k->count<=64?1u:2u;
    c->first=c->tier==0?0u:c->tier==1?768u:832u;
    c->limit=c->tier==0?768u:c->tier==1?64u:4u;
    c->capacity=c->tier==0?16u:c->tier==1?64u:256u;
    c->base=c->tier==0?0u:c->tier==1?12288u:16384u;
    c->snapshot=c->record=c->field=c->member=c->copied=c->captured=0;
    c->empty=c->selected=OC_NONE;c->state=OC_SNAPSHOT;
}
static uint32_t oc_offset(const CxOrderCache* c,uint32_t r){return c->base+r*c->capacity;}
static int oc_miss(CxOrderCache* c){
    c->selected=c->empty;
    if(c->selected==OC_NONE){c->selected=c->victim[c->tier];c->victim[c->tier]=(c->selected+1u)%c->limit;}
    c->records[c->first+c->selected].epoch=0;
    memcpy(c->records[c->first+c->selected].key,c->key,sizeof(c->key));
    c->state=OC_MISS;return CX_ORDER_CACHE_MISS;
}
int cx_order_cache_lookup_step(CxOrderCache* c,const TerraVmGateRef* events,uint32_t work){
    if(!c)return CX_ORDER_CACHE_MISS;
    if(c->state==OC_MISS||c->state==OC_UNCOVERED||c->state==OC_IDLE)return CX_ORDER_CACHE_MISS;
    if(c->state==OC_HIT||c->state==OC_COPIED)return CX_ORDER_CACHE_HIT;
    if(!events&&c->key[6]){cx_order_cache_cancel(c);return CX_ORDER_CACHE_MISS;}
    while(work){
        OcRecord* r;
        if(c->state==OC_SNAPSHOT){
            if(c->snapshot<c->key[6]){c->pending[c->snapshot]=events[c->snapshot];c->snapshot++;work--;continue;}
            c->state=OC_SCAN;
        }
        if(c->record==c->limit)return oc_miss(c);
        r=&c->records[c->first+c->record];
        if(c->field==0){
            work--;
            if(r->epoch!=c->epoch){if(c->empty==OC_NONE)c->empty=c->record;c->record++;continue;}
            c->field=1;continue;
        }
        if(c->field<=7){
            uint32_t f=c->field-1u;work--;
            if(r->key[f]!=c->key[f]){c->record++;c->field=c->member=0;continue;}
            c->field++;continue;
        }
        if(c->member<c->key[6]){
            TerraVmGateRef a=c->inputs[oc_offset(c,c->record)+c->member],b=c->pending[c->member];work--;
            if(a.group!=b.group||a.offset!=b.offset){c->record++;c->field=c->member=0;continue;}
            c->member++;continue;
        }
        c->selected=c->record;c->state=OC_HIT;return CX_ORDER_CACHE_HIT;
    }
    return CX_ORDER_CACHE_MORE;
}
int cx_order_cache_copy_step(CxOrderCache* c,TerraVmGateRef* events,uint32_t work){
    if(!c||(c->state!=OC_HIT&&c->state!=OC_COPIED))return CX_ORDER_CACHE_MISS;
    if(!events&&c->key[6]){cx_order_cache_cancel(c);return CX_ORDER_CACHE_MISS;}
    while(work&&c->copied<c->key[6]){events[c->copied]=c->outputs[oc_offset(c,c->selected)+c->copied];c->copied++;work--;}
    if(c->copied==c->key[6]){c->state=OC_COPIED;return CX_ORDER_CACHE_HIT;}
    return CX_ORDER_CACHE_MORE;
}
void cx_order_cache_capture_output(CxOrderCache* c,uint32_t index,TerraVmGateRef event){
    uint32_t at;
    if(!c||c->state==OC_UNCOVERED)return;
    if(c->state!=OC_MISS||index!=c->captured||index>=c->key[6]){cx_order_cache_cancel(c);return;}
    at=oc_offset(c,c->selected)+index;c->inputs[at]=c->pending[index];c->outputs[at]=event;c->captured++;
}
void cx_order_cache_complete(CxOrderCache* c){
    if(!c)return;
    if(c->state==OC_MISS&&c->captured==c->key[6])c->records[c->first+c->selected].epoch=c->epoch;
    else if(c->state!=OC_COPIED&&c->state!=OC_UNCOVERED){cx_order_cache_cancel(c);return;}
    c->state=OC_IDLE;
}
