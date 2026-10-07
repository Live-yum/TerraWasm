/* Compiled into terra_circuit_query.c to reuse its checkpoint seek and current
 * tile semantics. Indexing retains O(networks + wired objects + height), never a
 * width*height grid. Cooperative column replays build groups, anchors and
 * descriptors; selected records are materialized after the caller bound check. */
#include "terra_tile_record.h"
#include "terra_circuit_objects.h"
#include "terra_circuit_materials.h"
#include "terra_circuit_mechanism_tiles.h"
typedef struct CxGeometry {uint32_t type,frame,shape,anchor;} CxGeometry;
typedef struct CxFragmentObject {
    uint32_t x,y,width,height,type,net,flags,anchor,frame;
    uint32_t support_options,support_bad,support_choice,support_warning;
} CxFragmentObject;
typedef struct CxFragmentSupport {
    uint32_t x,y,object,option,kind,net,selected,chain_checked;
    TxTile tile;
} CxFragmentSupport;
struct CxFragments {
    CxGeometry* geometry;uint32_t geometry_count;
    uint32_t* parents;uint32_t* descriptors;uint32_t* owner;
    CxFragmentObject* objects;uint32_t object_count,object_capacity;
    uint32_t* slots;uint32_t slot_capacity;
    CxFragmentSupport* supports;uint32_t support_count,support_capacity,support_cursor,support_scan_count;
    uint32_t support_prepared,support_round;
    uint32_t stage,ready,total,selected,result_count,object_first,object_end;
    uint8_t* object_payload;uint32_t object_bytes,object_offset;
};
static uint32_t cf_root(CxFragments* f,uint32_t n){uint32_t root=n;while(f->parents[root]!=root)root=f->parents[root];while(n!=root){uint32_t next=f->parents[n];f->parents[n]=root;n=next;}return root;}
static uint32_t cf_join(CxFragments* f,uint32_t a,uint32_t b){if(!a)return b;if(!b)return a;a=cf_root(f,a);b=cf_root(f,b);if(a>b){uint32_t n=a;a=b;b=n;}f->parents[b]=a;return a;}
static int cf_geometry_compare(const void* a,const void* b){const CxGeometry* x=a,*y=b;if(x->type!=y->type)return x->type<y->type?-1:1;if(x->frame!=y->frame)return x->frame<y->frame?-1:1;if(x->shape!=y->shape)return x->shape<y->shape?-1:1;return x->anchor<y->anchor?-1:x->anchor>y->anchor;}
static const CxGeometry* cf_geometry(CxFragments* f,const TxTile* t){
    uint32_t frame=(uint16_t)t->frame_x|((uint32_t)(uint16_t)t->frame_y<<16),lo=0,hi=f->geometry_count;
    while(lo<hi){uint32_t m=lo+(hi-lo)/2;CxGeometry* g=f->geometry+m;if(g->type<t->type||(g->type==t->type&&g->frame<frame))lo=m+1;else hi=m;}
    if(lo==f->geometry_count||f->geometry[lo].type!=t->type||f->geometry[lo].frame!=frame)return NULL;
    if(lo+1<f->geometry_count&&f->geometry[lo+1].type==t->type&&f->geometry[lo+1].frame==frame&&(f->geometry[lo+1].shape!=f->geometry[lo].shape||f->geometry[lo+1].anchor!=f->geometry[lo].anchor))return NULL;
    return f->geometry+lo;
}
static uint32_t cf_hash(uint32_t x,uint32_t y,uint32_t type){uint32_t h=x*0x9e3779b1u^y*0x85ebca6bu^type*0xc2b2ae35u;h^=h>>16;return h;}
static int cf_slots(CxWorld* w,uint32_t capacity){
    CxFragments* f=w->fragments;uint32_t* p=cx_alloc(w,capacity*4u);if(!p)return 0;memset(p,0,capacity*4u);
    for(uint32_t i=0;i<f->object_count;i++){CxFragmentObject* o=f->objects+i;uint32_t at=cf_hash(o->x,o->y,o->type)&(capacity-1);while(p[at])at=(at+1)&(capacity-1);p[at]=i+1;}
    cx_free(w,f->slots);f->slots=p;f->slot_capacity=capacity;return 1;
}
static int cf_object(CxWorld* w,uint32_t x,uint32_t y,uint32_t width,uint32_t height,uint32_t type,uint32_t net,uint32_t flags,uint32_t anchor,uint32_t frame){
    CxFragments* f=w->fragments;if(!net)return TCW_OK;
    if(!f->slot_capacity||f->object_count>=f->slot_capacity/2){uint32_t n=f->slot_capacity?f->slot_capacity*2:128;if(n<f->slot_capacity||n>UINT32_MAX/4u||!cf_slots(w,n))return TCW_MEMORY;}
    uint32_t at=cf_hash(x,y,type)&(f->slot_capacity-1);
    while(f->slots[at]){CxFragmentObject* o=f->objects+f->slots[at]-1;if(o->x==x&&o->y==y&&o->type==type){o->net=cf_join(f,o->net,net);o->flags|=flags;if(o->width!=width||o->height!=height||o->anchor!=anchor)o->flags|=1;return TCW_OK;}at=(at+1)&(f->slot_capacity-1);}
    if(f->object_count==f->object_capacity){uint32_t n=f->object_capacity?f->object_capacity*2:64;if(n<f->object_capacity||n>UINT32_MAX/sizeof(CxFragmentObject))return TCW_MEMORY;CxFragmentObject* p=cx_alloc(w,n*sizeof(*p));if(!p)return TCW_MEMORY;if(f->object_count)memcpy(p,f->objects,f->object_count*sizeof(*p));cx_free(w,f->objects);f->objects=p;f->object_capacity=n;}
    f->objects[f->object_count]=(CxFragmentObject){x,y,width,height,type,net,flags,anchor,frame,0,0,UINT32_MAX,0};f->slots[at]=++f->object_count;return TCW_OK;
}
static int cf_known_single(uint32_t type){return type==19||type==427||(type>=435&&type<=439)||type==135||type==136||type==137||type==141||type==144||type==314||type==419||type==420||type==424||type==428||type==442||type==445||type==0xfffeu;}
static int cf_wire_object(const TxTile* t){
    if(t->actuator||tx_tile_needs_section(t->type)||t->type==0xfffeu)return 1;
    if(t->type==15u&&t->frame_y/40!=1&&t->frame_y/40!=20)return 0;
    return cx_wire_object_type(t->type)||cx_material_has(t->type,CX_MATERIAL_BOULDER)||t->type==484u||t->type==654u;
}
static uint32_t cf_single_anchor(const TxTile* t){
    if(t->type==135u||t->type==141u||t->type==428u)return 1;
    if(t->type==136u)return 5;
    if(t->type==419u)return 8;
    if(t->type==442u){uint32_t d=(uint16_t)t->frame_x/22u;return d==0?1u:d==1?2u:d==2?15u:d==3?16u:0u;}
    return 0;
}
static int cf_collect(CxWorld* w,uint32_t y){
    CxFragments* f=w->fragments;TxTile* t=w->column+y;uint32_t wires=cx_wire_mask(t),net=0;
    if(!wires||!t->active||!cf_wire_object(t))return TCW_OK;
    for(uint32_t c=0;c<4;c++){net=cf_join(f,net,w->seed_ids[y*4+c]);net=cf_join(f,net,w->opposite_ids[y*4+c]);}
    if(!net)return TCW_OK;const CxGeometry* g=cf_geometry(f,t);uint32_t x=w->x,top=y,width=1,height=1,flags=0;
    if(g){uint32_t dx=g->shape&255u,dy=(g->shape>>8)&255u;width=(g->shape>>16)&255u;height=g->shape>>24;
        if(dx>x||dy>y||width>w->width-(x-dx)||height>w->height-(y-dy)){width=height=1;flags|=1;}else{x-=dx;top-=dy;}}
    else if(tx_tile_framed(w->world,t->type)&&!cf_known_single(t->type))flags|=1;
    if(tx_tile_needs_section(t->type))flags|=2;if(t->type==0xfffeu)flags|=4;
    int status=cf_object(w,x,top,width,height,t->type,net,flags,g?g->anchor:cf_single_anchor(t),(uint16_t)t->frame_x|((uint32_t)(uint16_t)t->frame_y<<16));
    if(status)return status;
    /* Natural boulder traps release an unwired payload by retracting the wired
     * support below it. Retain that complete payload, never the surrounding cave. */
    if(y&&(t->actuator||t->type==130u||t->type==131u)){
        TxTile* above=w->column+y-1u;
        if(above->active&&(cx_material_has(above->type,CX_MATERIAL_BOULDER)||above->type==484u||above->type==654u)){
            const CxGeometry* shape=cf_geometry(f,above);uint32_t ax=w->x,ay=y-1u,aw=1,ah=1,af=0;
            if(shape){uint32_t dx=shape->shape&255u,dy=shape->shape>>8&255u;aw=shape->shape>>16&255u;ah=shape->shape>>24;
                if(dx>ax||dy>ay||aw>w->width-(ax-dx)||ah>w->height-(ay-dy)){af=1;aw=ah=1;}else{ax-=dx;ay-=dy;}}
            else af=1;
            return cf_object(w,ax,ay,aw,ah,above->type,net,af,shape?shape->anchor:0,(uint16_t)above->frame_x|((uint32_t)(uint16_t)above->frame_y<<16));
        }
    }
    return TCW_OK;
}
static int cf_gate_stacks(CxWorld* w){
    CxFragments* f=w->fragments;for(uint32_t y=0;y<w->height;y++)if(w->column[y].active&&w->column[y].type==419){
        uint32_t first=y,net=0;while(y<w->height&&w->column[y].active&&w->column[y].type==419){for(uint32_t c=0;c<4;c++)net=cf_join(f,net,w->seed_ids[y*4+c]);y++;}
        if(y<w->height&&w->column[y].active&&w->column[y].type==420){for(uint32_t c=0;c<4;c++)net=cf_join(f,net,w->seed_ids[y*4+c]);int s=cf_object(w,w->x,first,1,y-first+1,UINT32_MAX,net,0,0,0);if(s)return s;}
        if(y==w->height)break;
    }return TCW_OK;
}
#include "terra_circuit_supports.inc"
static int cf_object_compare(const void* a,const void* b){const CxFragmentObject* x=a,*y=b;if(x->x!=y->x)return x->x<y->x?-1:1;return x->y<y->y?-1:x->y>y->y;}
static void cf_column_owners(CxWorld* w){
    CxFragments* f=w->fragments;memset(f->owner,0,w->height*4u);
    while(f->object_first<f->object_count&&f->objects[f->object_first].x+255u<=w->x)f->object_first++;
    while(f->object_end<f->object_count&&f->objects[f->object_end].x<=w->x)f->object_end++;
    for(uint32_t i=f->object_first;i<f->object_end;i++){CxFragmentObject* o=f->objects+i;if(w->x>=o->x+o->width)continue;uint32_t group=cf_root(f,o->net);
        for(uint32_t y=o->y;y<o->y+o->height;y++){TxTile* t=w->column+y;int match=0;
            if(o->type==UINT32_MAX)match=t->active&&(t->type==419||t->type==420);
            else if(t->active&&t->type==o->type){if(o->width==1&&o->height==1)match=1;else{const CxGeometry* g=cf_geometry(f,t);match=g&&(g->shape&255u)==w->x-o->x&&((g->shape>>8)&255u)==y-o->y&&((g->shape>>16)&255u)==o->width&&(g->shape>>24)==o->height;}}
            if(match){if(f->owner[y]&&f->owner[y]!=group){if(!f->ready){f->descriptors[(group-1)*8+7]|=1;f->descriptors[(f->owner[y]-1)*8+7]|=1;}}else f->owner[y]=group;}
            else if(!f->ready)f->descriptors[(group-1)*8+7]|=1;
        }
        if(!f->ready)f->descriptors[(group-1)*8+7]|=o->flags;
    }
}
static uint32_t cf_groups(CxWorld* w,uint32_t y,uint32_t* groups,uint32_t* masks,uint32_t* layers){
    CxFragments* f=w->fragments;uint32_t count=0;if(f->owner[y]){groups[0]=f->owner[y];masks[0]=0;layers[0]=1;count=1;}
    while(f->support_cursor<f->support_count&&(f->supports[f->support_cursor].x<w->x||(f->supports[f->support_cursor].x==w->x&&f->supports[f->support_cursor].y<y)))f->support_cursor++;
    for(uint32_t at=f->support_cursor;at<f->support_count;at++){
        CxFragmentSupport* support=f->supports+at;if(support->x!=w->x||support->y!=y)break;if(!support->selected)continue;
        uint32_t group=cf_root(f,support->net),i=0;while(i<count&&groups[i]!=group)i++;
        if(i==count){if(count==16)return UINT32_MAX;groups[count]=group;masks[count]=layers[count]=0;count++;}
        layers[i]|=support->kind==CF_SUPPORT_WALL?2u:1u;
    }
    for(uint32_t c=0;c<4;c++)if(cx_wire_mask(w->column+y)&(1u<<c)){uint32_t nets[2]={w->seed_ids[y*4+c],w->opposite_ids[y*4+c]};for(uint32_t j=0;j<2;j++)if(nets[j]){uint32_t group=cf_root(f,nets[j]),i=0;while(i<count&&groups[i]!=group)i++;if(i==count){if(count==16)return UINT32_MAX;groups[count]=group;masks[count]=layers[count]=0;count++;}masks[i]|=1u<<c;}}
    return count;
}
void cx_fragments_cancel(CxWorld* w){CxFragments* f=w->fragments;if(!f)return;cx_free(w,f->object_payload);f->object_payload=NULL;f->object_bytes=f->object_offset=0;}
void cx_fragments_ack(CxWorld* w){if(w->fragments)w->fragments->object_offset+=w->event.length;}
void cx_fragments_free(CxWorld* w){CxFragments* f=w->fragments;if(!f)return;cx_fragments_cancel(w);cx_free(w,f->geometry);cx_free(w,f->parents);cx_free(w,f->descriptors);cx_free(w,f->owner);cx_free(w,f->objects);cx_free(w,f->slots);cx_free(w,f->supports);cx_free(w,f);w->fragments=NULL;}
static int cf_objects_build(CxWorld* w) {
    CxFragments* f=w->fragments;
    uint32_t count=0,total=32,origin_x=0,origin_y=0;
    CoItem* items=NULL;
    int result=TCW_FORMAT;
    for(uint32_t i=0;i<f->total;i++)if(f->descriptors[i*8]==f->selected) {
        origin_x=f->descriptors[i*8+1];
        origin_y=f->descriptors[i*8+2];
        if(f->descriptors[i*8+7]&5)return cx_fail(w,TCW_UNSUPPORTED,"unsupported fragment geometry or objects");
    }
    for(uint32_t i=0;i<f->object_count;i++)if(cf_root(f,f->objects[i].net)==f->selected&&co_section(f->objects[i].type))count++;
    if(count>w->command.height)return cx_fail(w,TCW_MEMORY,"fragment object count exceeds budget");
    if(count&&(w->world->version<88||w->world->version>326))return cx_fail(w,TCW_UNSUPPORTED,"object transfer requires a supported sectioned WLD");
    if(count) {
        items=cx_alloc(w,count*sizeof(*items));
        if(!items)return TCW_MEMORY;
        memset(items,0,count*sizeof(*items));
        uint32_t j=0;
        for(uint32_t i=0;i<f->object_count;i++) {
            CxFragmentObject* o=f->objects+i;
            if(cf_root(f,o->net)==f->selected&&co_section(o->type)) {
                items[j].x=o->x;
                items[j].y=o->y;
                items[j++].tile=o->type;
            }
        }
        for(uint32_t i=1;i<count;i++)if(!co_item_compare(items+i-1,items+i))goto done;
        const uint32_t sections[]= {
            2,3,5
        };
        for(uint32_t s=0;s<3;s++) {
            CoCursor cursor;
            if(!co_cursor(w->world,sections[s],&cursor))goto done;
            CoItem value;
            int next;
            while((next=co_next(&cursor,&value))==1) {
                CoItem* item=co_find(items,count,value.x,value.y);
                if(!item)continue;
                if(item->seen||co_section(item->tile)!=value.section||(value.section==5&&item->tile!=value.tile))goto done;
                uint32_t tile=item->tile;
                *item=value;
                item->tile=tile;
                item->seen=1;
                uint32_t length=co_encode_payload(item,NULL,326);
                if(length>w->command.width-total||32>w->command.width-total-length) {
                    result=TCW_MEMORY;
                    goto done;
                }
                total+=32+length;
            }
            if(next!=2)goto done;
        }
        for(uint32_t i=0;i<count;i++)if(!items[i].seen)goto done;
    }
    f->object_payload=cx_alloc(w,total);
    if(!f->object_payload) {
        result=TCW_MEMORY;
        goto done;
    }
    f->object_bytes=total;
    f->object_offset=0;
    uint8_t* p=f->object_payload;
    uint32_t header[]= {
        CO_MAGIC,1,w->world->version,count,total,origin_x,origin_y,0
    };
    for(uint32_t i=0;i<8;i++)co_put(p+i*4,header[i]);
    p+=32;
    for(uint32_t i=0;i<count;i++) {
        CoItem* a=items+i;
        uint32_t length=co_encode_payload(a,NULL,326);
        uint32_t h[]= {
            a->section,a->kind,a->x-origin_x,a->y-origin_y,a->tile,length,0,0
        };
        for(uint32_t k=0;k<8;k++)co_put(p+k*4,h[k]);
        co_encode_payload(a,p+32,326);
        p+=32+length;
    }
    result=TCW_OK;
    done:cx_free(w,items);
    return result?cx_fail(w,result,result==TCW_MEMORY?"fragment object bytes exceed budget":"object section missing, duplicate or malformed"):TCW_OK;
}
uint32_t cx_fragments_result_count(CxWorld* w){return w->fragments?(w->command.kind==TCW_FRAGMENTS?w->fragments->total:w->fragments->result_count):0;}
static int cf_emit_page(CxWorld* w){CxFragments* f=w->fragments;uint32_t n=w->command.x<f->total?f->total-w->command.x:0;if(n>w->command.count)n=w->command.count;int s=reserve_result(w,n*2);if(s)return s;if(n)memcpy(w->result,f->descriptors+w->command.x*8,n*32u);cx_emit(w,TCW_RESULT,0,0,n*32u,w->result);w->event.result_kind=TCW_FRAGMENTS;w->event.result_count=n;return TCW_CONTINUE;}
int cx_fragments_begin(CxWorld* w,const TerraCircuitWorldCommand* cmd){
    if(!cmd->count||cmd->count>32768u||cmd->data_count>65536u||(!cmd->data_ptr&&cmd->data_count))return TCW_INVALID;
    if(cmd->kind==TCW_EXTRACT&&(!w->fragments||!w->fragments->ready||cmd->data_count))return TCW_STATE;
    if(cmd->kind==TCW_EXTRACT&&(cmd->flags&1)&&(!cmd->aux_source_id||cmd->aux_source_id==w->world_source||cmd->aux_source_id==w->scratch_source||cmd->aux_source_id==w->twld_source||cmd->width<32||cmd->width>CO_LIMIT||!cmd->height||cmd->height>CO_COUNT))return TCW_INVALID;
#ifdef __wasm__
    if(cmd->data_count&&!tx_bridge_range_is_valid(cmd->data_ptr,cmd->data_count*16u))return TCW_INVALID;
#endif
    w->error=0;
    if(cmd->kind==TCW_FRAGMENTS&&w->fragments&&w->fragments->ready){if(cmd->data_count)return TCW_STATE;w->command=*cmd;return cf_emit_page(w);}
    if(cmd->kind==TCW_EXTRACT){CxFragments* f=w->fragments;cx_fragments_cancel(w);uint32_t i=0;while(i<f->total&&f->descriptors[i*8]!=cmd->mask)i++;if(i==f->total)return TCW_INVALID;if(f->descriptors[i*8+5]>cmd->count)return cx_fail(w,TCW_MEMORY,"fragment exceeds cell budget");
        int s=reserve_result(w,f->descriptors[i*8+5]*2u);if(s)return s;w->command=*cmd;f->selected=cmd->mask;f->result_count=0;f->stage=3;f->object_first=f->object_end=f->support_cursor=0;seek_checkpoint(w,f->descriptors[i*8+1]);w->query_x1=f->descriptors[i*8+1]+f->descriptors[i*8+3];w->phase=CX_FRAGMENTS;return TCW_OK;}
    CxGeometry* saved_geometry=NULL;uint32_t saved_count=0;
    if(w->fragments&&!cmd->data_count){saved_geometry=w->fragments->geometry;saved_count=w->fragments->geometry_count;w->fragments->geometry=NULL;}
    cx_fragments_free(w);CxFragments* f=cx_alloc(w,sizeof(*f));if(!f){cx_free(w,saved_geometry);return TCW_MEMORY;}memset(f,0,sizeof(*f));w->fragments=f;f->geometry=saved_geometry;f->geometry_count=saved_count;
    if(w->networks>UINT32_MAX/32u-1u)goto memory;
    f->parents=cx_alloc(w,(w->networks+1)*4u);f->descriptors=cx_alloc(w,(w->networks+1)*32u);f->owner=cx_alloc(w,w->height*4u);
    if(!f->parents||!f->descriptors||!f->owner)goto memory;memset(f->descriptors,0,(w->networks+1)*32u);for(uint32_t i=0;i<=w->networks;i++)f->parents[i]=i;
    if(cmd->data_count){f->geometry=cx_alloc(w,cmd->data_count*16u);if(!f->geometry)goto memory;memcpy(f->geometry,(const void*)(uintptr_t)cmd->data_ptr,cmd->data_count*16u);f->geometry_count=cmd->data_count;
        for(uint32_t i=0;i<f->geometry_count;i++){CxGeometry* g=f->geometry+i;uint32_t width=(g->shape>>16)&255u,height=g->shape>>24;if(g->type>65535u||g->anchor>17u||!width||!height||(g->shape&255u)>=width||((g->shape>>8)&255u)>=height){cx_fragments_free(w);return TCW_INVALID;}}
        qsort(f->geometry,f->geometry_count,sizeof(*f->geometry),cf_geometry_compare);
        uint32_t n=0;for(uint32_t i=0;i<f->geometry_count;i++)if(!n||memcmp(f->geometry+n-1,f->geometry+i,sizeof(*f->geometry)))f->geometry[n++]=f->geometry[i];f->geometry_count=n;
    }
    w->command=*cmd;f->stage=1;seek_checkpoint(w,0);w->phase=CX_FRAGMENTS;return TCW_OK;
memory:cx_fragments_free(w);return TCW_MEMORY;
}
int cx_fragments_step(CxWorld* w,uint32_t* work){
    CxFragments* f=w->fragments;uint32_t end=f->stage==3?w->query_x1:w->width;
    if(f->stage==4){if(f->object_offset<f->object_bytes){uint32_t n=f->object_bytes-f->object_offset;if(n>65536)n=65536;cx_emit(w,TCW_WRITE,w->command.aux_source_id,f->object_offset,n,f->object_payload+f->object_offset);return TCW_CONTINUE;}cx_fragments_cancel(w);cx_emit(w,TCW_RESULT,0,0,f->result_count*32u,w->result);w->event.result_kind=TCW_EXTRACT;w->event.result_count=f->result_count;return TCW_CONTINUE;}
    while(*work&&w->x<end){int status=cx_scan_step(w,work);if(status<0||w->event.kind)return status;if(status!=2)return TCW_CONTINUE;
        if(!w->emit_y){if(f->stage==1){int s=cf_gate_stacks(w);if(s)return s;}else if(f->stage!=5)cf_column_owners(w);}
        while(*work&&w->emit_y<w->height){uint32_t y=w->emit_y++,groups[16],masks[16],layers[16];--*work;
            if(f->stage==1){int s=cf_collect(w,y);if(s)return s;continue;}
            if(f->stage==5){int s=cf_support_scan(w,y);if(s)return s;continue;}
            uint32_t count=cf_groups(w,y,groups,masks,layers);if(count==UINT32_MAX)return cx_fail(w,TCW_FORMAT,"cell has over 16 support owners");
            for(uint32_t i=0;i<count;i++){uint32_t group=groups[i];if(f->stage==2){uint32_t* d=f->descriptors+(group-1)*8;
                    if(!d[5]){d[0]=group;d[1]=w->x;d[2]=y;d[3]=w->x;d[4]=y;}else{if(w->x<d[1])d[1]=w->x;if(y<d[2])d[2]=y;if(w->x>d[3])d[3]=w->x;if(y>d[4])d[4]=y;}d[5]++;if(masks[i])d[6]++;
                }else if(group==f->selected){if(f->result_count>=w->command.count||f->result_count*2>=w->result_capacity)return TCW_MEMORY;TxTile tile=w->column[y];cx_current_tile(w,w->x,y,point_gate(w,y),&tile,w->ids+y*4u);cf_filter_tile(&tile,layers[i]);tx_tile_record(w->result+f->result_count++*8,w->x,y,&tile,masks[i]);}
            }
        }
        if(w->emit_y<w->height)return TCW_CONTINUE;++w->x;w->y=0;w->col_stage=0;w->emit_y=0;
    }
    if(w->x<end)return TCW_CONTINUE;
    if(f->stage==1||f->stage==5){
        if(f->stage==5){int status=cf_support_resolve(w);if(status)return status;}
        if(f->support_prepared<f->object_count){int status=cf_support_prepare(w);if(status)return status;if(f->support_count){f->stage=5;seek_checkpoint(w,0);return TCW_OK;}}
        cf_support_finish(w);
        if(f->object_count>1)qsort(f->objects,f->object_count,sizeof(*f->objects),cf_object_compare);
        cx_free(w,f->slots);f->slots=NULL;f->slot_capacity=0;f->stage=2;f->object_first=f->object_end=f->support_cursor=0;seek_checkpoint(w,0);return TCW_OK;
    }
    if(f->stage==2){uint32_t n=0;for(uint32_t i=0;i<w->networks;i++){uint32_t* d=f->descriptors+i*8;if(d[5]){d[3]-=d[1]-1u;d[4]-=d[2]-1u;if(n!=i)memcpy(f->descriptors+n*8,d,32);n++;}}f->total=n;f->ready=1;return cf_emit_page(w);}
    if(w->command.flags&1){int status=cf_objects_build(w);if(status)return status;f->stage=4;return TCW_OK;}
    cx_emit(w,TCW_RESULT,0,0,f->result_count*32u,w->result);w->event.result_kind=TCW_EXTRACT;w->event.result_count=f->result_count;return TCW_CONTINUE;
}
