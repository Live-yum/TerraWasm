/* Standalone synthetic differential contract. No source worlds or external IO.
 * Build separately, WITHOUT linking terra_circuit_world.c or replay.c:
 * cc -std=c11 -Wall -Wextra -Iinclude -Isrc tests/circuit_order_replay_contract.c -o order-replay-contract
 * This file includes replay.c to inspect per-cell commit atomicity. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/terra_circuit_order_replay.c"

typedef union TestAllocation { max_align_t align; struct {uint32_t size;} v; } TestAllocation;
static uint32_t live_allocations;
void* cx_alloc(CxWorld* w,uint32_t n){
    if((uint64_t)w->bytes+n+sizeof(TestAllocation)>w->maximum)return NULL;
    TestAllocation* p=malloc(sizeof(*p)+n);if(!p)return NULL;p->v.size=n+sizeof(*p);
    w->bytes+=p->v.size;++live_allocations;return p+1;
}
void cx_free(CxWorld* w,void* ptr){if(ptr){TestAllocation* p=(TestAllocation*)ptr-1;w->bytes-=p->v.size;--live_allocations;free(p);}}
uint8_t cx_bytes_get(const CxBytes* b,uint32_t at){assert(at<b->length);assert((at>>CX_PAGE_SHIFT)<b->pages_count);return b->pages[at>>CX_PAGE_SHIFT][at&(CX_PAGE_SIZE-1u)];}
int cx_inside_wiring(const CxWorld* w,uint32_t x,uint32_t y){return w->width>4&&w->height>4&&x>=2&&y>=2&&x<w->width-2&&y<w->height-2;}

typedef struct Fixture {
    CxWorld w;
    uint8_t *words,*routes,*map_bytes,*cp_bytes;
    uint32_t *expected,*parents,*right,*births,*prefix,*final;
    uint32_t label_count,capacity,map_length,cp_length;
} Fixture;
static uint32_t root_of(Fixture* f,uint32_t a){while(f->parents[a]!=a)a=f->parents[a];return a;}
static uint32_t label(Fixture* f){uint32_t n=++f->label_count;assert(n<f->capacity);f->parents[n]=n;return n;}
static uint32_t unite(Fixture* f,uint32_t a,uint32_t b){
    if(!a&&!b)return label(f);if(!a)return b;if(!b)return a;
    a=root_of(f,a);b=root_of(f,b);if(a>b){uint32_t t=a;a=b;b=t;}f->parents[b]=a;return a;
}
static void put(uint8_t* bytes,uint32_t* length,uint32_t v){do{uint8_t b=v&127u;v>>=7;if(v)b|=128u;bytes[(*length)++]=b;}while(v);}
static void pages(CxBytes* b,uint8_t* data,uint32_t length){
    b->length=length;b->pages_count=b->pages_capacity=(length+CX_PAGE_SIZE-1u)/CX_PAGE_SIZE;
    b->pages=calloc(b->pages_count?b->pages_count:1u,sizeof(*b->pages));assert(b->pages);
    for(uint32_t i=0;i<b->pages_count;i++)b->pages[i]=data+i*CX_PAGE_SIZE;
}
static uint32_t cell(Fixture* f,uint32_t x,uint32_t y,uint32_t c){return (x*f->w.height+y)*4u+c;}
static void compile_fixture(Fixture* f){
    uint32_t width=f->w.width,height=f->w.height;
    /* Separate full-column arrays emulate the compiler's original topology
     * pass. A union-find resolves merges only AFTER the whole world is scanned. */
    uint32_t* front=calloc(height*4u,4);uint32_t* output=calloc(height*4u,4);assert(front&&output);
    for(uint32_t x=0;x<width;x++){
        f->births[x]=f->label_count;memset(output,0,height*16u);
        for(uint32_t c=0;c<4;c++){uint32_t up=0;
            for(uint32_t y=0;y<height;y++){
                uint32_t k=x*height+y,left=front[y*4u+c],right=0,down=0;
                if((f->words[k]&(1u<<c))&&cx_inside_wiring(&f->w,x,y)){
                    switch(f->routes[k]){
                    case 1:right=left?left:label(f);down=up?up:label(f);break;
                    case 2:(void)unite(f,left,up);right=down=label(f);break;
                    case 3:right=up?up:label(f);down=left?left:label(f);break;
                    default:right=down=unite(f,left,up);break;
                    }
                }
                up=down;output[y*4u+c]=right;f->right[cell(f,x,y,c)]=right;
            }
        }
        memcpy(front,output,height*16u);
    }
    uint32_t nets=0;
    for(uint32_t i=1;i<=f->label_count;i++)if(root_of(f,i)==i)f->final[i]=++nets;
    for(uint32_t i=1;i<=f->label_count;i++)f->final[i]=f->final[root_of(f,i)];
    for(uint32_t i=0;i<width*height*4u;i++)f->expected[i]=f->final[f->right[i]];
    uint32_t prev=0;
    for(uint32_t i=1;i<=f->label_count;i++){
        int64_t d=(int64_t)f->final[i]-prev;uint32_t zz=d<0?(uint32_t)(-d*2-1):(uint32_t)(d*2);
        put(f->map_bytes,&f->map_length,zz);f->prefix[i]=f->map_length;prev=f->final[i];
    }
    f->w.networks=nets;f->w.labels_count=f->label_count;
    f->w.checkpoint_count=(width+63u)/64u;f->w.checkpoint_index=calloc(f->w.checkpoint_count,sizeof(CxCheckpoint));assert(f->w.checkpoint_index);
    for(uint32_t i=0;i<f->w.checkpoint_count;i++){
        uint32_t x=i*64u,birth=f->births[x];CxCheckpoint* cp=f->w.checkpoint_index+i;
        cp->column=x;cp->offset=f->cp_length;cp->map_offset=f->prefix[birth];cp->map_previous=f->final[birth];cp->label_count=birth;
        for(uint32_t c=0;c<4;c++)for(uint32_t y=0;y<height;){
            uint32_t value=x?f->expected[cell(f,x-1u,y,c)]:0,run=1;
            while(y+run<height&&(x?f->expected[cell(f,x-1u,y+run,c)]:0)==value)++run;
            put(f->cp_bytes,&f->cp_length,run);put(f->cp_bytes,&f->cp_length,value);y+=run;
        }
        cp->length=f->cp_length-cp->offset;
    }
    pages(&f->w.map,f->map_bytes,f->map_length);pages(&f->w.checkpoints,f->cp_bytes,f->cp_length);
    free(front);free(output);
}
static void initialize(Fixture* f,uint32_t width,uint32_t height,uint32_t seed,int special){
    memset(f,0,sizeof(*f));f->w.width=width;f->w.height=height;f->w.phase=CX_RUN;f->w.maximum=UINT32_MAX;
    uint32_t cells=width*height;f->capacity=cells*8u+1u;
    f->words=calloc(cells,1);f->routes=calloc(cells,1);
    f->right=calloc(cells*4u,4);f->expected=calloc(cells*4u,4);
    f->parents=calloc(f->capacity,4);f->final=calloc(f->capacity,4);f->prefix=calloc(f->capacity,4);f->births=calloc(width,4);
    f->map_bytes=calloc(f->capacity,5);f->cp_bytes=calloc(cells+1u,40);
    assert(f->words&&f->routes&&f->right&&f->expected&&f->parents&&f->final&&f->prefix&&f->births&&f->map_bytes&&f->cp_bytes);
    if(special){f->words[2u*height+2u]=1;f->routes[2u*height+2u]=2;}
    else if(seed)for(uint32_t i=0;i<cells;i++){
        seed=seed*1664525u+1013904223u;f->words[i]=(uint8_t)((seed>>24)&15u);
        /* Route includes raw-0xfffe semantics via separate route=1 even when
         * the BFS word deliberately has zero routing bits. */
        f->routes[i]=(uint8_t)((seed>>16)&3u);if(seed&0x100u)f->words[i]|=64u;
    }
    compile_fixture(f);
    /* Nonzero unrelated owner state catches accidental global query writes. */
    f->w.x=12345;f->w.y=6789;f->w.map_cursor=77;f->w.map_previous=88;f->w.label_cursor=99;f->w.cursor=123;
    f->w.query_x0=8;f->w.query_x1=9;f->w.col_stage=3;f->w.up=42;
}
static void dispose(Fixture* f){
    assert(!f->w.bytes);free(f->w.map.pages);free(f->w.checkpoints.pages);free(f->w.checkpoint_index);
    free(f->words);free(f->routes);free(f->right);free(f->expected);free(f->parents);free(f->final);free(f->prefix);free(f->births);free(f->map_bytes);free(f->cp_bytes);
}
typedef struct Geometry {Fixture* fixture;uint32_t stage,x,y,calls,yields;int yielding,failure;} Geometry;
static int geometry(void* context,uint32_t x,uint32_t y,uint8_t* word,uint8_t* route){
    Geometry* g=context;++g->calls;if(g->failure)return g->failure;
    if(g->yielding){
        if(!g->stage){g->x=x;g->y=y;g->stage=1;++g->yields;return 1;}
        assert(x==g->x&&y==g->y);
        if(g->stage<3){++g->stage;++g->yields;return 2;}g->stage=0;
    }
    uint32_t i=x*g->fixture->w.height+y;*word=g->fixture->words[i];*route=g->fixture->routes[i];return 0;
}
static void differential(uint32_t seed,uint32_t width,uint32_t height,int yielding){
    Fixture f;initialize(&f,width,height,seed,0);CxOrderReplay* r=NULL;assert(cx_order_replay_create(&f.w,&r)==0);
    assert(f.w.bytes<height*16u+sizeof(*r)+2u*sizeof(TestAllocation)+1u);
    const uint32_t xs[]={0,2,63,64,65,127,129};
    for(uint32_t ix=0;ix<sizeof(xs)/sizeof(xs[0]);ix++)if(xs[ix]<width)for(uint32_t y=0;y<height;y+=3u){
        uint32_t out[4]={91,92,93,94},untouched[4];memcpy(untouched,out,sizeof(out));
        assert(cx_order_replay_begin(r,xs[ix],y)==0);Geometry g={0};g.fixture=&f;g.yielding=yielding;CxWorld guard=f.w;
        uint32_t iterations=0;int s;
        do{
            s=cx_order_replay_step(&f.w,r,1u+iterations%7u,geometry,&g,out);
            assert(!memcmp(&f.w,&guard,sizeof(guard)));assert(s>=0&&s<=2);
            if(s)assert(!memcmp(out,untouched,sizeof(out)));assert(++iterations<2000000u);
        }while(s);
        for(uint32_t c=0;c<4;c++)assert(out[c]==f.expected[cell(&f,xs[ix],y,c)]);
        uint32_t calls=g.calls;assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==0&&g.calls==calls);
    }
    /* Cancel a partially restored checkpoint, then restart at a distant one. */
    assert(cx_order_replay_begin(r,width-1u,height-1u)==0);Geometry g={0};g.fixture=&f;uint32_t out[4];
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==1);cx_order_replay_cancel(r);
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_STATE);assert(cx_order_replay_begin(r,2,2)==0);
    int s;do{s=cx_order_replay_step(&f.w,r,13,geometry,&g,out);}while(s>0);assert(!s);
    for(uint32_t c=0;c<4;c++)assert(out[c]==f.expected[cell(&f,2,2,c)]);
    cx_order_replay_destroy(&f.w,r);dispose(&f);
}
static void malformed(void){
    Fixture f;initialize(&f,8,8,0,1);CxOrderReplay* r;assert(cx_order_replay_create(&f.w,&r)==0);
    CxCheckpoint saved=f.w.checkpoint_index[0];Geometry g={0};g.fixture=&f;uint32_t out[4]={7,7,7,7};
    f.w.checkpoint_index[0].offset=UINT32_MAX;assert(cx_order_replay_begin(r,2,2)==TCW_FORMAT);f.w.checkpoint_index[0]=saved;
    f.w.checkpoint_index[0].length++;assert(cx_order_replay_begin(r,2,2)==TCW_FORMAT);f.w.checkpoint_index[0]=saved;
    f.w.checkpoint_index[0].column=1;assert(cx_order_replay_begin(r,2,2)==TCW_FORMAT);f.w.checkpoint_index[0]=saved;
    f.w.checkpoint_index[0].map_offset=f.w.map.length+1;assert(cx_order_replay_begin(r,2,2)==TCW_FORMAT);f.w.checkpoint_index[0]=saved;
    uint8_t original=f.cp_bytes[0];f.cp_bytes[0]=0;assert(cx_order_replay_begin(r,2,2)==0);
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_FORMAT);f.cp_bytes[0]=original;
    f.cp_bytes[0]=9;assert(cx_order_replay_begin(r,2,2)==0);assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_FORMAT);f.cp_bytes[0]=original;
    f.w.checkpoint_index[0].length=1;assert(cx_order_replay_begin(r,2,2)==0);assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_FORMAT);f.w.checkpoint_index[0]=saved;
    /* First new label succeeds, second is truncated. Neither is committed. */
    assert(cx_order_replay_begin(r,2,2)==0);
    while(!(r->state==2&&r->x==2&&r->colour==0&&r->row==2))assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==1);
    uint32_t old_length=f.w.map.length;f.w.map.length=1;CxOrderReplay before=*r;uint32_t frontier[32];memcpy(frontier,r->front,sizeof(frontier));CxWorld guard=f.w;
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_FORMAT);
    assert(!memcmp(&guard,&f.w,sizeof(guard)));assert(!memcmp(frontier,r->front,sizeof(frontier)));
    assert(r->map_at==before.map_at&&r->map_previous==before.map_previous&&r->label_cursor==before.label_cursor&&r->up==before.up&&r->x==before.x&&r->row==before.row&&r->colour==before.colour);
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_FORMAT);f.w.map.length=old_length;
    assert(cx_order_replay_begin(r,2,2)==0);g.failure=TCW_IO;
    while(r->state==1)assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==1);
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_IO);g.failure=0;
    /* Truncated, overflowing and out-of-range map values. */
    const uint8_t bad[][5]={{0x80,0x80,0x80,0x80,0x10},{0x80,0x80,0x80,0x80,0x80},{0,0,0,0,0},{6,0,0,0,0}};
    uint8_t old[5];memcpy(old,f.map_bytes,5);
    for(uint32_t i=0;i<4;i++){
        memcpy(f.map_bytes,bad[i],5);f.w.map.length=5;assert(cx_order_replay_begin(r,2,2)==0);
        int s;do{s=cx_order_replay_step(&f.w,r,11,geometry,&g,out);}while(s>0);assert(s==TCW_FORMAT);
    }
    memcpy(f.map_bytes,old,5);f.w.map.length=old_length;
    assert(cx_order_replay_begin(r,8,2)==TCW_INVALID);assert(cx_order_replay_begin(r,2,8)==TCW_INVALID);
    cx_order_replay_cancel(r);assert(cx_order_replay_step(&f.w,r,0,geometry,&g,out)==TCW_INVALID);
    f.w.phase=CX_CANCELLED;assert(cx_order_replay_begin(r,2,2)==TCW_STATE);assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==TCW_STATE);f.w.phase=CX_RUN;
    cx_order_replay_destroy(&f.w,r);dispose(&f);
}
static void callback_atomicity(void){
    Fixture f;initialize(&f,8,8,41,0);CxOrderReplay* r;assert(cx_order_replay_create(&f.w,&r)==0);
    assert(cx_order_replay_begin(r,3,3)==0);Geometry g={0};g.fixture=&f;uint32_t out[4]={5,5,5,5};
    while(r->state==1)assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==1);
    g.yielding=1;CxOrderReplay before=*r;uint32_t frontier[32];memcpy(frontier,r->front,sizeof(frontier));
    assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==1);
    assert(!memcmp(r,&before,sizeof(before))&&!memcmp(frontier,r->front,sizeof(frontier)));
    for(uint32_t i=0;i<2;i++){
        assert(cx_order_replay_step(&f.w,r,1,geometry,&g,out)==2);
        assert(!memcmp(r,&before,sizeof(before))&&!memcmp(frontier,r->front,sizeof(frontier)));
    }
    /* The caller cancels its own pending geometry request as well. */
    cx_order_replay_cancel(r);g.stage=0;g.yielding=0;
    assert(cx_order_replay_begin(r,3,3)==0);int status;
    do{status=cx_order_replay_step(&f.w,r,17,geometry,&g,out);}while(status>0);assert(!status);
    for(uint32_t c=0;c<4;c++)assert(out[c]==f.expected[cell(&f,3,3,c)]);
    cx_order_replay_destroy(&f.w,r);dispose(&f);
}
static void paged_varints(void){
    uint8_t* data=calloc(CX_PAGE_SIZE+8u,1);assert(data);CxBytes b={0};
    uint32_t at=CX_PAGE_SIZE-2u,end=at;put(data,&end,UINT32_MAX);pages(&b,data,end);
    uint32_t value=0,cursor=at;assert(read_var(&b,b.length,&cursor,&value)==0&&value==UINT32_MAX&&cursor==end);
    cursor=at;assert(read_var(&b,end-1u,&cursor,&value)==TCW_FORMAT&&cursor==at);
    data[end-1u]=0x10;cursor=at;assert(read_var(&b,end,&cursor,&value)==TCW_FORMAT&&cursor==at);
    free(b.pages);free(data);
}
static void allocation_failure(void){
    Fixture f;initialize(&f,8,8,0,0);CxOrderReplay* r=(CxOrderReplay*)1;f.w.maximum=0;
    assert(cx_order_replay_create(&f.w,&r)==TCW_MEMORY&&!r&&!f.w.bytes);
    f.w.maximum=sizeof(CxOrderReplay)+sizeof(TestAllocation);
    assert(cx_order_replay_create(&f.w,&r)==TCW_MEMORY&&!r&&!f.w.bytes);dispose(&f);
}
typedef struct BatchCandidate {uint32_t identity,x,y,group,selected;} BatchCandidate;
typedef struct BatchProbe {
    Fixture* fixture;
    BatchCandidate candidate[17];
    uint32_t count,entry,x,y,colour,unique,updates;
    int active,failure;
} BatchProbe;
static int batch_visitor(void* context,uint32_t x,uint32_t y,uint32_t colour,uint32_t net,uint8_t word){
    BatchProbe* p=context;Fixture* f=p->fixture;
    if(p->failure)return p->failure;
    assert(x==p->x&&y==p->y&&colour==p->colour);
    assert(net==f->expected[cell(f,x,y,colour)]&&word==f->words[x*f->w.height+y]);
    if(!p->active){p->active=1;p->entry=0;++p->unique;}
    if(p->entry<p->count){
        BatchCandidate* c=p->candidate+p->entry++;
        if(c->x==x&&c->y==y&&(word&64u)&&(word&15u)&&c->group==net&&c->selected==UINT32_MAX){c->selected=colour;++p->updates;}
        /* Last chain entry also yields: completion must wait for one more call. */
        return 1;
    }
    p->active=0;
    if(++p->y==f->w.height){p->y=0;if(++p->colour==4){p->colour=0;++p->x;}}
    return 0;
}
static void prepare_probe(BatchProbe* p,Fixture* f,uint32_t x,uint32_t y,uint32_t count){
    memset(p,0,sizeof(*p));p->fixture=f;p->count=count;p->x=x/64u*64u;
    for(uint32_t i=0;i<count;i++){
        uint32_t cx=p->x+i%(x-p->x+1u),cy=cx==x?i%(y+1u):i%f->w.height;
        p->candidate[i]=(BatchCandidate){1000u+i,cx,cy,f->expected[cell(f,cx,cy,i%4u)],UINT32_MAX};
    }
    /* Deliberate same-coordinate duplicates; input order/identity never moves. */
    if(count>2){p->candidate[count-1]=p->candidate[1];p->candidate[count-1].identity=1000u+count-1;}
}
static uint32_t batch_drive(Fixture* f,CxOrderReplay* r,uint32_t x,uint32_t y,uint32_t count,int yielding){
    Geometry g={0};g.fixture=f;g.yielding=yielding;BatchProbe p;prepare_probe(&p,f,x,y,count);
    assert(cx_order_replay_begin_batch(r,x,y)==0);CxWorld guard=f->w;uint32_t steps=0,out[4];
    assert(cx_order_replay_step(&f->w,r,1,geometry,&g,out)==TCW_STATE);
    int status;
    do{
        uint32_t before_state=r->state,before_at=r->map_at,before_label=r->label_cursor,before_calls=g.calls;
        status=cx_order_replay_step_batch(&f->w,r,1,geometry,&g,batch_visitor,&p);
        assert(status>=0&&status<=2);assert(!memcmp(&f->w,&guard,sizeof(guard)));
        if(before_state==5){assert(g.calls==before_calls&&r->map_at==before_at&&r->label_cursor==before_label);}
        if(r->state==5&&r->x==x&&r->row==y&&r->colour==3u)assert(status==1);
        assert(++steps<5000000u);
    }while(status);
    uint32_t unique=(x-x/64u*64u)*4u*f->w.height+3u*f->w.height+y+1u;
    assert(p.unique==unique&&!p.active);uint32_t geometry_calls=g.calls;
    assert(cx_order_replay_step_batch(&f->w,r,1,geometry,&g,batch_visitor,&p)==0&&g.calls==geometry_calls&&p.unique==unique);
    if(!yielding)assert(geometry_calls==unique);
    for(uint32_t i=0;i<count;i++){
        BatchCandidate* c=p.candidate+i;uint32_t expected=UINT32_MAX,word=f->words[c->x*f->w.height+c->y];
        if((word&64u)&&(word&15u))for(uint32_t colour=0;colour<4;colour++)if(f->expected[cell(f,c->x,c->y,colour)]==c->group){expected=colour;break;}
        assert(c->identity==1000u+i&&c->selected==expected);
        /* The unmodified single-point API must agree with the batch stream. */
        assert(cx_order_replay_begin(r,c->x,c->y)==0);g.yielding=0;g.stage=0;
        assert(cx_order_replay_step_batch(&f->w,r,1,geometry,&g,batch_visitor,&p)==TCW_STATE);
        do{status=cx_order_replay_step(&f->w,r,19,geometry,&g,out);}while(status>0);assert(!status);
        for(uint32_t colour=0;colour<4;colour++)assert(out[colour]==f->expected[cell(f,c->x,c->y,colour)]);
    }
    return geometry_calls;
}
/* Collapse final IDs across colours to exercise duplicate-group semantics.
 * The replay input is synthetic but preserves all label births and frontiers. */
static void collapse_networks(Fixture* f){
    assert(f->w.networks);f->w.networks=1;f->map_length=f->cp_length=0;
    for(uint32_t i=1;i<=f->label_count;i++){f->final[i]=1;put(f->map_bytes,&f->map_length,i==1?2u:0u);f->prefix[i]=f->map_length;}
    for(uint32_t i=0;i<f->w.width*f->w.height*4u;i++)if(f->expected[i])f->expected[i]=1;
    for(uint32_t i=0;i<f->w.width*f->w.height;i++)if(f->words[i]&15u)f->words[i]|=64u;
    for(uint32_t i=0;i<f->w.checkpoint_count;i++){
        CxCheckpoint* cp=f->w.checkpoint_index+i;uint32_t x=cp->column;
        cp->offset=f->cp_length;cp->map_offset=f->prefix[cp->label_count];cp->map_previous=cp->label_count?1u:0u;
        for(uint32_t c=0;c<4;c++)for(uint32_t y=0;y<f->w.height;){
            uint32_t value=x?f->expected[cell(f,x-1u,y,c)]:0,run=1;
            while(y+run<f->w.height&&(x?f->expected[cell(f,x-1u,y+run,c)]:0)==value)++run;
            put(f->cp_bytes,&f->cp_length,run);put(f->cp_bytes,&f->cp_length,value);y+=run;
        }
        cp->length=f->cp_length-cp->offset;
    }
    free(f->w.map.pages);free(f->w.checkpoints.pages);
    pages(&f->w.map,f->map_bytes,f->map_length);pages(&f->w.checkpoints,f->cp_bytes,f->cp_length);
}
static void duplicate_colour_batch(void){
    Fixture f;initialize(&f,70,19,317,0);collapse_networks(&f);
    uint32_t x=0,y=0,first=UINT32_MAX,second=UINT32_MAX;
    for(uint32_t xx=64;xx<68&&second==UINT32_MAX;xx++)for(uint32_t yy=2;yy<17&&second==UINT32_MAX;yy++){
        first=UINT32_MAX;for(uint32_t c=0;c<4;c++)if(f.expected[cell(&f,xx,yy,c)]){if(first==UINT32_MAX)first=c;else{second=c;x=xx;y=yy;break;}}
    }
    assert(second!=UINT32_MAX&&first<second);CxOrderReplay* r;assert(cx_order_replay_create(&f.w,&r)==0);
    BatchProbe p;prepare_probe(&p,&f,x,y,1);p.candidate[0]=(BatchCandidate){1000,x,y,1,UINT32_MAX};
    Geometry g={0};g.fixture=&f;g.yielding=1;assert(cx_order_replay_begin_batch(r,x,y)==0);int status;
    do{status=cx_order_replay_step_batch(&f.w,r,3,geometry,&g,batch_visitor,&p);}while(status>0);assert(!status);
    assert(p.candidate[0].selected==first&&p.updates==1);
    /* If only the later colour is enabled, retain the first selection: caller
     * must leave it unranked, not retarget the candidate to the later colour. */
    uint32_t mask=1u<<second;assert(!(mask&(1u<<p.candidate[0].selected)));
    cx_order_replay_destroy(&f.w,r);dispose(&f);
}
static void batch_contract(void){
    Fixture f;initialize(&f,130,19,121,0);CxOrderReplay* r;assert(cx_order_replay_create(&f.w,&r)==0);
    const uint32_t ends[]={2,63,64,65,127,129};
    for(uint32_t i=0;i<sizeof(ends)/sizeof(ends[0]);i++){
        uint32_t x=ends[i],y=i%2u?0u:18u;
        uint32_t one=batch_drive(&f,r,x,y,1,0),many=batch_drive(&f,r,x,y,17,0);
        assert(one==many&&many<=64u*4u*f.w.height);
        (void)batch_drive(&f,r,x,y,5,1);
    }
    Geometry g={0};g.fixture=&f;BatchProbe p;prepare_probe(&p,&f,3,3,2);
    assert(cx_order_replay_begin_batch(r,3,3)==0);
    while(r->state!=5)assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==1);
    assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==1);
    uint32_t before_calls=g.calls;CxOrderReplay before=*r;CxWorld guard=f.w;
    p.failure=2;assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==TCW_STATE);
    assert(r->map_at==before.map_at&&r->label_cursor==before.label_cursor&&g.calls==before_calls&&!memcmp(&f.w,&guard,sizeof(guard)));
    assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==TCW_STATE);
    cx_order_replay_cancel(r);assert(!r->pending_net&&!r->pending_word&&!r->batch);
    /* Restart replaces caller's visitor state; no previous chain survives. */
    (void)batch_drive(&f,r,65,7,5,1);
    prepare_probe(&p,&f,3,3,2);assert(cx_order_replay_begin_batch(r,3,3)==0);
    while(r->state!=5)assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==1);
    p.failure=TCW_IO;assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==TCW_IO);
    /* Cancel while a committed visitor is pending, without a preceding error. */
    prepare_probe(&p,&f,3,3,2);assert(cx_order_replay_begin_batch(r,3,3)==0);
    while(r->state!=5)assert(cx_order_replay_step_batch(&f.w,r,1,geometry,&g,batch_visitor,&p)==1);
    cx_order_replay_cancel(r);assert(r->state==0);(void)batch_drive(&f,r,3,3,3,0);
    cx_order_replay_destroy(&f.w,r);dispose(&f);
}
int main(void){
    for(uint32_t seed=1;seed<=12;seed++)differential(seed,130,19,seed%2u);
    differential(0,130,19,1);differential(7,4,4,1);malformed();callback_atomicity();paged_varints();allocation_failure();batch_contract();duplicate_colour_batch();
    assert(!live_allocations);puts("private checkpoint replay: topology differential, yields, cancel, bounded storage, immutable owner and atomic failures passed");return 0;
}
