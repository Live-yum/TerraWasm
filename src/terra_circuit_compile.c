/* Column-frontier connected components and shared circuit rule bytecode.
 * The union table is discarded before rule construction. No width*height net
 * array is retained; a delta-coded label replay and 64-column checkpoints serve
 * coordinate queries and exports. Large intermediate rule storage is paged in
 * the host scratch file and is no longer needed after compilation. */
#include "terra_circuit_world_internal.h"
#include <limits.h>
#include <string.h>

static uint32_t root(CxWorld* w,uint32_t a){
    while(cx_word(&w->parents,a)!=a){uint32_t p=cx_word(&w->parents,a);uint32_t pp=cx_word(&w->parents,p);cx_set_word(&w->parents,a,pp);a=pp;}return a;
}
static uint32_t make_label(CxWorld* w){
    if(w->phase==CX_TOPOLOGY){
        if(w->labels_count>=0x7ffffffeu){cx_fail(w,TCW_MEMORY,"circuit connectivity labels exceed the native address space");return 0;}
        uint32_t id=++w->labels_count;if(!cx_words_size(w,&w->parents,id+1u))return 0;cx_set_word(&w->parents,id,id);return id;
    }
    if(w->label_cursor>=w->labels_count||w->map_cursor>=w->map.length){cx_fail(w,TCW_FORMAT,"compiled connectivity replay diverged from the WLD source");return 0;}
    uint32_t v=cx_var_read(&w->map,&w->map_cursor);int32_t delta=(int32_t)((v>>1)^(uint32_t)-(int32_t)(v&1u));
    w->map_previous=(uint32_t)((int64_t)w->map_previous+delta);++w->label_cursor;return w->map_previous;
}
static uint32_t join(CxWorld* w,uint32_t a,uint32_t b){
    if(!a)return b?b:make_label(w);if(!b)return a;
    if(w->phase!=CX_TOPOLOGY){if(a!=b)cx_fail(w,TCW_FORMAT,"wire network replay does not match its compiled union graph");return a;}
    a=root(w,a);b=root(w,b);if(a>b){uint32_t t=a;a=b;b=t;}cx_set_word(&w->parents,b,a);return a;
}
static int important_tile(const TxTile* t){return t->active&&(t->type==419u||t->type==420u||t->type==424u||t->type==445u||t->actuator||t->type==132u||t->type==135u||t->type==136u||t->type==144u||t->type==33u||t->type==4u||t->type==429u||t->type==423u);}
static int standard(const CxWorld* w,uint32_t y){
    if(!(y>=2u&&w->column[y].active&&w->column[y].type==420u&&w->column[y].frame_x==36&&
        w->column[y-1].active&&w->column[y-1].type==419u&&w->column[y-1].frame_x!=36&&
        w->column[y-2].active&&w->column[y-2].type==419u&&w->column[y-2].frame_x==36))return 0;
    uint32_t row=y-2u;while(row){TxTile* t=w->column+--row;if(!t->active||t->type!=419u)break;if(t->frame_x==36)return 0;}return 1;
}
void cx_scan_reset(CxWorld* w){
    w->cursor=w->tile_start;w->x=0;w->y=0;w->col_stage=0;w->col_colour=0;w->col_row=0;w->up=0;
    w->label_cursor=0;w->map_cursor=0;w->map_previous=0;w->gate_cursor=0;
    w->emit_y=w->emit_colour=w->emit_phase=w->emit_general=0;
    memset(w->front,0,w->height*16u);memset(w->ids,0,w->height*16u);w->input_length=0;
}
static int checkpoint(CxWorld* w){
    if(w->phase!=CX_COUNT||w->x%CX_CHECK_COLUMNS)return 1;
    CxCheckpoint* c=w->checkpoint_index+w->x/CX_CHECK_COLUMNS;
    *c=(CxCheckpoint){w->x,w->world_source?w->world->stream_columns[w->x]:w->cursor,w->map_cursor,w->map_previous,w->label_cursor,w->gate_cursor,w->checkpoints.length,0};
    /* Source cursor is captured before decoding this column. */
    c->source_offset=w->cursor;
    uint8_t bytes[10];
    for(uint32_t colour=0;colour<4;colour++){
        uint32_t previous=w->front[colour],run=1;
        for(uint32_t y=1;y<=w->height;y++){
            uint32_t value=y==w->height?UINT32_MAX:w->front[y*4u+colour];
            if(value==previous){++run;continue;}
            uint32_t n=cx_var_put(bytes,run);n+=cx_var_put(bytes+n,previous);
            if(!cx_bytes_append(w,&w->checkpoints,bytes,n))return 0;previous=value;run=1;
        }
    }
    c->length=w->checkpoints.length-c->offset;return 1;
}
int cx_scan_step(CxWorld* w,uint32_t* work){
    if(w->x>=w->width)return TCW_OK;
    if(w->col_stage==0&&w->y==0){if(!checkpoint(w))return TCW_MEMORY;w->col_stage=1;}
    while(*work&&w->col_stage==1){
        TxTile t;int s=cx_world_tile(w,&t);if(s)return s;uint32_t run=(uint32_t)t.same+1u;
        if(run>w->height-w->y)return cx_fail(w,TCW_FORMAT,"circuit RLE run crosses a world column");
        for(uint32_t i=0;i<run;i++)w->column[w->y+i]=t;
        if(w->phase==CX_TOPOLOGY){uint32_t wires=cx_wire_mask(&t);if(wires){w->wire_cells+=run;if(w->x<w->min_x)w->min_x=w->x;if(w->x>w->max_x)w->max_x=w->x;if(w->y<w->min_y)w->min_y=w->y;if(w->y+run-1u>w->max_y)w->max_y=w->y+run-1u;}
            if(important_tile(&t))w->devices_count+=run;
        }
        w->y+=run;--*work;if(w->y==w->height){int s=cx_mod_column(w);if(s)return s;memset(w->pixel_at_y,0,w->height*4u);if(w->phase==CX_COUNT){s=cx_devices_column(w);if(s)return s;}w->col_stage=2;w->col_colour=0;w->col_row=0;w->up=0;}
    }
    while(*work&&w->col_stage==2){
        uint32_t c=w->col_colour,y=w->col_row;TxTile* t=w->column+y;
        uint32_t left=w->front[y*4u+c],up=w->up,right=0,down=0,seed=0,opposite=0;
        if(cx_inside_wiring(w,w->x,y)&&(cx_wire_mask(t)&(1u<<c))){
            if(t->active&&(t->type==424u||(t->type==445u&&!w->twld_state)||t->type==0xfffeu)){
                int route=t->type==424u?t->frame_x/18:0;
                if(route<0||route>2){opposite=seed=right=down=join(w,left,up);}
                else if(route==1){seed=join(w,left,up);opposite=right=down=make_label(w);}
                else if(route==2){right=up?up:make_label(w);down=left?left:make_label(w);seed=right;opposite=down;}
                else {right=left?left:make_label(w);down=up?up:make_label(w);seed=down;opposite=right;}
            }else opposite=seed=right=down=join(w,left,up);
        }
        if(w->error)return -(int)w->error;
        w->front[y*4u+c]=right;w->up=down;w->ids[y*4u+c]=right;w->seed_ids[y*4u+c]=seed;w->opposite_ids[y*4u+c]=opposite;
        if(w->phase==CX_COUNT&&w->pixel_at_y[y])cx_pixel_ports(w,y,c,right,down);
        --*work;if(++w->col_row==w->height){w->col_row=0;w->up=0;if(++w->col_colour==4u){w->col_stage=3;w->col_row=0;
            if(w->phase==CX_TOPOLOGY)w->column_gates[w->x]=w->gate_cursor;
            else w->gate_cursor=w->column_gates[w->x];
        }}
    }
    while(*work&&w->col_stage==3){uint32_t y=w->col_row++;w->gate_at_y[y]=0;
        if(w->column[y].active&&w->column[y].type==420u)w->gate_at_y[y]=++w->gate_cursor;
        --*work;if(w->col_row==w->height){if(w->phase==CX_COUNT)cx_devices_bind_column(w);w->col_stage=4;w->emit_y=0;w->emit_colour=0;w->emit_phase=0;}
    }
    return w->col_stage==4?2:TCW_CONTINUE;
}
static void next_column(CxWorld* w){++w->x;w->y=0;w->col_stage=0;w->emit_y=w->emit_colour=w->emit_phase=0;}
static int grow_array(CxWorld* w,void** pointer,uint32_t* capacity,uint32_t count,uint32_t size){
    if(count<=*capacity)return 1;uint32_t n=*capacity?*capacity:32u;while(n<count){if(n>UINT32_MAX/2u)return 0;n*=2u;}
    if((uint64_t)n*size>UINT32_MAX)return 0;void* p=cx_alloc(w,n*size);if(!p)return 0;if(*pointer)memcpy(p,*pointer,*capacity*size);cx_free(w,*pointer);*pointer=p;*capacity=n;return 1;
}
static uint32_t find_general(CxWorld* w,uint32_t gate){uint32_t lo=0,hi=w->general_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(w->general[m].id<gate)lo=m+1;else hi=m;}return lo;}
static int add_general(CxWorld* w,uint32_t y){
    if(!grow_array(w,(void**)&w->general,&w->general_capacity,w->general_count+1u,sizeof(CxGeneralGate)))return 0;
    CxGeneralGate* g=w->general+w->general_count;memset(g,0,sizeof(*g));g->id=w->gate_at_y[y];g->x=w->x;g->y=y;g->first=w->lamp_count;g->style=(uint8_t)(w->column[y].frame_y/18);g->frame=g->initial_frame=(uint8_t)(w->column[y].frame_x/18);
    for(uint32_t c=0;c<4;c++)g->nets[c]=(y+2u==w->height&&cx_inside_wiring(w,w->x,y-1u)&&cx_wire_mask(w->column+y)&(1u<<c))?w->ids[(y-1u)*4u+c]:w->ids[y*4u+c];
    uint32_t row=y,conditions=0;int faulty=0;
    while(row>0){--row;TxTile* t=w->column+row;if(!t->active||t->type!=419u)break;
        if(!grow_array(w,(void**)&w->lamps,&w->lamp_capacity,w->lamp_count+1u,sizeof(CxGeneralLamp)))return 0;
        CxGeneralLamp* l=w->lamps+w->lamp_count++;memset(l,0,sizeof(*l));l->x=w->x;l->y=row;l->gate=g->id;l->initial=t->frame_x==18;l->faulty=t->frame_x==36;
        for(uint32_t c=0;c<4;c++)l->nets[c]=w->ids[row*4u+c];
        if(!faulty){if(l->faulty){faulty=1;g->faulty=1;}else ++conditions;}
    }
    g->count=conditions;g->trigger_first=g->first;g->trigger_count=w->lamp_count-g->first;++w->general_count;return 1;
}
static int append_rule(CxWorld* w,uint32_t net,uint32_t gate,const uint8_t* data,uint32_t n){
    if(!net)return TCW_OK;
    uint32_t prev=cx_word(&w->previous_gate,net),delta=gate-prev;
    if(gate<prev)return cx_fail(w,TCW_FORMAT,"circuit membership order is not monotonic");
    uint8_t member[5];uint32_t mn=cx_var_put(member,delta);
    if(w->phase==CX_COUNT){
        uint32_t old=cx_word(&w->code_ends,net);if(!old)++w->action_count;
        if(n>UINT32_MAX-old||mn>UINT32_MAX-cx_word(&w->member_ends,net))return cx_fail(w,TCW_MEMORY,"one compiled circuit net exceeds the address space");
        cx_set_word(&w->code_ends,net,old+n);cx_set_word(&w->member_ends,net,cx_word(&w->member_ends,net)+mn);
        cx_set_word(&w->previous_gate,net,gate);return TCW_OK;
    }
    if(!w->record_length){memcpy(w->record,data,n);memcpy(w->record+n,member,mn);w->record_length=n;w->record_member_length=mn;w->record_position=0;w->record_store=cx_word(&w->code_ends,net);}
    while(w->record_position<w->record_length){uint32_t pos=w->record_store+w->record_position,bytes=w->record_length-w->record_position,bound=CX_PAGE_SIZE-(pos&(CX_PAGE_SIZE-1u));if(bytes>bound)bytes=bound;
        int s=cx_store(w,pos,w->record+w->record_position,bytes,1);if(s)return s;w->record_position+=bytes;
    }
    while(w->record_position<w->record_length+w->record_member_length){uint32_t mpos=w->record_position-w->record_length;uint32_t pos=cx_word(&w->member_ends,net)+mpos,bytes=w->record_member_length-mpos,bound=CX_PAGE_SIZE-(pos&(CX_PAGE_SIZE-1u));if(bytes>bound)bytes=bound;
        int s=cx_store(w,pos,w->record+w->record_position,bytes,1);if(s)return s;w->record_position+=bytes;
    }
    cx_set_word(&w->code_ends,net,cx_word(&w->code_ends,net)+w->record_length);cx_set_word(&w->member_ends,net,cx_word(&w->member_ends,net)+w->record_member_length);
    cx_set_word(&w->previous_gate,net,gate);w->record_length=0;return TCW_OK;
}
static int column_rules(CxWorld* w,uint32_t* work){
    while(*work&&w->emit_y<w->height){uint32_t y=w->emit_y,gate=w->gate_at_y[y];
        if(!gate){++w->emit_y;--*work;continue;}
        if(standard(w,y)){
            uint8_t record[64];uint32_t n=1,nc=0,no=0;
            for(uint32_t c=0;c<4;c++)if(w->ids[(y-1u)*4u+c]){n+=cx_var_put(record+n,w->ids[(y-1u)*4u+c]);++nc;}
            for(uint32_t c=0;c<4;c++){uint32_t net=(y+2u==w->height&&cx_inside_wiring(w,w->x,y-1u)&&cx_wire_mask(w->column+y)&(1u<<c))?w->ids[(y-1u)*4u+c]:w->ids[y*4u+c];if(net){n+=cx_var_put(record+n,net);++no;}}
            record[0]=(uint8_t)(nc|(no<<3)|(w->column[y-1u].frame_x==18?64u:0u));
            while(w->emit_colour<4u){uint32_t c=w->emit_colour,net=w->ids[(y-2u)*4u+c];int s=append_rule(w,net,gate,record,n);if(s)return s;++w->emit_colour;}
        }else{
            if(!w->emit_phase){
                if(w->phase==CX_COUNT){if(!add_general(w,y))return TCW_MEMORY;w->emit_general=w->general_count-1u;}
                else{w->emit_general=find_general(w,gate);if(w->emit_general>=w->general_count||w->general[w->emit_general].id!=gate)return cx_fail(w,TCW_FORMAT,"general gate replay mismatch");}
                w->emit_phase=1;w->emit_gate=0;w->emit_colour=0;
            }
            CxGeneralGate* g=w->general+w->emit_general;
            while(w->emit_gate<g->trigger_count){CxGeneralLamp* l=w->lamps+g->trigger_first+w->emit_gate;uint8_t record[8];record[0]=(uint8_t)(128u|(l->faulty?1u:0u));uint32_t n=1+cx_var_put(record+1,g->trigger_first+w->emit_gate);
                while(w->emit_colour<4u){uint32_t net=l->nets[w->emit_colour];int s=append_rule(w,net,gate,record,n);if(s)return s;++w->emit_colour;}
                w->emit_colour=0;++w->emit_gate;
            }
        }
        ++w->emit_y;w->emit_colour=0;w->emit_phase=0;--*work;
    }
    return w->emit_y==w->height?TCW_OK:TCW_CONTINUE;
}
static uint32_t zigzag(int32_t v){return ((uint32_t)v<<1)^(uint32_t)(v>>31);}
int cx_compile_initialize(CxWorld* w){
    w->phase=CX_TOPOLOGY;if(!cx_words_size(w,&w->parents,1))return 0;cx_scan_reset(w);return 1;
}
static int prepare_count(CxWorld* w){
    if(!cx_words_size(w,&w->code_ends,w->networks+1u)||!cx_words_size(w,&w->member_ends,w->networks+1u)||!cx_words_size(w,&w->previous_gate,w->networks+1u))return 0;
    if(cx_devices_prepare(w)<0)return 0;
    w->phase=CX_COUNT;cx_scan_reset(w);return 1;
}
static int prepare_store(CxWorld* w){
    uint64_t size=(uint64_t)w->code_size+w->member_size;if(size>UINT32_MAX)return cx_fail(w,TCW_MEMORY,"compiled circuit scratch exceeds the file offset range");w->store_size=(uint32_t)size;
    w->store_pages=(w->store_size+CX_PAGE_SIZE-1u)/CX_PAGE_SIZE;w->cache_count=w->store_pages<CX_CACHE_PAGES?w->store_pages:CX_CACHE_PAGES;
    if(!w->cache_count)w->cache_count=1;
    w->cache=(uint8_t*)cx_alloc(w,w->cache_count*CX_PAGE_SIZE);w->cache_pages=(CxCachePage*)cx_alloc(w,w->cache_count*sizeof(CxCachePage));
    w->cache_map=(uint32_t*)cx_alloc(w,(w->store_pages?w->store_pages:1u)*4u);w->stored_pages=(uint8_t*)cx_alloc(w,(w->store_pages+7u)/8u+1u);
    w->output=(uint8_t*)cx_alloc(w,TERRA_CIRCUIT_WORLD_WINDOW);w->output_capacity=TERRA_CIRCUIT_WORLD_WINDOW;
    if(!w->cache||!w->cache_pages||!w->cache_map||!w->stored_pages||!w->output)return TCW_MEMORY;
    memset(w->cache_pages,0,w->cache_count*sizeof(CxCachePage));memset(w->cache_map,0,(w->store_pages?w->store_pages:1u)*4u);memset(w->stored_pages,0,(w->store_pages+7u)/8u+1u);memset(w->output,0,TERRA_CIRCUIT_WORLD_WINDOW);
    w->output_offset=0;w->phase=CX_ZERO;return TCW_OK;
}
static uint32_t popcount(uint32_t v){v=v-((v>>1)&0x55555555u);v=(v&0x33333333u)+((v>>2)&0x33333333u);return (((v+(v>>4))&0x0f0f0f0fu)*0x01010101u)>>24;}
uint32_t cx_action_index(const CxWorld* w,uint32_t net){
    if(!net||net>w->networks||!(w->action_bits[net>>3]&(1u<<(net&7u))))return CX_NONE;
    uint32_t block=net>>8,count=w->action_rank[block],start=block<<8;
    const uint32_t* words=(const uint32_t*)w->action_bits;
    for(uint32_t i=start>>5;i<(net>>5);i++)count+=popcount(words[i]);
    uint32_t mask=net&31u;count+=popcount(words[net>>5]&((mask==0u)?0u:((1u<<mask)-1u)));return count;
}
static int prepare_intern(CxWorld* w){
    cx_words_free(w,&w->previous_gate);
    uint32_t bitbytes=((w->networks+32u)/32u)*4u;
    w->action_bits=(uint8_t*)cx_alloc(w,bitbytes);w->action_rank=(uint32_t*)cx_alloc(w,((w->networks>>8)+2u)*4u);
    w->actions=(CxAction*)cx_alloc(w,(w->action_count?w->action_count:1u)*sizeof(CxAction));
    w->hash_capacity=16u;uint64_t needed=(uint64_t)w->action_count*4u/3u+1u;while(w->hash_capacity<needed){if(w->hash_capacity>UINT32_MAX/2u)return TCW_MEMORY;w->hash_capacity*=2u;}
    if((uint64_t)w->hash_capacity*sizeof(CxHash)>UINT32_MAX)return TCW_MEMORY;
    w->hashes=(CxHash*)cx_alloc(w,w->hash_capacity*sizeof(CxHash));
    w->intern_capacity=w->max_group<TERRA_CIRCUIT_WORLD_WINDOW?w->max_group:TERRA_CIRCUIT_WORLD_WINDOW;if(!w->intern_capacity)w->intern_capacity=1u;
    w->intern_buffer=(uint8_t*)cx_alloc(w,w->intern_capacity);
    if(!w->action_bits||!w->action_rank||!w->actions||!w->hashes||!w->intern_buffer)return TCW_MEMORY;
    memset(w->action_bits,0,bitbytes);memset(w->action_rank,0,((w->networks>>8)+2u)*4u);memset(w->hashes,0,w->hash_capacity*sizeof(CxHash));
    w->intern_group=1;w->intern_start=0;w->intern_member_start=w->code_size;w->intern_stage=0;w->phase=CX_INTERN;return TCW_OK;
}
static int intern_step(CxWorld* w,uint32_t* work){
    while(*work&&w->intern_group<=w->networks){uint32_t g=w->intern_group;
        if(!w->intern_stage){w->intern_end=cx_word(&w->code_ends,g);w->intern_member_end=cx_word(&w->member_ends,g);w->intern_copy=w->intern_member_copy=0;
            if(w->intern_end==w->intern_start){w->intern_start=w->intern_end;w->intern_member_start=w->intern_member_end;++w->intern_group;--*work;goto drop_pages;}
            w->intern_stage=1;w->intern_pool_code=w->code.length;
        }
        uint32_t length=w->intern_end-w->intern_start;
        while(w->intern_stage==1&&w->intern_copy<length){uint32_t at=w->intern_start+w->intern_copy,n=length-w->intern_copy,bound=CX_PAGE_SIZE-(at&(CX_PAGE_SIZE-1u));if(n>bound)n=bound;if(n>w->intern_capacity)n=w->intern_capacity;
            uint8_t* target=length<=w->intern_capacity?w->intern_buffer+w->intern_copy:w->intern_buffer;
            int s=cx_store(w,at,target,n,0);if(s)return s;
            if(length>w->intern_capacity&&!cx_bytes_append(w,&w->code,target,n))return TCW_MEMORY;
            w->intern_copy+=n;--*work;if(!*work)return TCW_CONTINUE;
        }
        if(w->intern_stage==1){
            if(length<=w->intern_capacity){uint64_t hash=1469598103934665603ull;for(uint32_t i=0;i<length;i++)hash=(hash^w->intern_buffer[i])*1099511628211ull;if(!hash)hash=1;
                uint32_t slot=(uint32_t)(hash^(hash>>32))&(w->hash_capacity-1u);
                while(w->hashes[slot].hash){CxHash* h=w->hashes+slot;if(h->hash==hash&&h->length==length&&cx_bytes_equal(&w->code,h->code,w->intern_buffer,length)){w->intern_pool_code=h->code;break;}slot=(slot+1u)&(w->hash_capacity-1u);}
                if(!w->hashes[slot].hash){w->intern_pool_code=w->code.length;if(!cx_bytes_append(w,&w->code,w->intern_buffer,length))return TCW_MEMORY;w->hashes[slot]=(CxHash){hash,w->intern_pool_code,length};}
            }
            w->actions[w->intern_action]=(CxAction){w->intern_pool_code,length,w->members.length};w->action_bits[g>>3]|=(uint8_t)(1u<<(g&7u));w->intern_stage=2;
        }
        while(w->intern_stage==2&&w->intern_member_start+w->intern_member_copy<w->intern_member_end){uint32_t at=w->intern_member_start+w->intern_member_copy,n=w->intern_member_end-at,bound=CX_PAGE_SIZE-(at&(CX_PAGE_SIZE-1u));if(n>bound)n=bound;if(n>w->intern_capacity)n=w->intern_capacity;
            int s=cx_store(w,at,w->intern_buffer,n,0);if(s)return s;if(!cx_bytes_append(w,&w->members,w->intern_buffer,n))return TCW_MEMORY;w->intern_member_copy+=n;--*work;if(!*work)return TCW_CONTINUE;
        }
        w->intern_start=w->intern_end;w->intern_member_start=w->intern_member_end;w->intern_stage=0;++w->intern_action;++w->intern_group;--*work;
drop_pages:
        if((g&(CX_WORDS-1u))==CX_WORDS-1u){cx_words_drop_page(w,&w->code_ends,g/CX_WORDS);cx_words_drop_page(w,&w->member_ends,g/CX_WORDS);}
    }
    if(w->intern_group<=w->networks)return TCW_CONTINUE;
    uint32_t rank=0;for(uint32_t g=0;g<=w->networks;g++){if(!(g&255u))w->action_rank[g>>8]=rank;if(w->action_bits[g>>3]&(1u<<(g&7u)))++rank;}
    if(rank!=w->action_count)return cx_fail(w,TCW_FORMAT,"compiled circuit action index count mismatch");
    cx_words_free(w,&w->code_ends);cx_words_free(w,&w->member_ends);
#define DROP(f) do{cx_free(w,w->f);w->f=NULL;}while(0)
    DROP(cache);DROP(cache_pages);DROP(cache_map);DROP(stored_pages);DROP(hashes);DROP(intern_buffer);
#undef DROP
    w->cache_count=0;w->eval_valid=0;int s=cx_devices_compile(w);if(s)return s;s=cx_create_vm(w);if(s)return s;w->phase=CX_IDLE;return TCW_OK;
}
int cx_compile_step(CxWorld* w,uint32_t* work){
    if(w->phase==CX_TOPOLOGY||w->phase==CX_COUNT||w->phase==CX_WRITE){
        while(*work&&w->x<w->width){int s=cx_scan_step(w,work);if(s<0||w->event.kind)return s;if(s==2){
                if(w->phase==CX_COUNT||w->phase==CX_WRITE){s=column_rules(w,work);if(s)return s;}
                next_column(w);
            }else if(s==TCW_CONTINUE)return s;
        }
        if(w->x<w->width)return TCW_CONTINUE;
        if(w->cursor!=w->tile_end)return cx_fail(w,TCW_FORMAT,"circuit tile section does not end at its declared boundary");
        if(w->phase==CX_TOPOLOGY){w->gates=w->gate_cursor;w->column_gates[w->width]=w->gates;w->phase=CX_ROOTS;w->phase_cursor=1;}
        else if(w->phase==CX_COUNT){cx_devices_end_columns(w);w->phase=CX_LAYOUT;w->phase_cursor=1;w->layout_code=w->layout_member=0;}
        else{w->phase=CX_FLUSH;w->flush_index=0;}
        return TCW_OK;
    }
    if(w->phase>=CX_ROOTS&&w->phase<=CX_MAP_ENCODE){
        while(*work&&w->phase_cursor<=w->labels_count){uint32_t i=w->phase_cursor++,value=cx_word(&w->parents,i);--*work;
            if(w->phase==CX_ROOTS){root(w,i);}
            else if(w->phase==CX_NUMBER){if(value==i)cx_set_word(&w->parents,i,0x80000000u|++w->networks);}
            else if(w->phase==CX_REMAP){if(!(value&0x80000000u))cx_set_word(&w->parents,i,cx_word(&w->parents,value));}
            else{value&=0x7fffffffu;uint32_t encoded=zigzag((int32_t)value-(int32_t)w->map_previous);w->map_previous=value;
                if(w->phase==CX_MAP_SIZE){uint32_t n=cx_var_size(encoded);if(n>UINT32_MAX-w->map_size)return TCW_MEMORY;w->map_size+=n;}
                else{uint8_t b[5];uint32_t n=cx_var_put(b,encoded);if(!cx_bytes_append(w,&w->map,b,n))return TCW_MEMORY;}
            }
        }
        if(w->phase_cursor<=w->labels_count)return TCW_CONTINUE;
        if(w->phase<CX_MAP_ENCODE){++w->phase;w->phase_cursor=1;w->map_previous=0;}
        else{cx_words_free(w,&w->parents);if(!prepare_count(w))return TCW_MEMORY;}
        return TCW_OK;
    }
    if(w->phase==CX_LAYOUT){
        while(*work&&w->phase_cursor<=w->networks){uint32_t g=w->phase_cursor++,n=cx_word(&w->code_ends,g),m=cx_word(&w->member_ends,g);--*work;
            if(n>w->max_group)w->max_group=n;if(n>UINT32_MAX-w->layout_code||m>UINT32_MAX-w->layout_member)return TCW_MEMORY;
            cx_set_word(&w->code_ends,g,w->layout_code);cx_set_word(&w->member_ends,g,w->layout_member);cx_set_word(&w->previous_gate,g,0);w->layout_code+=n;w->layout_member+=m;
        }
        if(w->phase_cursor<=w->networks)return TCW_CONTINUE;
        w->code_size=w->layout_code;w->member_size=w->layout_member;
        /* Member offsets follow the canonical-code interval in the same file. */
        for(uint32_t g=1;g<=w->networks;g++)cx_set_word(&w->member_ends,g,cx_word(&w->member_ends,g)+w->code_size);
        return prepare_store(w);
    }
    if(w->phase==CX_ZERO){
        if(w->output_offset<w->store_size){uint32_t n=w->store_size-w->output_offset;if(n>TERRA_CIRCUIT_WORLD_WINDOW)n=TERRA_CIRCUIT_WORLD_WINDOW;--*work;return cx_emit(w,TCW_WRITE,w->scratch_source,w->output_offset,n,w->output);}
        w->phase=CX_WRITE;cx_scan_reset(w);return TCW_OK;
    }
    if(w->phase==CX_FLUSH){int s=cx_cache_flush(w);if(s)return s;return prepare_intern(w);}
    if(w->phase==CX_INTERN)return intern_step(w,work);
    return cx_fail(w,TCW_STATE,"invalid circuit compiler phase");
}
