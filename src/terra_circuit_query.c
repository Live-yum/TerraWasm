#include "terra_circuit_world_internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
extern int tx_bridge_range_is_valid(uint32_t,uint32_t);
extern void write_tile(TxWorld*,TxBuf*,const TxTile*,uint32_t);
extern int same_tile(const TxTile*,const TxTile*);

static int is_standard(const CxWorld* w,uint32_t y){if(!(y>=2u&&y<w->height&&w->column[y].active&&w->column[y].type==420u&&w->column[y].frame_x==36&&w->column[y-1].active&&w->column[y-1].type==419u&&w->column[y-1].frame_x!=36&&w->column[y-2].active&&w->column[y-2].type==419u&&w->column[y-2].frame_x==36))return 0;uint32_t row=y-2u;while(row){TxTile* t=w->column+--row;if(!t->active||t->type!=419u)break;if(t->frame_x==36)return 0;}return 1;}
static uint32_t point_gate(CxWorld* w,uint32_t y){if(w->column[y].type==420u)return w->gate_at_y[y];if(w->column[y].type==419u){if(y+1u<w->height&&is_standard(w,y+1u))return w->gate_at_y[y+1u];if(y+2u<w->height&&w->column[y].frame_x==36&&is_standard(w,y+2u))return w->gate_at_y[y+2u];}return 0;}
static int compare_points(const void* a,const void* b){const CxInputPoint* x=(const CxInputPoint*)a;const CxInputPoint* y=(const CxInputPoint*)b;if(x->x!=y->x)return x->x<y->x?-1:1;if(x->y!=y->y)return x->y<y->y?-1:1;return x->index<y->index?-1:x->index>y->index;}
static CxBinding* find_binding(CxWorld* w,uint32_t x,uint32_t y){for(uint32_t i=0;i<w->binding_count;i++)if(w->bindings[i].x==x&&w->bindings[i].y==y)return w->bindings+i;return NULL;}
static CxBinding* bind_point(CxWorld* w,uint32_t y){
    CxBinding* b=find_binding(w,w->x,y);if(b)return b;
    if(w->binding_count==w->binding_capacity){uint32_t n=w->binding_capacity?w->binding_capacity*2u:128u;if(n<w->binding_capacity||(uint64_t)n*sizeof(CxBinding)>UINT32_MAX)return NULL;CxBinding* p=(CxBinding*)cx_alloc(w,n*sizeof(CxBinding));if(!p)return NULL;if(w->binding_count)memcpy(p,w->bindings,w->binding_count*sizeof(CxBinding));cx_free(w,w->bindings);w->bindings=p;w->binding_capacity=n;}
    b=w->bindings+w->binding_count++;memset(b,0,sizeof(*b));b->x=w->x;b->y=y;b->tile=w->column[y];b->gate=point_gate(w,y);for(uint32_t c=0;c<4;c++){b->nets[c]=w->seed_ids[y*4u+c];b->opposite[c]=w->opposite_ids[y*4u+c];}return b;
}
static int routing(CxWorld* w,const TxTile* t){if(!t->active)return -1;if(t->type==424u){int r=t->frame_x/18;return r>=0&&r<=2?r:-1;}if(t->type==0xfffeu||(t->type==445u&&!w->twld_state))return 0;return -1;}
static uint32_t entering_net(CxWorld* w,const CxBinding* b,uint32_t colour,uint32_t direction){int r=routing(w,&b->tile);int other=r==0?direction>=2u:r==1?(direction==1u||direction==3u):r==2?(direction==1u||direction==2u):0;return other?b->opposite[colour]:b->nets[colour];}
static uint32_t trigger_net(CxWorld* w,const CxBinding* b,uint32_t colour){
    if(!(cx_wire_mask(&b->tile)&(1u<<colour)))return 0;if(cx_inside_wiring(w,b->x,b->y))return b->nets[colour];
    const int dx[4]={0,0,1,-1},dy[4]={1,-1,0,0};int r=routing(w,&b->tile);
    for(uint32_t d=0;d<4u;d++){if(r>=0&&d!=(r==1?3u:r==2?2u:0u))continue;int64_t x=(int64_t)b->x+dx[d],y=(int64_t)b->y+dy[d];if(x<0||y<0||!cx_inside_wiring(w,(uint32_t)x,(uint32_t)y))continue;CxBinding* inside=find_binding(w,(uint32_t)x,(uint32_t)y);if(inside)return entering_net(w,inside,colour,d);}
    return 0;
}
static void seek_checkpoint(CxWorld* w,uint32_t x){
    CxCheckpoint* c=w->checkpoint_index+x/CX_CHECK_COLUMNS;w->x=c->column;w->cursor=c->source_offset;w->map_cursor=c->map_offset;w->map_previous=c->map_previous;w->label_cursor=c->label_count;w->gate_cursor=c->gate_count;
    uint32_t at=c->offset;for(uint32_t colour=0;colour<4;colour++){uint32_t y=0;while(y<w->height){uint32_t run=cx_var_read(&w->checkpoints,&at),value=cx_var_read(&w->checkpoints,&at);for(uint32_t i=0;i<run;i++)w->front[(y+i)*4u+colour]=value;y+=run;}}
    w->y=0;w->col_stage=0;w->col_colour=w->col_row=0;w->input_length=0;w->emit_y=0;
}
static int reserve_result(CxWorld* w,uint32_t count){
    if(count<=w->result_capacity)return TCW_OK;if((uint64_t)count*16u>TERRA_CIRCUIT_WORLD_WINDOW)return TCW_INVALID;
    uint32_t* p=(uint32_t*)cx_alloc(w,(count?count:1u)*16u);if(!p)return TCW_MEMORY;cx_free(w,w->result);w->result=p;w->result_capacity=count;return TCW_OK;
}
#include "terra_circuit_fragments.inc"
int cx_query_begin(CxWorld* w){
    w->point_index=0;w->query_point=0;w->result_count=0;w->trigger_count=0;
    if(w->command.kind==TCW_VIEWPORT){w->query_x0=w->command.x;w->query_x1=w->command.x+w->command.width;w->query_y0=w->command.y;w->query_y1=w->command.y+w->command.height;w->query_stride=w->command.stride?w->command.stride:1u;
        uint64_t count=((uint64_t)w->command.width+w->query_stride-1u)/w->query_stride*(((uint64_t)w->command.height+w->query_stride-1u)/w->query_stride);if(count>65536u)return TCW_INVALID;int s=reserve_result(w,(uint32_t)count);if(s)return s;
    }else{
        if(!w->point_count)return cx_command_complete(w);
        w->query_x0=w->points[0].x;w->query_x1=w->points[w->point_count-1u].x+1u;w->query_y0=0;w->query_y1=w->height;w->query_stride=1;
        int cached=1;for(uint32_t i=0;i<w->point_count;i++)if(!find_binding(w,w->points[i].x,w->points[i].y)){cached=0;break;}
        if(cached)return cx_command_complete(w);
        if(w->command.kind==TCW_READ_LAMPS){int s=reserve_result(w,w->point_count);if(s)return s;}
    }
    seek_checkpoint(w,w->query_x0);w->phase=CX_QUERY;return TCW_OK;
}
static void pack_cell(CxWorld* w,uint32_t x,uint32_t y,uint32_t* out){
    if(w->command.flags&2u){const TxTile* t=w->column+y;uint32_t flags=(t->wall?1u:0u)|32u|(t->invisible_wall?64u:0u)|(t->fullbright_wall?128u:0u);out[0]=x;out[1]=y;out[2]=t->wall|(flags<<16);out[3]=t->wall_color;return;}
    TxTile t=w->column[y];uint32_t gate=point_gate(w,y);cx_current_tile(w,x,y,gate,&t,w->ids+y*4u);
    uint32_t flags=t.active|(t.actuator<<1)|(t.inactive<<2)|(t.wall?16u:0u);if(t.type==0xfffeu)flags|=8u;
    out[0]=x;out[1]=y;out[2]=t.type|(flags<<16)|(cx_wire_mask(&t)<<24);out[3]=(uint16_t)t.frame_x|((uint32_t)(uint16_t)t.frame_y<<16);
}
int cx_query_step(CxWorld* w,uint32_t* work){
    while(*work&&w->x<w->query_x1){int status=cx_scan_step(w,work);if(status<0||w->event.kind)return status;if(status!=2)return TCW_CONTINUE;
        if(w->x>=w->query_x0){
            if(w->command.kind==TCW_VIEWPORT){
                if((w->x-w->query_x0)%w->query_stride==0){if(w->emit_y<w->query_y0)w->emit_y=w->query_y0;while(*work&&w->emit_y<w->query_y1){if(w->result_count>=w->result_capacity)return TCW_INVALID;pack_cell(w,w->x,w->emit_y,w->result+w->result_count*4u);++w->result_count;w->emit_y+=w->query_stride;--*work;}if(w->emit_y<w->query_y1)return TCW_CONTINUE;}
            }else{
                while(w->point_index<w->point_count&&w->points[w->point_index].x<w->x)++w->point_index;
                while(*work&&w->point_index<w->point_count&&w->points[w->point_index].x==w->x){if(!bind_point(w,w->points[w->point_index].y))return TCW_MEMORY;++w->point_index;--*work;}
                if(w->point_index<w->point_count&&w->points[w->point_index].x==w->x)return TCW_CONTINUE;
            }
        }
        ++w->x;w->y=0;w->col_stage=0;w->emit_y=0;
    }
    if(w->x<w->query_x1)return TCW_CONTINUE;return cx_command_complete(w);
}
int cx_command_complete(CxWorld* w){
    if(w->command.kind==TCW_VIEWPORT){cx_emit(w,TCW_RESULT,0,0,w->result_count*16u,w->result);w->event.result_kind=TCW_VIEWPORT;w->event.result_count=w->result_count;return TCW_CONTINUE;}
    if(w->command.kind==TCW_READ_LAMPS){int s=reserve_result(w,w->point_count);if(s)return s;
        for(uint32_t i=0;i<w->point_count;i++){CxInputPoint* p=w->points+i;CxBinding* b=find_binding(w,p->x,p->y);if(!b)return TCW_STATE;uint32_t* out=w->result+p->index*4u;out[0]=p->x;out[1]=p->y;out[2]=cx_true_lamp(w,b);out[3]=b->tile.type;}
        w->result_count=w->point_count;cx_emit(w,TCW_RESULT,0,0,w->result_count*16u,w->result);w->event.result_kind=TCW_READ_LAMPS;w->event.result_count=w->result_count;return TCW_CONTINUE;
    }
    if(w->command.kind==TCW_WRITE_LAMPS){
        /* Validate the full batch before its first mutation. */
        for(uint32_t i=0;i<w->point_count;i++){CxBinding* b=find_binding(w,w->points[i].x,w->points[i].y);if(!b||!b->tile.active||b->tile.type!=419u||b->tile.frame_x==36)return cx_fail(w,TCW_INVALID,"lamp writes require ordinary logic lamps at every requested coordinate");}
        /* Reserve for every requested lamp so a later allocation cannot produce
         * a partially acknowledged program load. */
        uint64_t needed=(uint64_t)w->override_count+w->point_count;if(needed>UINT32_MAX/sizeof(CxOverride))return TCW_MEMORY;
        if(needed>w->override_capacity){CxOverride* p=(CxOverride*)cx_alloc(w,(uint32_t)needed*sizeof(CxOverride));if(!p)return TCW_MEMORY;if(w->override_count)memcpy(p,w->overrides,w->override_count*sizeof(CxOverride));cx_free(w,w->overrides);w->overrides=p;w->override_capacity=(uint32_t)needed;}
        for(uint32_t i=0;i<w->point_count;i++){CxInputPoint* p=w->points+i;CxBinding* b=find_binding(w,p->x,p->y);uint32_t value=p->value^(b->tile.frame_x==18);for(uint32_t c=0;c<4;c++)if(b->nets[c])value^=(uint32_t)terra_vm_parity(w->vm,b->nets[c]-1u);int s=cx_set_override(w,b->gate,b->x,b->y,value);if(s)return s;}
        w->phase=CX_IDLE;return TCW_OK;
    }
    if(w->command.kind==TCW_TRIGGER){
        if(w->phase==CX_RUN){cx_operation_commit(w);w->phase=CX_IDLE;return TCW_OK;}
        if((uint64_t)w->point_count*4u>UINT32_MAX)return TCW_MEMORY;int reserve=cx_seed_reserve(w,w->point_count*4u);if(reserve)return reserve;
        w->seed_lamps=0;uint32_t lazy=0;for(uint32_t i=0;i<w->point_count;i++){if(w->points[i].value)continue;CxBinding* b=find_binding(w,w->points[i].x,w->points[i].y);if(!b)return TCW_STATE;if(b->tile.type==419u)++w->seed_lamps;if(cx_lazy_toggle(&b->tile)){++lazy;++w->seed_lamps;}}
        if((uint64_t)w->override_count+lazy>UINT32_MAX)return TCW_MEMORY;reserve=cx_override_reserve(w,w->override_count+lazy);if(reserve)return reserve;
        w->trigger_count=0;
        CxDevice* device=(w->command.flags&1u)?cx_device_find(w,w->command.x,w->command.y):NULL;
        if(device&&device->tile.type==144u){CxBinding* b=find_binding(w,device->x,device->y);if(!b)return TCW_STATE;for(uint32_t c=0;c<4;c++)device->pulse_nets[c]=trigger_net(w,b,c);}
        if(!device||device->tile.type!=144u)for(uint32_t c=0;c<4;c++)if(w->command.mask&(1u<<c))for(uint32_t i=0;i<w->point_count;i++){if(w->points[i].value)continue;CxBinding* b=find_binding(w,w->points[i].x,w->points[i].y);if(!b)return TCW_STATE;uint32_t net=trigger_net(w,b,c);if(net)w->trigger_nets[w->trigger_count++]=net-1u;}
        w->trigger_remaining=w->command.count?w->command.count:1u;w->seed_x=w->command.x;w->seed_y=w->command.y;w->seed_width=w->command.width;w->seed_height=w->command.height;
        int s=cx_operation_begin(w);if(s)return s;w->interaction_pending=(w->command.flags&1u)!=0;
        s=terra_vm_begin(w->vm,w->trigger_nets,w->trigger_count);if(s<0){cx_operation_rollback(w);return s==TERRA_VM_OOM||s==TERRA_VM_LIMIT?TCW_MEMORY:TCW_STATE;}w->phase=CX_RUN;return TCW_OK;
    }
    if(w->command.kind==TCW_TICKS){int s=cx_seed_reserve(w,4u);if(s)return s;s=cx_operation_begin(w);if(s)return s;w->tick_remaining=w->command.count;w->tick_stage=0;w->phase=CX_TICKS;return TCW_OK;}
    w->phase=CX_IDLE;return TCW_OK;
}
int32_t terra_circuit_world_command(uint32_t handle,const TerraCircuitWorldCommand* input){
    CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;if(!input)return TCW_INVALID;if(w->phase!=CX_IDLE||w->event.kind)return TCW_STATE;
    TerraCircuitWorldCommand cmd=*input;if(cmd.abi_version!=1||cmd.kind<TCW_VIEWPORT||cmd.kind>TCW_EXTRACT||cmd.reserved1||cmd.reserved2||(cmd.flags&~(cmd.kind==TCW_TRIGGER?1u:cmd.kind==TCW_VIEWPORT?2u:0u)))return TCW_INVALID;
    if(cmd.kind==TCW_FRAGMENTS||cmd.kind==TCW_EXTRACT)return cx_fragments_begin(w,&cmd);
    if(cmd.kind==TCW_TRIGGER&&(cmd.flags&1u)){int s=cx_interaction_rect(w,&cmd);if(s<0)return TCW_INVALID;if(!s){w->command=cmd;return TCW_OK;}}
    if((cmd.kind==TCW_VIEWPORT||cmd.kind==TCW_TRIGGER)&&(!cmd.width||!cmd.height||cmd.x>=w->width||cmd.y>=w->height||cmd.width>w->width-cmd.x||cmd.height>w->height-cmd.y))return TCW_INVALID;
    if(cmd.kind==TCW_TRIGGER&&(cmd.mask>15u||!cmd.mask))return TCW_INVALID;
    if(cmd.kind==TCW_SAVE){if(!cmd.source_id||cmd.source_id==w->world_source||cmd.source_id==w->scratch_source||cmd.source_id==w->twld_source)return TCW_INVALID;if(w->twld&&(!cmd.aux_source_id||cmd.aux_source_id==cmd.source_id||cmd.aux_source_id==w->world_source||cmd.aux_source_id==w->scratch_source||cmd.aux_source_id==w->twld_source))return TCW_INVALID;w->command=cmd;w->output_source=cmd.source_id;w->output_offset=0;w->output_length=0;w->save_cursor=0;w->twld_saved_size=0;w->phase=CX_SAVE_PREFIX;w->input_length=0;return TCW_OK;}
    uint32_t count=0,allocated_count=0;
    if(cmd.kind==TCW_READ_LAMPS||cmd.kind==TCW_WRITE_LAMPS){if(cmd.data_count>65536u||(!cmd.data_ptr&&cmd.data_count))return TCW_INVALID;count=cmd.data_count;
#ifdef __wasm__
        if(count&&!tx_bridge_range_is_valid(cmd.data_ptr,count*16u))return TCW_INVALID;
#endif
    }else if(cmd.kind==TCW_TRIGGER){uint64_t n=(uint64_t)cmd.width*cmd.height;if(n>UINT32_MAX/(2u*sizeof(CxInputPoint)))return TCW_MEMORY;count=(uint32_t)n;allocated_count=count*2u;}
    if(!allocated_count)allocated_count=count;CxInputPoint* points=NULL;if(allocated_count){points=(CxInputPoint*)cx_alloc(w,allocated_count*sizeof(CxInputPoint));if(!points)return TCW_MEMORY;}
    if(cmd.kind==TCW_READ_LAMPS||cmd.kind==TCW_WRITE_LAMPS){const uint32_t* data=(const uint32_t*)(uintptr_t)cmd.data_ptr;for(uint32_t i=0;i<count;i++){uint32_t x=data[i*4u],y=data[i*4u+1u],value=data[i*4u+2u];if(x>=w->width||y>=w->height||(cmd.kind==TCW_WRITE_LAMPS&&value>1u)||data[i*4u+3u]){cx_free(w,points);return TCW_INVALID;}points[i]=(CxInputPoint){x,y,value,i};}}
    else if(cmd.kind==TCW_TRIGGER){uint32_t i=0;for(uint32_t x=cmd.x;x<cmd.x+cmd.width;x++)for(uint32_t y=cmd.y;y<cmd.y+cmd.height;y++){points[i]=(CxInputPoint){x,y,0,i};++i;if(!cx_inside_wiring(w,x,y)){const int dx[4]={0,0,1,-1},dy[4]={1,-1,0,0};for(uint32_t d=0;d<4;d++){int64_t a=(int64_t)x+dx[d],b=(int64_t)y+dy[d];if(a>=0&&b>=0&&cx_inside_wiring(w,(uint32_t)a,(uint32_t)b)){if(i>=allocated_count){cx_free(w,points);return TCW_MEMORY;}points[i]=(CxInputPoint){(uint32_t)a,(uint32_t)b,1,i};++i;}}}}count=i;}
    if(count)qsort(points,count,sizeof(CxInputPoint),compare_points);
    cx_free(w,w->points);w->points=points;w->point_count=count;w->command=cmd;w->error=0;
    int status=cmd.kind==TCW_TICKS?cx_command_complete(w):cx_query_begin(w);if(status<0){cx_operation_rollback(w);w->phase=CX_IDLE;}return status;
}
static int copy_source(CxWorld* w,uint32_t offset,uint32_t count){
    if(!w->world_source)return cx_emit(w,TCW_WRITE,w->output_source,w->output_offset,count,w->world->file+offset);
    if(w->input_offset!=offset||w->input_length!=count){w->read_target=0;return cx_emit(w,TCW_READ,w->world_source,offset,count,NULL);}
    return cx_emit(w,TCW_WRITE,w->output_source,w->output_offset,count,w->input);
}
static int flush_tile(CxWorld* w){
    if(!w->save_previous_valid)return TCW_OK;if(w->output_length>w->output_capacity-64u)return cx_emit(w,TCW_WRITE,w->output_source,w->output_offset,w->output_length,w->output);
    TxBuf b={w->output,w->output_length,w->output_capacity,1};write_tile(w->world,&b,&w->save_previous,w->save_repeat);if(!b.ok||b.data!=w->output)return cx_fail(w,TCW_MEMORY,"streaming WLD circuit output buffer overflow");w->output_length=b.len;w->save_previous_valid=0;return TCW_OK;
}
int cx_save_step(CxWorld* w,uint32_t* work){
    if(w->phase==CX_SAVE_PREFIX){
        if(w->save_cursor<w->tile_start){uint32_t n=w->tile_start-w->save_cursor;if(n>TERRA_CIRCUIT_WORLD_WINDOW)n=TERRA_CIRCUIT_WORLD_WINDOW;int s=copy_source(w,w->save_cursor,n);if(w->event.kind==TCW_WRITE)w->save_cursor+=n;--*work;return s;}
        w->phase=CX_SAVE_TILES;cx_scan_reset(w);w->save_previous_valid=0;w->save_repeat=0;return TCW_OK;
    }
    if(w->phase==CX_SAVE_TILES){
        while(*work&&w->x<w->width){int s=cx_scan_step(w,work);if(s<0||w->event.kind)return s;if(s!=2)return TCW_CONTINUE;
            while(*work&&w->emit_y<w->height){uint32_t y=w->emit_y;TxTile t=w->column[y];cx_current_tile(w,w->x,y,point_gate(w,y),&t,w->ids+y*4u);if(t.type==0xfffeu&&!cx_mod_vanilla(w,w->x,y,&t))return TCW_FORMAT;t.same=0;
                if(w->save_previous_valid&&w->save_repeat<32767u&&same_tile(&w->save_previous,&t))++w->save_repeat;
                else{s=flush_tile(w);if(s)return s;w->save_previous=t;w->save_repeat=0;w->save_previous_valid=1;}
                ++w->emit_y;--*work;
            }
            if(w->emit_y<w->height)return TCW_CONTINUE;s=flush_tile(w);if(s)return s;++w->x;w->y=0;w->col_stage=0;w->emit_y=0;
        }
        if(w->x<w->width)return TCW_CONTINUE;w->save_new_tile_end=w->output_offset+w->output_length;w->save_cursor=w->tile_end;w->phase=CX_SAVE_SUFFIX;
        if(w->output_length)return cx_emit(w,TCW_WRITE,w->output_source,w->output_offset,w->output_length,w->output);return TCW_OK;
    }
    if(w->phase==CX_SAVE_SUFFIX){
        if(w->save_cursor<w->world_size){uint32_t n=w->world_size-w->save_cursor;if(n>TERRA_CIRCUIT_WORLD_WINDOW)n=TERRA_CIRCUIT_WORLD_WINDOW;int s=copy_source(w,w->save_cursor,n);if(w->event.kind==TCW_WRITE)w->save_cursor+=n;--*work;return s;}
        w->save_result_size=w->output_offset;uint32_t table=w->world->version>=135u?26u:6u;
        for(uint32_t i=0;i<w->world->pointer_count;i++){uint32_t p=w->world->starts[i];if(i>=2u){if(w->world_source)p+=w->save_new_tile_end-w->tile_start;else p+=(uint32_t)((int64_t)w->save_new_tile_end-w->tile_end);}uint8_t* b=w->save_patch+i*4u;b[0]=(uint8_t)p;b[1]=(uint8_t)(p>>8);b[2]=(uint8_t)(p>>16);b[3]=(uint8_t)(p>>24);}
        w->phase=CX_SAVE_PATCH;return cx_emit(w,TCW_WRITE,w->output_source,table,w->world->pointer_count*4u,w->save_patch);
    }
    return TCW_STATE;
}
