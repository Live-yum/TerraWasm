#include "terra_circuit.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint32_t tx_native_heap_used(void);
extern void txw_test_allocation_limit(uint32_t maximum);
extern void tx_reset_heap(void);
#define OK(call) assert((call) == TERRA_CIRCUIT_OK)
#define BYTES (96u * 1024u * 1024u)
static uint32_t create(uint32_t width, uint32_t height, uint32_t count) {
    uint32_t h = 0; OK(terra_circuit_create(width,height,count,BYTES,&h)); assert(h); return h;
}
static TerraCircuitStats stats(uint32_t h) { TerraCircuitStats out; OK(terra_circuit_stats(h,&out)); return out; }
static void compile(uint32_t h, uint32_t chunk) {
    uint32_t compiled; int32_t status;
    do { status = terra_circuit_compile(h,chunk,&compiled); assert(status >= 0); } while (status == TERRA_CIRCUIT_MORE);
    assert(compiled == stats(h).cell_count);
}
static void lifecycle_contract(void) {
    uint32_t baseline = tx_native_heap_used(), h = 99;
    assert(terra_circuit_create(0,1,1,BYTES,&h) == TERRA_CIRCUIT_INVALID && !h);
    assert(terra_circuit_create(65537,1,1,BYTES,&h) == TERRA_CIRCUIT_INVALID);
    assert(terra_circuit_create(1,1,1,1,&h) == TERRA_CIRCUIT_LIMIT);
    assert(terra_circuit_create(1,1,1048577,BYTES,&h) == TERRA_CIRCUIT_INVALID);
    txw_test_allocation_limit(0);
    assert(terra_circuit_create(2,2,4,BYTES,&h) == TERRA_CIRCUIT_OOM && !h);
    txw_test_allocation_limit(UINT32_MAX);
    for (uint32_t allocation_limit = 128; allocation_limit <= 1024; allocation_limit *= 2) {
        txw_test_allocation_limit(allocation_limit);
        assert(terra_circuit_create(128,128,1024,BYTES,&h) == TERRA_CIRCUIT_OOM && !h);
        assert(tx_native_heap_used() == baseline);
    }
    txw_test_allocation_limit(UINT32_MAX);
    h = create(8,8,8);
    TerraCircuitCell original = {2,2,15,1}; OK(terra_circuit_load(h,&original,1));
    TerraCircuitCell duplicate[] = {{3,3,1,0},{4,4,1,0},{3,3,1,0}};
    assert(terra_circuit_load(h,duplicate,3) == TERRA_CIRCUIT_DUPLICATE);
    assert(stats(h).cell_count == 1);
    OK(terra_circuit_load(h,duplicate,2));
    TerraCircuitCell bad[] = {{5,5,1,0},{8,0,1,0}};
    assert(terra_circuit_load(h,bad,2) == TERRA_CIRCUIT_BOUNDS && stats(h).cell_count == 3);
    assert(terra_circuit_patch(h,&original,1) == TERRA_CIRCUIT_STATE);
    uint32_t compiled;
    assert(terra_circuit_compile(h,1,&compiled) == TERRA_CIRCUIT_MORE && compiled == 1);
    assert(terra_circuit_load(h,&original,0) == TERRA_CIRCUIT_STATE);
    assert(terra_circuit_begin(h,NULL,0,0,0,1) == TERRA_CIRCUIT_STATE);
    compile(h,1); compile(h,1);
    TerraCircuitCell patch[] = {{2,2,0,0},{6,6,1,0}};
    assert(terra_circuit_patch(h,patch,2) == TERRA_CIRCUIT_BOUNDS);
    TerraCircuitPoint seed = {2,2};
    OK(terra_circuit_begin(h,&seed,1,0,0,100));
    assert(terra_circuit_compile(h,1,&compiled) == TERRA_CIRCUIT_STATE);
    assert(terra_circuit_begin(h,&seed,1,0,0,100) == TERRA_CIRCUIT_STATE);
    TerraCircuitEvent event; TerraCircuitStep step;
    assert(terra_circuit_step(h,0,&event,1,&step) == TERRA_CIRCUIT_INVALID && stats(h).active);
    assert(terra_circuit_step(h,1,&event,1,&step) == TERRA_CIRCUIT_MORE);
    assert(step.processed == 1 && step.emitted == 1 && event.flags == 3 && step.remaining == 1);
    tx_reset_heap(); assert(stats(h).cell_count == 3); /* persistent allocator isolation */
    OK(terra_circuit_step(h,1,&event,1,&step)); assert(!step.processed && !step.emitted);
    assert(terra_circuit_step(h,1,&event,1,&step) == TERRA_CIRCUIT_STATE);
    OK(terra_circuit_cancel(h)); OK(terra_circuit_close(h));
    assert(terra_circuit_cancel(h) == TERRA_CIRCUIT_HANDLE);
    assert(terra_circuit_close(h) == TERRA_CIRCUIT_HANDLE);
    assert(tx_native_heap_used() == baseline);
    h = create(65536,65536,2);
    TerraCircuitCell extremes[] = {{0,0,1,0},{65535,65535,1,5}};
    OK(terra_circuit_load(h,extremes,2)); compile(h,2);
    seed = (TerraCircuitPoint){65535,65535};
    txw_test_allocation_limit(128);
    assert(terra_circuit_begin(h,&seed,1,0,1,100) == TERRA_CIRCUIT_OOM && !stats(h).active);
    txw_test_allocation_limit(UINT32_MAX);
    OK(terra_circuit_begin(h,&seed,1,0,1,100));
    assert(terra_circuit_step(h,1,&event,1,&step) == TERRA_CIRCUIT_MORE);
    assert(event.x == 65535 && event.y == 65535 && event.flags == 3);
    OK(terra_circuit_step(h,1,&event,1,&step)); assert(!step.processed);
    OK(terra_circuit_close(h)); assert(tx_native_heap_used() == baseline);
}

static void fifo_and_interleaving_contract(void) {
    uint32_t h = create(6,6,5);
    TerraCircuitCell cells[] = {{2,2,3,0},{2,3,1,1},{2,1,1,0},{3,2,1,0},{1,2,2,0}};
    OK(terra_circuit_load(h,cells,5)); compile(h,5);
    TerraCircuitPoint seed[] = {{2,2},{2,2}};
    TerraCircuitEvent events[8]; TerraCircuitStep step;
    for (uint32_t pass = 0; pass < 100; ++pass) {
        OK(terra_circuit_begin(h,seed,2,0,TERRA_CIRCUIT_TRACE,100));
        assert(terra_circuit_step(h,100,events,8,&step) == TERRA_CIRCUIT_MORE);
        assert(step.processed == 2 && step.emitted == 2); /* pauses at first tile */
        assert(events[0].x == 2 && events[0].y == 2 && events[0].flags == 2);
        assert(events[1].x == 2 && events[1].y == 3 && events[1].direction == 0 && events[1].flags == 1);
        OK(terra_circuit_step(h,100,events,8,&step));
        assert(step.processed == 2 && step.total_processed == 4 && step.emitted == 2);
        assert(events[0].x == 2 && events[0].y == 1 && events[0].direction == 1);
        assert(events[1].x == 3 && events[1].y == 2 && events[1].direction == 2);
    }
    OK(terra_circuit_begin(h,seed,2,1,TERRA_CIRCUIT_TRACE,100));
    OK(terra_circuit_step(h,100,events,8,&step));
    assert(step.processed == 2 && events[1].x == 1 && events[1].direction == 3);
    OK(terra_circuit_close(h));

    h = create(6,1,6);
    TerraCircuitCell row[6]; for (uint32_t x=0;x<6;++x) row[x]=(TerraCircuitCell){x,0,1,x==2 ? 1u : 0u};
    OK(terra_circuit_load(h,row,6)); compile(h,6); seed[0]=(TerraCircuitPoint){0,0};
    OK(terra_circuit_begin(h,seed,1,0,0,100));
    assert(terra_circuit_step(h,100,events,8,&step) == TERRA_CIRCUIT_MORE);
    assert(step.processed == 3 && step.emitted == 1 && events[0].x == 2);
    TerraCircuitCell removed = {3,0,0,0}; OK(terra_circuit_patch(h,&removed,1));
    OK(terra_circuit_step(h,100,events,8,&step)); assert(step.processed == 0);
    /* The wire after the emitted tile was not expanded before the host effect. */
    removed.wires=1; removed.routing=1; OK(terra_circuit_patch(h,&removed,1));
    OK(terra_circuit_begin(h,seed,1,0,0,100));
    assert(terra_circuit_step(h,100,events,8,&step) == TERRA_CIRCUIT_MORE);
    assert(events[0].x==2);
    assert(terra_circuit_step(h,100,events,8,&step) == TERRA_CIRCUIT_MORE);
    assert(events[0].x==3 && step.processed==1);
    OK(terra_circuit_cancel(h));
    OK(terra_circuit_begin(h,seed,1,0,0,2));
    assert(terra_circuit_step(h,100,events,8,&step) == TERRA_CIRCUIT_LIMIT && !stats(h).active);
    OK(terra_circuit_close(h));
}

/* Independent dense coordinate reference, matching the source's counters and
 * append-only queue. Random routers deliberately include nonterminating loops. */
#define RW 9u
#define RH 8u
#define RL 512u
static int32_t reference(const TerraCircuitCell* grid, const TerraCircuitPoint* seeds,
    uint32_t seed_count, uint32_t colour, TerraCircuitEvent* out, uint32_t* out_count) {
    static const int dx[4]={0,0,1,-1},dy[4]={1,-1,0,0};
    static const uint32_t routes[3][4]={{0,1,2,3},{3,2,1,0},{2,3,0,1}};
    uint32_t queue[RL*4u+RW*RH], n=0, cursor=0;
    uint8_t counters[RW*RH]={0}, source[RW*RH]={0};
    for(uint32_t i=0;i<seed_count;++i){uint32_t k=seeds[i].y*RW+seeds[i].x;
        if(!(grid[k].wires&(1u<<colour))||source[k])continue;
        source[k]=1;counters[k]=4;queue[n++]=k*4u;}
    while(cursor<n){
        if(cursor==RL){*out_count=cursor;return TERRA_CIRCUIT_LIMIT;}
        uint32_t word=queue[cursor],k=word/4u,incoming=word%4u,x=k%RW,y=k/RW,r=grid[k].routing;
        uint32_t flags=(r?1u:0u)|(source[k]?2u:0u);
        for(uint32_t d=0;d<4;++d){
            int nx=(int)x+dx[d],ny=(int)y+dy[d];
            if(nx<0||ny<0||nx>=(int)RW||ny>=(int)RH)continue;
            if(r>=2&&r<=4&&routes[r-2][incoming]!=d)continue;
            if(r==5){if(d!=incoming)continue;flags|=d<2?8u:4u;}
            uint32_t next=(uint32_t)ny*RW+(uint32_t)nx;
            if(!(grid[next].wires&(1u<<colour)))continue;
            if(counters[next]){--counters[next];continue;}
            assert(n<sizeof(queue)/sizeof(queue[0]));queue[n++]=next*4u+d;
            if(grid[next].routing<2)counters[next]=3;
        }
        out[cursor++]=(TerraCircuitEvent){x,y,incoming,flags};
    }
    *out_count=cursor;return TERRA_CIRCUIT_OK;
}
static uint32_t random_state=0x8317b52au;
static uint32_t random_u32(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void randomized_source_contract(void) {
    for(uint32_t trial=0;trial<350;++trial){
        TerraCircuitCell grid[RW*RH],cells[RW*RH];uint32_t count=0;
        for(uint32_t y=0;y<RH;++y)for(uint32_t x=0;x<RW;++x){uint32_t k=y*RW+x;
            grid[k]=(TerraCircuitCell){x,y,random_u32()%16u,random_u32()%6u};
            if(grid[k].wires)cells[count++]=grid[k];}
        uint32_t h=create(RW,RH,RW*RH);OK(terra_circuit_load(h,cells,count));compile(h,7);
        TerraCircuitPoint seeds[4]={{1,1},{3,2},{1,1},{7,6}};
        for(uint32_t colour=0;colour<4;++colour){
            TerraCircuitEvent expected[RL],event;uint32_t expected_count=0,actual_count=0;
            int32_t expected_status=reference(grid,seeds,4,colour,expected,&expected_count),status;
            OK(terra_circuit_begin(h,seeds,4,colour,TERRA_CIRCUIT_TRACE,RL));
            do{TerraCircuitStep step;status=terra_circuit_step(h,1,&event,1,&step);
                if(status<0){assert(status==expected_status);break;}
                assert(step.processed==step.emitted);
                if(step.emitted){assert(actual_count<expected_count);assert(!memcmp(&event,&expected[actual_count],sizeof(event)));++actual_count;}
            }while(status==TERRA_CIRCUIT_MORE);
            assert(status==expected_status&&actual_count==expected_count);
        }
        OK(terra_circuit_close(h));
    }
}
static void million_cell_contract(void) {
    const uint32_t side=1000,n=side*side;
    uint32_t baseline=tx_native_heap_used(),h=create(side,side,n);
    TerraCircuitCell* batch=(TerraCircuitCell*)malloc(TERRA_CIRCUIT_BATCH*sizeof(*batch));assert(batch);
    clock_t start=clock();
    for(uint32_t first=0;first<n;first+=TERRA_CIRCUIT_BATCH){uint32_t count=n-first;if(count>TERRA_CIRCUIT_BATCH)count=TERRA_CIRCUIT_BATCH;
        for(uint32_t i=0;i<count;++i){uint32_t k=first+i;batch[i]=(TerraCircuitCell){k%side,k/side,15,k==n-1u?1u:0u};}
        OK(terra_circuit_load(h,batch,count));}
    compile(h,TERRA_CIRCUIT_BATCH);free(batch);
    double build_ms=(double)(clock()-start)*1000.0/CLOCKS_PER_SEC;
    start=clock();TerraCircuitPoint seed={0,0};
    for(uint32_t pass=0;pass<4;++pass){
        OK(terra_circuit_begin(h,&seed,1,pass,0,n+1u));uint32_t processed=0,emitted=0;int32_t status;
        do{TerraCircuitEvent event;TerraCircuitStep step;status=terra_circuit_step(h,TERRA_CIRCUIT_BATCH,&event,1,&step);
            assert(status>=0);processed+=step.processed;emitted+=step.emitted;
            if(step.emitted)assert(event.x==side-1u&&event.y==side-1u&&event.flags==1u);
        }while(status==TERRA_CIRCUIT_MORE);
        assert(processed==n&&emitted==1u);
    }
    double traversal_ms=(double)(clock()-start)*1000.0/CLOCKS_PER_SEC;
    TerraCircuitStats s=stats(h);assert(s.active_bytes<48u*1024u*1024u&&s.peak_bytes<=s.max_bytes);
    printf("circuit native million cells: build=%.2fms four-colour visits=4000000 traversal=%.2fms active=%u peak=%u\n",build_ms,traversal_ms,s.active_bytes,s.peak_bytes);
    OK(terra_circuit_close(h));assert(tx_native_heap_used()==baseline);
}
int main(void) {
    assert(terra_circuit_abi_version()==1);
    lifecycle_contract();fifo_and_interleaving_contract();randomized_source_contract();million_cell_contract();
    assert(tx_native_heap_used()==0);
    puts("circuit contracts: FIFO, colour isolation, interleaving, rollback validation, 1400 randomized source comparisons, million cells, no leaks: OK");
    return 0;
}
