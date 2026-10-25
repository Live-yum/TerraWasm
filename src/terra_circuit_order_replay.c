/* Bounded final-network reconstruction from immutable compiler checkpoints. */
#include "terra_circuit_order_replay.h"
#include <string.h>

struct CxOrderReplay {
    CxWorld* owner;
    uint32_t* front;
    uint32_t target_x,target_y,x,colour,row,up;
    uint32_t cp_at,cp_end,cp_run,cp_value;
    uint32_t map_at,map_previous,label_cursor;
    uint32_t nets[4];
    uint32_t state; /* 0 cancelled, 1 checkpoint, 2 cells, 3 complete, 4 failed, 5 visitor */
    uint32_t batch,pending_net;
    uint8_t pending_word;
    int failure;
};
typedef struct ReplayMap { uint32_t at,previous,label; } ReplayMap;

/* Unlike cx_var_read, this decoder checks the complete bounded varint before
 * publishing the local cursor. Fifth-byte overflow and truncation are errors. */
static int read_var(const CxBytes* bytes,uint32_t end,uint32_t* cursor,uint32_t* value) {
    uint32_t at=*cursor,v=0;
    if(end>bytes->length||at>end)return TCW_FORMAT;
    for(uint32_t i=0;i<5u;i++) {
        if(at==end)return TCW_FORMAT;
        uint8_t b=cx_bytes_get(bytes,at++);
        if(i==4u&&(b&0xf0u))return TCW_FORMAT;
        v|=(uint32_t)(b&127u)<<(i*7u);
        if(!(b&128u)){*cursor=at;*value=v;return TCW_OK;}
    }
    return TCW_FORMAT;
}
static int new_label(const CxWorld* w,ReplayMap* m,uint32_t* out) {
    if(m->label>=w->labels_count)return TCW_FORMAT;
    uint32_t encoded;int s=read_var(&w->map,w->map.length,&m->at,&encoded);if(s)return s;
    int64_t delta=(encoded&1u)?-(int64_t)(encoded>>1)-1:(int64_t)(encoded>>1);
    int64_t value=(int64_t)m->previous+delta;
    if(value<=0||value>w->networks)return TCW_FORMAT;
    m->previous=(uint32_t)value;++m->label;*out=m->previous;return TCW_OK;
}
static int joined(const CxWorld* w,ReplayMap* m,uint32_t a,uint32_t b,uint32_t* out) {
    if(a&&b&&a!=b)return TCW_FORMAT;
    if(a||b){*out=a?a:b;return TCW_OK;}
    return new_label(w,m,out);
}
static int failed(CxOrderReplay* r,int status){r->state=4;r->failure=status;return status;}
int cx_order_replay_create(CxWorld* w,CxOrderReplay** out) {
    if(!out)return TCW_INVALID;*out=NULL;
    if(!w||!w->height||!w->width||w->height>UINT32_MAX/16u||w->phase==CX_CANCELLED||w->phase==CX_FAILED)return TCW_STATE;
    CxOrderReplay* r=cx_alloc(w,sizeof(*r));if(!r)return TCW_MEMORY;memset(r,0,sizeof(*r));r->owner=w;
    r->front=cx_alloc(w,w->height*16u);if(!r->front){cx_free(w,r);return TCW_MEMORY;}
    *out=r;return TCW_OK;
}
void cx_order_replay_destroy(CxWorld* w,CxOrderReplay* r) {
    if(!r||w!=r->owner)return;
    cx_free(w,r->front);cx_free(w,r);
}
void cx_order_replay_cancel(CxOrderReplay* r) {
    if(r){r->state=0;r->failure=0;r->cp_run=0;r->batch=0;r->pending_net=0;r->pending_word=0;}
}
int cx_order_replay_begin(CxOrderReplay* r,uint32_t x,uint32_t y) {
    if(!r||!r->owner)return TCW_STATE;
    const CxWorld* w=r->owner;
    if(w->phase==CX_CANCELLED||w->phase==CX_FAILED)return TCW_STATE;
    if(x>=w->width||y>=w->height)return TCW_INVALID;
    cx_order_replay_cancel(r);
    uint32_t index=x/CX_CHECK_COLUMNS;
    if(!w->checkpoint_index||index>=w->checkpoint_count)return failed(r,TCW_FORMAT);
    const CxCheckpoint* cp=w->checkpoint_index+index;
    if(cp->column!=index*CX_CHECK_COLUMNS||cp->column>x||
       cp->offset>w->checkpoints.length||cp->length>w->checkpoints.length-cp->offset||
       cp->map_offset>w->map.length||cp->map_previous>w->networks||cp->label_count>w->labels_count)return failed(r,TCW_FORMAT);
    r->target_x=x;r->target_y=y;r->x=cp->column;r->colour=r->row=r->up=0;
    r->cp_at=cp->offset;r->cp_end=cp->offset+cp->length;r->cp_run=0;
    r->map_at=cp->map_offset;r->map_previous=cp->map_previous;r->label_cursor=cp->label_count;
    memset(r->nets,0,sizeof(r->nets));r->state=1;return TCW_OK;
}
int cx_order_replay_begin_batch(CxOrderReplay* r,uint32_t x,uint32_t y) {
    int status=cx_order_replay_begin(r,x,y);if(!status)r->batch=1;return status;
}
static void advance_cell(const CxWorld* w,CxOrderReplay* r) {
    if(++r->row==w->height){r->row=0;r->up=0;if(++r->colour==4u){r->colour=0;++r->x;}}
}
static int replay_step(CxWorld* w,CxOrderReplay* r,uint32_t work,CxOrderReplayGeometry geometry,void* context,uint32_t nets[4],CxOrderReplayVisitor visitor,void* visitor_context) {
    if(!w||!r||r->owner!=w||!geometry||(!nets&&!visitor)||!work)return TCW_INVALID;
    if(w->phase==CX_CANCELLED||w->phase==CX_FAILED||!r->state)return TCW_STATE;
    if(r->state==4)return r->failure;
    if(r->state==3){if(nets)memcpy(nets,r->nets,sizeof(r->nets));return TCW_OK;}
    while(work) {
        if(r->state==5) {
            /* Coordinates, frontier and map are already committed. Retry only
             * the visitor until it reports that this cell is fully handled. */
            int status=visitor(visitor_context,r->x,r->row,r->colour,r->pending_net,r->pending_word);
            if(status==1)return TCW_CONTINUE;
            if(status<0)return failed(r,status);
            if(status)return failed(r,TCW_STATE);
            --work;
            if(r->x==r->target_x&&r->row==r->target_y&&r->colour==3u){r->state=3;return TCW_OK;}
            advance_cell(w,r);r->state=2;continue;
        }
        if(r->state==1) {
            if(!r->cp_run) {
                uint32_t at=r->cp_at,run,value;
                if(read_var(&w->checkpoints,r->cp_end,&at,&run)||read_var(&w->checkpoints,r->cp_end,&at,&value)||
                   !run||run>w->height-r->row||value>w->networks)return failed(r,TCW_FORMAT);
                r->cp_at=at;r->cp_run=run;r->cp_value=value;
                --work;continue;
            }
            uint32_t n=r->cp_run<work?r->cp_run:work;
            for(uint32_t i=0;i<n;i++)r->front[(r->row+i)*4u+r->colour]=r->cp_value;
            r->row+=n;r->cp_run-=n;work-=n;
            if(r->row==w->height){r->row=0;if(++r->colour==4u){
                if(r->cp_at!=r->cp_end)return failed(r,TCW_FORMAT);
                r->colour=0;r->up=0;r->state=2;
            }}
            continue;
        }
        uint8_t word=0,route=0;
        int s=geometry(context,r->x,r->row,&word,&route);
        if(s==1||s==2)return s;
        if(s<0)return failed(r,s);
        if(s||route>3u)return failed(r,TCW_FORMAT);
        uint32_t at=r->row*4u+r->colour,left=r->front[at],up=r->up,right=0,down=0,ignored=0;
        ReplayMap map={r->map_at,r->map_previous,r->label_cursor};
        if((word&(1u<<r->colour))&&cx_inside_wiring(w,r->x,r->row)) {
            if(route==1u){
                if(left)right=left;else if((s=new_label(w,&map,&right)))return failed(r,s);
                if(up)down=up;else if((s=new_label(w,&map,&down)))return failed(r,s);
            }else if(route==2u){
                if((s=joined(w,&map,left,up,&ignored))||(s=new_label(w,&map,&right)))return failed(r,s);
                down=right;
            }else if(route==3u){
                if(up)right=up;else if((s=new_label(w,&map,&right)))return failed(r,s);
                if(left)down=left;else if((s=new_label(w,&map,&down)))return failed(r,s);
            }else{
                if((s=joined(w,&map,left,up,&right)))return failed(r,s);down=right;
            }
        }
        /* Commit a whole cell only after callback and all label reads succeed. */
        r->map_at=map.at;r->map_previous=map.previous;r->label_cursor=map.label;
        r->front[at]=right;r->up=down;
        if(r->batch){
            r->pending_net=right;r->pending_word=word;r->state=5;--work;continue;
        }
        if(r->x==r->target_x&&r->row==r->target_y){r->nets[r->colour]=right;
            if(r->colour==3u){r->state=3;memcpy(nets,r->nets,sizeof(r->nets));return TCW_OK;}}
        --work;
        advance_cell(w,r);
    }
    return TCW_CONTINUE;
}

int cx_order_replay_step(CxWorld* w,CxOrderReplay* r,uint32_t work,CxOrderReplayGeometry geometry,void* context,uint32_t nets[4]) {
    if(r&&r->batch)return TCW_STATE;
    return replay_step(w,r,work,geometry,context,nets,NULL,NULL);
}
int cx_order_replay_step_batch(CxWorld* w,CxOrderReplay* r,uint32_t work,CxOrderReplayGeometry geometry,void* context,CxOrderReplayVisitor visitor,void* visitor_context) {
    if(!r||!visitor)return TCW_INVALID;
    if(!r->batch)return TCW_STATE;
    return replay_step(w,r,work,geometry,context,NULL,visitor,visitor_context);
}
