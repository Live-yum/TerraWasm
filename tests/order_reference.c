/* Frozen f9 source-order oracle. Linked into tests only. */
#define cx_order_rank reference_cx_order_rank
#define cx_order_gate_position reference_cx_order_gate_position
#define cx_order_capture_column reference_cx_order_capture_column
#define cx_order_close reference_cx_order_close
#define cx_order_candidates reference_cx_order_candidates
#define cx_order_event_rank reference_cx_order_event_rank
/* Exact source-relative Wiring.HitWire lamp ordering. Sparse, budgeted storage;
 * no width*height tiles, Manhattan approximation, or unbounded synchronous BFS. */
#include "terra_circuit_world_internal.h"
#include <limits.h>
uint32_t reference_cx_order_event_rank(CxWorld*,TerraVmGateRef);
#include <string.h>

typedef struct OrderLamp { uint32_t x,y,nets[4],rank[4],epoch; } OrderLamp;
typedef struct OrderItem { TerraVmGateRef event; uint32_t rank; } OrderItem;
struct CxOrder {
    uint32_t *cells,*columns,*gates; uint32_t count;
    OrderLamp* lamps;uint32_t lamp_count,lamp_capacity;
    uint8_t* process;uint32_t initialized,epoch;
    uint32_t* touched;uint32_t touched_count,touched_capacity;
    uint32_t* queue;uint32_t queue_capacity,head,length;
    uint32_t sx,sy,sw,sh,mask,colour,sequence,phase,scan;
    OrderItem *items,*scratch;uint32_t item_capacity,item_count;
    TerraVmGateRef* events;
    uint32_t run,left,mid,right,a,b,out;
};
static void* order_array(CxWorld* w,uint64_t count,uint32_t size){
    if(count>UINT32_MAX/size){cx_fail(w,TCW_MEMORY,"wire ordering allocation exceeds address space");return NULL;}
    void* p=cx_alloc(w,(uint32_t)(count?count:1u)*size);
    if(p&&w->vm&&terra_vm_set_callback_budget(w->vm,cx_vm_available(w))<0){cx_free(w,p);return NULL;}return p;
}
static uint32_t order_cell(CxWorld* w,uint32_t x,uint32_t y){
    CxOrder* o=w->order;if(x>=w->width||y>=w->height)return CX_NONE;
    uint32_t lo=o->columns[x],hi=o->columns[x+1u];
    while(lo<hi){uint32_t m=lo+(hi-lo)/2u,v=o->cells[m]&0xffffffu;if(v<y)lo=m+1u;else hi=m;}
    return lo<o->columns[x+1u]&&(o->cells[lo]&0xffffffu)==y?lo:CX_NONE;
}
static uint32_t order_x(CxWorld* w,uint32_t cell){
    CxOrder* o=w->order;uint32_t lo=0,hi=w->width;
    while(lo<hi){uint32_t m=lo+(hi-lo)/2u;if(o->columns[m+1u]<=cell)lo=m+1u;else hi=m;}return lo;
}
static OrderLamp* order_lamp(CxWorld* w,uint32_t x,uint32_t y){
    CxOrder* o=w->order;uint32_t lo=0,hi=o->lamp_count;
    while(lo<hi){uint32_t m=lo+(hi-lo)/2u;OrderLamp* l=o->lamps+m;if(l->x<x||(l->x==x&&l->y<y))lo=m+1u;else hi=m;}
    return lo<o->lamp_count&&o->lamps[lo].x==x&&o->lamps[lo].y==y?o->lamps+lo:NULL;
}
uint32_t cx_order_rank(CxWorld* w,uint32_t x,uint32_t y,uint32_t group){
    OrderLamp* l=order_lamp(w,x,y);if(!l||l->epoch!=w->order->epoch)return UINT32_MAX;
    for(uint32_t c=0;c<4;c++)if(l->nets[c]==group)return l->rank[c];return UINT32_MAX;
}
int cx_order_gate_position(CxWorld* w,uint32_t gate,uint32_t* x,uint32_t* y){
    if(!w->order||!gate||gate>w->gates)return 0;*x=w->order->gates[gate*2u];*y=w->order->gates[gate*2u+1u];return 1;
}
int cx_order_capture_column(CxWorld* w){
    if(w->twld_state)return TCW_OK; /* Explicit mod rule profile remains unchanged. */
    if(!w->order){
        if(w->height>0xffffffu||w->wire_cells>UINT32_MAX/4u)return cx_fail(w,TCW_MEMORY,"wire ordering index exceeds coordinate capacity");
        CxOrder* o=(CxOrder*)cx_alloc(w,sizeof(CxOrder));if(!o)return TCW_MEMORY;memset(o,0,sizeof(*o));w->order=o;
        o->cells=(uint32_t*)order_array(w,w->wire_cells,4);o->columns=(uint32_t*)order_array(w,(uint64_t)w->width+1u,4);
        o->gates=(uint32_t*)order_array(w,((uint64_t)w->gates+1u)*2u,4);
        if(!o->cells||!o->columns||!o->gates)return TCW_MEMORY;
    }
    CxOrder* o=w->order;o->columns[w->x]=o->count;
    for(uint32_t y=0;y<w->height;y++){
        TxTile* t=w->column+y;uint32_t mask=cx_wire_mask(t),gate=w->gate_at_y[y];
        if(gate){o->gates[gate*2u]=w->x;o->gates[gate*2u+1u]=y;}
        if(!mask)continue;
        if(o->count>=w->wire_cells)return cx_fail(w,TCW_STATE,"wire ordering index count mismatch");
        uint32_t route=0;if(t->active&&(t->type==424u||t->type==445u)){int r=t->type==424u?t->frame_x/18:0;if(r>=0&&r<=2)route=(uint32_t)r+1u;}
        o->cells[o->count++]=y|(mask<<24)|(route<<28)|(t->active&&t->type==419u?1u<<30:0u);
        if(t->active&&t->type==419u){
            if(o->lamp_count==o->lamp_capacity){uint32_t cap=o->lamp_capacity?o->lamp_capacity*2u:64u;if(cap<o->lamp_capacity)return TCW_MEMORY;
                OrderLamp* p=(OrderLamp*)order_array(w,cap,sizeof(OrderLamp));if(!p)return TCW_MEMORY;if(o->lamp_count)memcpy(p,o->lamps,o->lamp_count*sizeof(OrderLamp));cx_free(w,o->lamps);o->lamps=p;o->lamp_capacity=cap;}
            OrderLamp* l=o->lamps+o->lamp_count++;l->x=w->x;l->y=y;l->epoch=0;for(uint32_t c=0;c<4;c++){l->nets[c]=w->ids[y*4u+c];l->rank[c]=UINT32_MAX;}
        }
    }
    o->columns[w->x+1u]=o->count;return TCW_OK;
}
void cx_order_close(CxWorld* w){
    CxOrder* o=w->order;if(!o)return;
    cx_free(w,o->cells);cx_free(w,o->columns);cx_free(w,o->gates);cx_free(w,o->lamps);cx_free(w,o->process);cx_free(w,o->touched);cx_free(w,o->queue);cx_free(w,o->items);cx_free(w,o->scratch);cx_free(w,o);w->order=NULL;
}
static int enqueue(CxWorld* w,uint32_t cell,uint32_t direction){
    CxOrder* o=w->order;
    if(o->length==o->queue_capacity){uint32_t cap=o->queue_capacity?o->queue_capacity*2u:256u;if(cap<o->queue_capacity)return -1;
        uint32_t* p=(uint32_t*)order_array(w,cap,4);if(!p)return -1;
        for(uint32_t i=0;i<o->length;i++)p[i]=o->queue[(o->head+i)%o->queue_capacity];cx_free(w,o->queue);o->queue=p;o->queue_capacity=cap;o->head=0;}
    o->queue[(o->head+o->length)%o->queue_capacity]=(cell<<2)|direction;++o->length;return 0;
}
static int touch(CxWorld* w,uint32_t cell,uint8_t value){
    CxOrder* o=w->order;
    if(o->touched_count==o->touched_capacity){uint32_t cap=o->touched_capacity?o->touched_capacity*2u:256u;if(cap<o->touched_capacity)return -1;
        uint32_t* p=(uint32_t*)order_array(w,cap,4);if(!p)return -1;if(o->touched_count)memcpy(p,o->touched,o->touched_count*4u);cx_free(w,o->touched);o->touched=p;o->touched_capacity=cap;}
    o->touched[o->touched_count++]=cell;o->process[cell]=value;return 0;
}
static int order_begin(CxWorld* w,TerraVmGateRef* events,uint32_t count){
    CxOrder* o=w->order;o->events=events;o->item_count=count;o->phase=0;o->scan=0;++o->epoch;o->colour=0;o->sequence=0;o->length=o->head=0;
    o->sx=w->seed_x;o->sy=w->seed_y;o->sw=w->seed_width;o->sh=w->seed_height;o->mask=w->command.kind==TCW_TRIGGER?w->command.mask:15u;
    if(w->vm_source_gate){if(!cx_order_gate_position(w,w->vm_source_gate,&o->sx,&o->sy))return -1;o->sw=o->sh=1;o->mask=15u;}
    if(!o->process){o->process=(uint8_t*)order_array(w,o->count,1);if(!o->process)return -1;}
    if(count>o->item_capacity){
        OrderItem* a=(OrderItem*)order_array(w,count,sizeof(OrderItem));if(!a)return -1;
        OrderItem* b=(OrderItem*)order_array(w,count,sizeof(OrderItem));if(!b){cx_free(w,a);return -1;}
        cx_free(w,o->items);cx_free(w,o->scratch);o->items=a;o->scratch=b;o->item_capacity=count;
    }
    return 0;
}
/* Exactly one small traversal/reset/merge unit per VM operation. */
int32_t cx_order_candidates(void* context,TerraVmGateRef* events,uint32_t count,uint32_t begin){
    CxWorld* w=(CxWorld*)context;CxOrder* o=w->order;
    if(!o||count<2u)return 0;
    if(terra_vm_set_callback_budget(w->vm,cx_vm_available(w))<0)return -1;
    if(begin&&order_begin(w,events,count)<0)return -1;
    if(o->phase==0){
        if(!o->epoch&&o->scan<o->lamp_count){o->lamps[o->scan++].epoch=0;return 1;}
        if(!o->epoch){o->epoch=1;o->scan=0;}
        if(!o->initialized&&o->scan<o->count){uint32_t n=o->count-o->scan;if(n>256u)n=256u;memset(o->process+o->scan,0,n);o->scan+=n;return 1;}
        o->initialized=1;o->phase=1;o->scan=0;
    }
    if(o->phase==1){
        if(o->touched_count){uint32_t n=o->touched_count;if(n>256u)n=256u;while(n--)o->process[o->touched[--o->touched_count]]=0;return 1;}
        while(o->colour<4u&&!(o->mask&(1u<<o->colour)))++o->colour;
        if(o->colour==4u){o->phase=4;o->scan=0;return 1;}
        o->scan=0;o->phase=2;
    }
    if(o->phase==2){
        uint64_t area=(uint64_t)o->sw*o->sh;
        if(o->scan<area){uint32_t x=o->sx+o->scan/o->sh,y=o->sy+o->scan%o->sh;++o->scan;uint32_t cell=order_cell(w,x,y);
            if(cell!=CX_NONE&&(o->cells[cell]&(1u<<(24u+o->colour)))){if(touch(w,cell,4)<0)return -1;if(enqueue(w,cell,0)<0)return -1;}return 1;}
        o->phase=3;
    }
    if(o->phase==3){
        if(!o->length){++o->colour;o->phase=1;o->scan=0;return 1;}
        uint32_t entry=o->queue[o->head],cell=entry>>2,direction=entry&3u;o->head=(o->head+1u)%o->queue_capacity;--o->length;
        uint32_t word=o->cells[cell],x=order_x(w,cell),y=word&0xffffffu,route=(word>>28)&3u;
        if((word&(1u<<30))&&!(x>=o->sx&&x-o->sx<o->sw&&y>=o->sy&&y-o->sy<o->sh)){
            OrderLamp* l=order_lamp(w,x,y);if(l){if(l->epoch!=o->epoch){for(uint32_t c=0;c<4;c++)l->rank[c]=UINT32_MAX;l->epoch=o->epoch;}if(l->rank[o->colour]==UINT32_MAX)l->rank[o->colour]=o->sequence++;}
        }
        static const int8_t dx[4]={0,0,1,-1},dy[4]={1,-1,0,0};static const uint8_t routes[3][4]={{0,1,2,3},{3,2,1,0},{2,3,0,1}};
        for(uint32_t d=0;d<4;d++){
            if(route&&routes[route-1u][direction]!=d)continue;
            int64_t xx=(int64_t)x+dx[d],yy=(int64_t)y+dy[d];if(xx<2||yy<2||xx>=(int64_t)w->width-2||yy>=(int64_t)w->height-2)continue;
            uint32_t next=order_cell(w,(uint32_t)xx,(uint32_t)yy);if(next==CX_NONE||!(o->cells[next]&(1u<<(24u+o->colour))))continue;
            if(o->process[next]){--o->process[next];continue;}
            if(enqueue(w,next,d)<0)return -1;if(!((o->cells[next]>>28)&3u)&&touch(w,next,3)<0)return -1;
        }
        return 1;
    }
    if(o->phase==4){
        if(o->scan<o->item_count){uint32_t i=o->scan++;o->items[i].event=o->events[i];o->items[i].rank=cx_order_event_rank(w,o->events[i]);return 1;}
        o->run=1;o->left=0;o->phase=5;
    }
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
    if(o->phase==7){if(o->scan<o->item_count){uint32_t i=o->scan++;o->events[i]=o->items[i].event;return 1;}return 0;}
    return 1;
}
