/* Sparse overlay controls travel as bounded JSON; cell records stay in an
 * immutable host source. Validate once, then replay 64 KiB windows during the
 * existing candidate writer. Cancellation never adopts this candidate. */
static int prepare_stamp(StreamTask* t,int length){
    int32_t values[6];const char* names[]={"x","y","width","height","recordCount","recordSourceId"};
    for(uint32_t i=0;i<6;i++){int p=json_find_key(t->request,length,names[i]);if(p<0||!json_extract_int(t->request,length,p,values+i)||values[i]<0)return 0;}
    char mode[16];int pos=json_find_key(t->request,length,"mode");if(pos<0||!json_extract_str(t->request,length,pos,mode,sizeof(mode))||strcmp(mode,"overlay"))return 0;
    uint32_t x=values[0],y=values[1],width=values[2],height=values[3],count=values[4],source=values[5];
    if(!width||!height||!count||count>1048576u||!source||source==t->source_id||x>=(uint32_t)t->candidate->maxTilesX||y>=(uint32_t)t->candidate->maxTilesY||width>(uint32_t)t->candidate->maxTilesX-x||height>(uint32_t)t->candidate->maxTilesY-y)return 0;
    StreamStamp* s=(StreamStamp*)tx_persistent_alloc(sizeof(*s));if(!s)return 0;memset(s,0,sizeof(*s));s->x=x;s->y=y;s->width=width;s->height=height;s->count=count;s->source_id=source;t->stamp=s;return 1;
}
/* 0=source requested, 1=record present, 2=end. The event range, lease pointer,
 * and legacy supply path all use the same independently owned stamp window. */
static int stamp_record(StreamTask* t,uint32_t* record){
    StreamStamp* s=t->stamp;if(s->index==s->count)return 2;uint32_t off=s->index*32u;
    if(off<s->offset||off-s->offset>=s->length||s->length-(off-s->offset)<32u){uint32_t n=s->count*32u-off;if(n>sizeof(s->bytes))n=sizeof(s->bytes);event(t,TX_STREAM_NEED_SOURCE,off,n,NULL);t->event.source_id=s->source_id;return 0;}
    for(uint32_t i=0;i<8;i++)record[i]=u32(s->bytes+off-s->offset+i*4);return 1;
}
static int stamp_validate_record(StreamTask* t,const uint32_t* r){
    StreamStamp* s=t->stamp;TxTile tile;if(r[0]>=s->width||r[1]>=s->height||!tx_tile_unrecord(r,&tile))return 0;
    if(s->has_previous&&(r[0]<s->previous_x||(r[0]==s->previous_x&&r[1]<=s->previous_y)))return 0;
    if(tile.active&&(tile.type>=t->candidate->tile_type_count||tile.type==0xfffeu||tx_tile_needs_section(tile.type)))return 0;
    /* Minecart.FrameTrack uses -1 for an absent back track (and its newly
     * placed front track). They are signed track IDs, not atlas coordinates. */
    if(tile.active&&tx_tile_framed(t->candidate,tile.type)&&(tile.frame_x<(tile.type==314?-1:0)||tile.frame_y<(tile.type==314?-1:0)))return 0;
    if(t->candidate->version<269u&&(tile.liquid_type==4||tile.invisible_block||tile.invisible_wall||tile.fullbright_block||tile.fullbright_wall))return 0;
    s->previous_x=r[0];s->previous_y=r[1];s->has_previous=1;return 1;
}
static int stamp_overlay(StreamTask* t,const uint32_t* r,TxTile* tile){
    TxTile source;if(!tx_tile_unrecord(r,&source))return 0;
    if(source.active){
        if(tile->active&&(tx_tile_framed(t->candidate,tile->type)||tx_tile_needs_section(tile->type))&&(tile->type!=source.type||tile->frame_x!=source.frame_x||tile->frame_y!=source.frame_y))return 0;
        tile->active=1;tile->type=source.type;tile->frame_x=source.frame_x;tile->frame_y=source.frame_y;tile->tile_color=source.tile_color;tile->brick_style=source.brick_style;
        tile->actuator=source.actuator;tile->inactive=source.inactive;tile->invisible_block=source.invisible_block;tile->fullbright_block=source.fullbright_block;
    }else if(source.actuator){tile->actuator=1;tile->inactive=source.inactive;}
    tile->wire_red|=source.wire_red;tile->wire_blue|=source.wire_blue;tile->wire_green|=source.wire_green;tile->wire_yellow|=source.wire_yellow;
    if(source.wall){tile->wall=source.wall;tile->wall_color=source.wall_color;tile->invisible_wall=source.invisible_wall;tile->fullbright_wall=source.fullbright_wall;}
    if(source.liquid_amount){tile->liquid_amount=source.liquid_amount;tile->liquid_type=source.liquid_type;}
    return 1;
}
