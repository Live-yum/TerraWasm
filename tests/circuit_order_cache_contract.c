/* Standalone synthetic module contract, no fixture or allocator dependencies.
 * Compile this translation unit only. Tests include implementation for arena
 * canaries and generation-overflow inspection. */
#include "../src/terra_circuit_order_cache.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static CxOrderCacheKey key(uint32_t n){CxOrderCacheKey k={1,2,3,4,5,15,n};return k;}
static TerraVmGateRef event(uint32_t i){TerraVmGateRef e={i%3?i%5:UINT32_MAX,i%7};return e;}
static int lookup(CxOrderCache* c,TerraVmGateRef* e){
    int r;unsigned steps=0;
    do{r=cx_order_cache_lookup_step(c,e,1);assert(++steps<300000);}while(r==CX_ORDER_CACHE_MORE);
    return r;
}
static void inputs(TerraVmGateRef* e,uint32_t n){for(uint32_t i=0;i<n;i++)e[i]=event(i);}
static void publish(CxOrderCache* c,CxOrderCacheKey k,TerraVmGateRef* e){
    cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
    for(uint32_t i=0;i<k.count;i++)cx_order_cache_capture_output(c,i,event(k.count-1-i));
    cx_order_cache_complete(c);
}
static void hit(CxOrderCache* c,CxOrderCacheKey k,TerraVmGateRef* e){
    cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_HIT);
    assert(cx_order_cache_copy_step(c,e,0)==(k.count?CX_ORDER_CACHE_MORE:CX_ORDER_CACHE_HIT));
    for(uint32_t i=0;i<k.count;i++){
        int r=cx_order_cache_copy_step(c,e,1);assert(r==(i+1==k.count?CX_ORDER_CACHE_HIT:CX_ORDER_CACHE_MORE));
        TerraVmGateRef want=event(k.count-1-i);assert(e[i].group==want.group&&e[i].offset==want.offset);
    }
    assert(cx_order_cache_copy_step(c,e,1)==CX_ORDER_CACHE_HIT);
    cx_order_cache_complete(c);
}
static void exact_and_boundaries(CxOrderCache* c){
    TerraVmGateRef e[257];uint32_t counts[]={0,1,16,17,64,65,256};
    for(unsigned j=0;j<sizeof(counts)/sizeof(counts[0]);j++){
        CxOrderCacheKey k=key(counts[j]);inputs(e,k.count);publish(c,k,e);inputs(e,k.count);hit(c,k,e);
    }
    CxOrderCacheKey k=key(257);inputs(e,257);cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
    for(unsigned i=0;i<257;i++)cx_order_cache_capture_output(c,i,e[i]);
    cx_order_cache_complete(c);
    k=key(16);inputs(e,16);
    for(unsigned f=0;f<7;f++){
        CxOrderCacheKey changed=k;
        switch(f){case 0:changed.source_gate++;break;case 1:changed.sx++;break;case 2:changed.sy++;break;case 3:changed.sw++;break;case 4:changed.sh++;break;case 5:changed.mask++;break;default:changed.count=15;}
        cx_order_cache_begin(c,&changed);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
        /* Abort clears prior entries; restore the exact baseline. */
        cx_order_cache_cancel(c);publish(c,k,e);
    }
    for(unsigned f=0;f<3;f++){
        inputs(e,16);if(f==0)e[0].group^=1;if(f==1)e[0].offset^=1;
        if(f==2){TerraVmGateRef t=e[0];e[0]=e[1];e[1]=t;}
        cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);cx_order_cache_cancel(c);
        inputs(e,16);publish(c,k,e);
    }
}
static void cancellation(CxOrderCache* c){
    TerraVmGateRef e[16];CxOrderCacheKey k=key(16);inputs(e,16);cx_order_cache_cancel(c);
    publish(c,k,e);cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_HIT);
    assert(cx_order_cache_copy_step(c,e,1)==CX_ORDER_CACHE_MORE);cx_order_cache_cancel(c);
    inputs(e,16);cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
    cx_order_cache_capture_output(c,0,e[0]);cx_order_cache_complete(c); /* incomplete cannot publish */
    cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
    cx_order_cache_capture_output(c,1,e[1]); /* out of order invalidates */
    cx_order_cache_complete(c);cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
    cx_order_cache_cancel(c);publish(c,k,e);
    cx_order_cache_begin(c,&k);assert(cx_order_cache_lookup_step(c,e,0)==CX_ORDER_CACHE_MORE);
    assert(cx_order_cache_lookup_step(c,e,1)==CX_ORDER_CACHE_MORE);assert(c->snapshot==1);
    cx_order_cache_cancel(c);cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);cx_order_cache_cancel(c);
}
static void postcancel_fifo(CxOrderCache* c){
    TerraVmGateRef e[256];CxOrderCacheKey k=key(256);inputs(e,256);
    cx_order_cache_cancel(c);
    for(uint32_t i=0;i<5;i++){k.source_gate=200+i;publish(c,k,e);}
    assert(c->victim[2]==1);
    cx_order_cache_cancel(c);
    assert(c->victim[0]==0&&c->victim[1]==0&&c->victim[2]==0);
    for(uint32_t i=0;i<5;i++){k.source_gate=300+i;publish(c,k,e);}
    k.source_gate=301;cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_HIT);
    assert(cx_order_cache_copy_step(c,e,256)==CX_ORDER_CACHE_HIT);cx_order_cache_complete(c);
    inputs(e,256);k.source_gate=300;cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);
    cx_order_cache_cancel(c);
}
static void capacity(CxOrderCache* c){
    uint32_t counts[]={16,64,256},caps[]={768,64,4};TerraVmGateRef e[256];
    for(unsigned t=0;t<3;t++){
        cx_order_cache_cancel(c);CxOrderCacheKey k=key(counts[t]);inputs(e,k.count);
        for(uint32_t i=0;i<caps[t]+1;i++){k.source_gate=100+i;publish(c,k,e);}
        k.source_gate=100;cx_order_cache_begin(c,&k);assert(lookup(c,e)==CX_ORDER_CACHE_MISS);cx_order_cache_cancel(c);
    }
}
int main(void){
    size_t n=cx_order_cache_required_bytes(),a=cx_order_cache_alignment();int owner1,owner2;
    assert(n<=384u*1024u);unsigned char* raw=malloc(n+3*a);assert(raw);
    uintptr_t aligned=((uintptr_t)raw+a+a-1)/a*a;unsigned char* arena=(unsigned char*)aligned;
    memset(raw,0xa5,n+3*a);
    assert(!cx_order_cache_init(NULL,n,&owner1));assert(!cx_order_cache_init(arena,n-1,&owner1));
    assert(!cx_order_cache_init(arena+1,n,&owner1));assert(!cx_order_cache_init(arena,n,NULL));
    memset(arena,0,n);
    assert(!cx_order_cache_for_owner(arena,n,&owner1));
    CxOrderCache* c=cx_order_cache_init(arena,n,&owner1);assert(c&&c->owner==&owner1);
    assert(cx_order_cache_for_owner(arena,n,&owner1)==c);
    assert(!cx_order_cache_for_owner(arena,n,&owner2));
    assert(!cx_order_cache_for_owner(arena,n-1,&owner1));
    assert(!cx_order_cache_for_owner(arena+1,n,&owner1));
    assert(!cx_order_cache_for_owner(NULL,n,&owner1));
    assert(!cx_order_cache_for_owner(arena,n,NULL));
    exact_and_boundaries(c);cancellation(c);capacity(c);postcancel_fifo(c);
    assert(arena[-1]==0xa5&&arena[n]==0xa5);
    c->epoch=UINT64_MAX;cx_order_cache_cancel(c);assert(c->disabled);
    TerraVmGateRef e=event(0);CxOrderCacheKey k=key(1);cx_order_cache_begin(c,&k);assert(lookup(c,&e)==CX_ORDER_CACHE_MISS);cx_order_cache_complete(c);
    c=cx_order_cache_init(arena,n,&owner2);assert(c->owner==&owner2&&!c->disabled);
    assert(!cx_order_cache_for_owner(arena,n,&owner1));
    assert(cx_order_cache_for_owner(arena,n,&owner2)==c);
    cx_order_cache_begin(c,&k);assert(lookup(c,&e)==CX_ORDER_CACHE_MISS);cx_order_cache_cancel(c);
    cx_order_cache_begin(NULL,&k);assert(cx_order_cache_lookup_step(NULL,&e,1)==CX_ORDER_CACHE_MISS);
    assert(cx_order_cache_copy_step(NULL,&e,1)==CX_ORDER_CACHE_MISS);
    cx_order_cache_capture_output(NULL,0,e);cx_order_cache_complete(NULL);cx_order_cache_cancel(NULL);
    assert(arena[-1]==0xa5&&arena[n]==0xa5);free(raw);puts("order cache contract: OK");return 0;
}
