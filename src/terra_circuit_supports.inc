/* Placement anchors copied from the caller's verified TileObjectData layouts.
 * We retain sparse candidate anchor cells, not a second world grid. A bounded
 * cooperative replay selects one complete valid alternative. Framed supports
 * (e.g. a candle's table) become whole objects and resolve their own anchors. */
#include "terra_circuit_materials.h"
#include "terra_circuit_support_materials.h"
#include "terra_circuit_anchor_permissions.h"
enum { CF_SUPPORT_SOLID=1, CF_SUPPORT_FLOOR=2, CF_SUPPORT_TABLE=3,
       CF_SUPPORT_WALL=4, CF_SUPPORT_LAMP=5, CF_SUPPORT_SIDE=6,
       CF_SUPPORT_TREE=7, CF_SUPPORT_BOULDER=8, CF_SUPPORT_HOLDER=9,
       CF_SUPPORT_CHAIN_CHECK=10, CF_SUPPORT_CHAIN_CELL=11, CF_SUPPORT_SAND_HOLDER=12 };

static int cf_support_compare(const void* a,const void* b) {
    const CxFragmentSupport* x=a;const CxFragmentSupport* y=b;
    if(x->x!=y->x)return x->x<y->x?-1:1;
    if(x->y!=y->y)return x->y<y->y?-1:1;
    if(x->object!=y->object)return x->object<y->object?-1:1;
    return x->option<y->option?-1:x->option>y->option;
}
static int cf_support_cell(CxWorld* w,uint32_t object,uint32_t option,
        uint32_t x,uint32_t y,uint32_t kind) {
    CxFragments* f=w->fragments;CxFragmentObject* o=f->objects+object;
    o->support_options|=1u<<option;
    /* World dimensions are positive int32 values, and object spans <=255.
     * A -1 neighbor wraps to UINT32_MAX and fails this check; valid positions
     * need no i64 coordinates or conversions in the Wasm32 hot path. */
    if(x>=w->width||y>=w->height) {
        o->support_bad|=1u<<option;return TCW_OK;
    }
    if(f->support_count==f->support_capacity) {
        uint32_t capacity=f->support_capacity?f->support_capacity*2u:128u;
        if(capacity<f->support_capacity||(uint64_t)capacity*sizeof(CxFragmentSupport)>UINT32_MAX)return TCW_MEMORY;
        CxFragmentSupport* p=cx_alloc(w,capacity*sizeof(*p));if(!p)return TCW_MEMORY;
        if(f->support_count)memcpy(p,f->supports,f->support_count*sizeof(*p));
        cx_free(w,f->supports);f->supports=p;f->support_capacity=capacity;
    }
    CxFragmentSupport* s=f->supports+f->support_count++;memset(s,0,sizeof(*s));
    s->x=(uint32_t)x;s->y=(uint32_t)y;s->object=object;s->option=option;s->kind=kind;
    return TCW_OK;
}
static int cf_support_line(CxWorld* w,uint32_t object,uint32_t option,
        uint32_t x,uint32_t y,uint32_t count,uint32_t dx,uint32_t dy,uint32_t kind) {
    for(uint32_t i=0;i<count;i++) {
        int s=cf_support_cell(w,object,option,x+i*dx,y+i*dy,kind);
        if(s)return s;
    }
    return TCW_OK;
}
static int cf_support_wall(CxWorld* w,uint32_t object,uint32_t option) {
    CxFragmentObject o=w->fragments->objects[object];
    for(uint32_t x=0;x<o.width;x++) {
        int s=cf_support_line(w,object,option,o.x+x,o.y,o.height,0,1,CF_SUPPORT_WALL);
        if(s)return s;
    }
    return TCW_OK;
}
static int cf_support_object_prepare(CxWorld* w,uint32_t object) {
    CxFragmentObject o=w->fragments->objects[object];
    uint32_t x=o.x,y=o.y;int s=0;
    /* A wired falling block is itself a mechanism payload. Its existing
     * vertical support chain is discovered in one already-decoded column. */
    if(cx_support_properties(o.type)&CX_SUPPORT_FALLING)
        return cf_support_cell(w,object,0,x,y,CF_SUPPORT_CHAIN_CHECK);
    /* Boulders remain held by any surviving foot or a saved holder above.
     * Retain every valid hold, since dropping one changes an actuator trap. */
    if(o.anchor==1u&&cx_material_has(o.type,CX_MATERIAL_BOULDER)&&o.width==2u){
        s=cf_support_cell(w,object,0,x,y+o.height,CF_SUPPORT_BOULDER);if(s)return s;
        s=cf_support_cell(w,object,1,x+1,y+o.height,CF_SUPPORT_BOULDER);if(s)return s;
        s=cf_support_cell(w,object,2,x,y-1,CF_SUPPORT_HOLDER);if(s)return s;
        return cf_support_cell(w,object,3,x+1,y-1,CF_SUPPORT_HOLDER);
    }
    switch(o.anchor) {
        case 0:return TCW_OK;
        case 1:return cf_support_line(w,object,0,x,y+o.height,o.width,1,0,CF_SUPPORT_FLOOR);
        case 2:return cf_support_line(w,object,0,x,y-1,o.width,1,0,CF_SUPPORT_SOLID);
        case 3:
            s=cf_support_line(w,object,0,x,y-1,o.width,1,0,CF_SUPPORT_SOLID);
            if(s)return s;
            return cf_support_line(w,object,0,x,y+o.height,o.width,1,0,CF_SUPPORT_SOLID);
        case 4:return cf_support_cell(w,object,0,x+o.width/2u,y-1,CF_SUPPORT_SOLID);
        case 5:
            s=cf_support_cell(w,object,0,x,y+o.height,CF_SUPPORT_FLOOR);if(s)return s;
            s=cf_support_cell(w,object,1,x-1,y,CF_SUPPORT_SIDE);if(s)return s;
            s=cf_support_cell(w,object,2,x+o.width,y,CF_SUPPORT_SIDE);if(s)return s;
            s=cf_support_wall(w,object,3);if(s)return s;
            s=cf_support_line(w,object,4,x-1,y-1,3,0,1,CF_SUPPORT_TREE);if(s)return s;
            return cf_support_line(w,object,5,x+o.width,y-1,3,0,1,CF_SUPPORT_TREE);
        case 6:return cf_support_wall(w,object,0);
        case 7:
            s=cf_support_line(w,object,0,x,y+o.height,o.width,1,0,CF_SUPPORT_FLOOR);
            return s?s:cf_support_wall(w,object,1);
        case 8:return cf_support_cell(w,object,0,x,y+o.height,CF_SUPPORT_LAMP);
        case 9:case 10:
            if(o.anchor==10)x+=o.width-1u;
            s=cf_support_cell(w,object,0,x,y-1,CF_SUPPORT_SOLID);if(s)return s;
            return cf_support_cell(w,object,0,x,y+o.height,CF_SUPPORT_SOLID);
        case 11:case 12:
            if(o.anchor==12)y++;
            s=cf_support_cell(w,object,0,x-1,y,CF_SUPPORT_SOLID);if(s)return s;
            return cf_support_cell(w,object,0,x+o.width,y,CF_SUPPORT_SOLID);
        case 13: {
            uint32_t direction=(o.frame&65535u)/22u%3u;
            uint32_t side=direction==1?x-1:direction==2?x+o.width:x;
            s=cf_support_cell(w,object,0,side,direction?y:y+o.height,direction?CF_SUPPORT_SIDE:CF_SUPPORT_FLOOR);if(s)return s;
            s=cf_support_wall(w,object,1);if(s||!direction)return s;
            return cf_support_line(w,object,2,side,y-1,3,0,1,CF_SUPPORT_TREE);
        }
        case 14:return cf_support_line(w,object,0,x,y+o.height,o.width,1,0,CF_SUPPORT_TABLE);
        case 15:case 16: {
            uint32_t side=o.anchor==15?x-1:x+o.width;
            s=cf_support_line(w,object,0,side,y,o.height,0,1,o.type==442u?CF_SUPPORT_SIDE:CF_SUPPORT_SOLID);
            if(s||o.type!=442u)return s;
            return cf_support_line(w,object,1,side,y-1,3,0,1,CF_SUPPORT_TREE);
        }
        case 17:
            if(o.width<2)return TCW_INVALID;
            return cf_support_line(w,object,0,x+(o.width-2u)/2u,y+o.height,2,1,0,CF_SUPPORT_FLOOR);
        default:return TCW_INVALID;
    }
}
static int cf_support_prepare(CxWorld* w) {
    CxFragments* f=w->fragments;
    if(++f->support_round>32u)return cx_fail(w,TCW_UNSUPPORTED,"placement support dependency exceeds 32 object layers");
    while(f->support_prepared<f->object_count) {
        int status=cf_support_object_prepare(w,f->support_prepared++);if(status)return status;
    }
    if(f->support_count>1)qsort(f->supports,f->support_count,sizeof(*f->supports),cf_support_compare);
    f->support_cursor=0;f->support_scan_count=f->support_count;
    return TCW_OK;
}
static int cf_sand_holder(uint32_t type) {
    /* WorldGen.AllowsSandfall, including inactive but active holder tiles. */
    return type==21u||type==467u||type==441u||type==468u||type==323u||type==88u||
        type==80u||type==77u||type==26u||type==475u||type==470u||type==597u;
}
static int cf_support_valid(const CxFragmentSupport* support,const TxTile* t,const CxFragmentObject* owner) {
    if(support->kind==CF_SUPPORT_WALL)return t->wall!=0;
    if(support->kind==CF_SUPPORT_CHAIN_CHECK)return t->active&&(cx_support_properties(t->type)&CX_SUPPORT_FALLING);
    if(support->kind==CF_SUPPORT_SAND_HOLDER)return t->active&&cf_sand_holder(t->type);
    if(support->kind==CF_SUPPORT_HOLDER)return t->active&&(t->type==21u||t->type==467u||t->type==441u||t->type==468u||t->type==88u||t->type==470u||t->type==475u);
    if(!t->active||t->inactive)return 0;
    if(support->kind==CF_SUPPORT_CHAIN_CELL)return 1;
    if(support->kind==CF_SUPPORT_LAMP)return t->type==419u||t->type==420u;
    if(support->kind==CF_SUPPORT_TREE)return cx_material_has(t->type,CX_MATERIAL_TREE_TRUNK);
    uint32_t properties=cx_support_properties(t->type);
    int solid=cx_material_has(t->type,CX_MATERIAL_SOLID)||(properties&CX_SUPPORT_PLATFORM),top=(properties&CX_SUPPORT_SOLID_TOP)!=0;
    int real=!cx_material_has(t->type,CX_MATERIAL_NOT_REALLY_SOLID),attach=!(properties&CX_SUPPORT_NO_ATTACH);
    int platform=(properties&CX_SUPPORT_PLATFORM)!=0,half=t->brick_style==1u;
    int top_slope=t->brick_style==2u||t->brick_style==3u,bottom_slope=t->brick_style==4u||t->brick_style==5u;
    /* Natural speleothems use WorldGen.CheckStalactite, not TileObjectData.
     * Generic glass is solid but cannot preserve their biome/material style. */
    if(owner->type==165u)return solid&&!top&&t->brick_style==0&&cx_stalactite_material(t->type,owner->height);
    /* WorldGen.PlatformProperTopFrame, not an atlas position. */
    int frame=t->frame_x/18;
    int proper=(frame>=0&&frame<=7)||(frame>=12&&frame<=16)||(frame>=25&&frame<=26);
    int table=(platform&&!half&&proper)||(!platform&&(properties&CX_SUPPORT_TABLE)&&t->brick_style==0);
    if(support->kind==CF_SUPPORT_TABLE)return table;
    if(support->kind==CF_SUPPORT_BOULDER)return (solid||top)&&!half&&(!top_slope||(platform&&proper));
    if(support->kind==CF_SUPPORT_SIDE){
        if(properties&CX_SUPPORT_BEAM)return 1;
        int slope_ok=t->brick_style==0||(support->x<owner->x?(t->brick_style==3u||t->brick_style==5u):(t->brick_style==2u||t->brick_style==4u));
        return solid&&!top&&real&&attach&&!half&&slope_ok;
    }
    if(support->kind==CF_SUPPORT_FLOOR){
        uint32_t allowed=cx_anchor_bottom_type(owner->type);
        if(owner->type==442u)return solid&&(attach||platform)&&!half&&!top_slope;
        if((allowed&CX_ANCHOR_TABLE)&&table)return 1;
        if((allowed&CX_ANCHOR_SOLID_WITH_TOP)&&((platform&&!half&&proper)||(solid&&top&&!platform)))return 1;
        if((allowed&CX_ANCHOR_SOLID_SIDE)&&solid&&!top&&bottom_slope)return 1;
        /* Custom torch/switch checks allow a proper platform top. */
        if((owner->type==4u||owner->type==136u||owner->type==442u)&&platform&&!half&&proper)return 1;
        return (allowed&CX_ANCHOR_SOLID_TILE)&&solid&&!top&&attach&&real&&(cx_anchor_flattens(owner->type)||t->brick_style==0);
    }
    if(support->y<owner->y){
        uint32_t allowed=cx_anchor_top_type(owner->type);
        if(owner->type==442u)return solid&&(attach||(platform&&half))&&!bottom_slope;
        if(platform&&((allowed&CX_ANCHOR_PLATFORM)||((allowed&CX_ANCHOR_PLATFORM_NON_HAMMERED)&&t->brick_style==0)))return 1;
        if((allowed&CX_ANCHOR_SOLID_BOTTOM)&&real&&!bottom_slope&&((solid&&!top)||half||top_slope))return 1;
        if((allowed&CX_ANCHOR_SOLID_SIDE)&&solid&&!top&&top_slope)return 1;
    }
    return solid&&!top&&real&&attach&&(cx_anchor_flattens(owner->type)||t->brick_style==0);
}
static int cf_support_chosen(const CxFragmentObject* o,uint32_t option){
    if(o->anchor==1u&&cx_material_has(o->type,CX_MATERIAL_BOULDER))return ((o->support_options&~o->support_bad)&(1u<<option))!=0;
    return o->support_choice==option;
}
static int cf_chain_cell(CxWorld* w,uint32_t object,uint32_t option,uint32_t y,uint32_t kind) {
    int status=cf_support_cell(w,object,option,w->x,y,kind);if(status)return status;
    CxFragmentSupport* added=w->fragments->supports+w->fragments->support_count-1u;
    added->tile=w->column[y];added->chain_checked=1;
    return TCW_OK;
}
static int cf_falling_chain(CxWorld* w,uint32_t object,uint32_t option,uint32_t y) {
    for(;;){
        int held_above=y&&w->column[y-1u].active&&cf_sand_holder(w->column[y-1u].type);
        if(held_above){int s=cf_chain_cell(w,object,option,y-1u,CF_SUPPORT_SAND_HOLDER);if(s)return s;}
        /* BlockBelowMakesSandFall: a non-solid tile only holds sand when the
         * tile below it is also nactive. Stalactites never block sandfall. */
        if(y+1u>=w->height){if(!held_above)w->fragments->objects[object].support_warning|=1u<<option;return TCW_OK;}
        TxTile* below=w->column+y+1u;
        int next_active=below->active&&!below->inactive;
        int solid=cx_material_has(below->type,CX_MATERIAL_SOLID)||(cx_support_properties(below->type)&CX_SUPPORT_PLATFORM);
        int second_active=y+2u<w->height&&w->column[y+2u].active&&!w->column[y+2u].inactive;
        if(!next_active||below->type==165u||(!solid&&!second_active)){
            if(!held_above)w->fragments->objects[object].support_warning|=1u<<option;
            return TCW_OK;
        }
        int status=cf_chain_cell(w,object,option,y+1u,CF_SUPPORT_CHAIN_CELL);if(status)return status;
        if(cx_support_properties(below->type)&CX_SUPPORT_FALLING){y++;continue;}
        if(!solid){
            status=cf_chain_cell(w,object,option,y+2u,CF_SUPPORT_CHAIN_CELL);if(status)return status;
            if(cx_support_properties(w->column[y+2u].type)&CX_SUPPORT_FALLING){y+=2u;continue;}
        }
        return TCW_OK;
    }
}
static int cf_support_scan(CxWorld* w,uint32_t y) {
    CxFragments* f=w->fragments;
    while(f->support_cursor<f->support_scan_count) {
        CxFragmentSupport* support=f->supports+f->support_cursor;
        if(support->x!=w->x||support->y!=y)break;
        support->tile=w->column[y];
        int valid=cf_support_valid(support,&support->tile,f->objects+support->object);
        if(!valid)f->objects[support->object].support_bad|=1u<<support->option;
        uint32_t object=support->object,option=support->option;
        int chain=valid&&!support->chain_checked&&support->kind!=CF_SUPPORT_WALL&&!support->tile.inactive&&(cx_support_properties(support->tile.type)&CX_SUPPORT_FALLING);
        support->chain_checked=1;
        f->support_cursor++;
        if(chain){int status=cf_falling_chain(w,object,option,y);if(status)return status;}
    }
    return TCW_OK;
}
static int cf_support_resolve(CxWorld* w) {
    CxFragments* f=w->fragments;
    for(uint32_t i=0;i<f->object_count;i++) {
        CxFragmentObject* o=f->objects+i;uint32_t valid=o->support_options&~o->support_bad;
        o->support_choice=UINT32_MAX;
        uint32_t preferred=valid&~o->support_warning;if(!preferred)preferred=valid;
        for(uint32_t option=0;option<6u;option++)if(preferred&(1u<<option)){o->support_choice=option;break;}
        if(o->support_options&&o->support_choice==UINT32_MAX)o->flags|=8u;
        for(uint32_t option=0;option<6u;option++)if((o->support_warning&(1u<<option))&&cf_support_chosen(o,option))o->flags|=8u;
    }
    /* Resolve only complete chosen alternatives. An unused side tile cannot
     * pull a decorative table or chest into an otherwise independent circuit. */
    for(uint32_t i=0;i<f->support_count;i++) {
        CxFragmentSupport* support=f->supports+i;
        CxFragmentObject owner=f->objects[support->object];
        if(!cf_support_chosen(&owner,support->option)||support->kind==CF_SUPPORT_WALL||support->kind==CF_SUPPORT_LAMP||support->kind==CF_SUPPORT_TREE)continue;
        TxTile* t=&support->tile;
        if(!tx_tile_framed(w->world,t->type)||cf_known_single(t->type))continue;
        const CxGeometry* g=cf_geometry(f,t);
        if(!g) { f->objects[support->object].flags|=1u;continue; }
        uint32_t dx=g->shape&255u,dy=g->shape>>8&255u,width=g->shape>>16&255u,height=g->shape>>24;
        if(dx>support->x||dy>support->y||width>w->width-(support->x-dx)||height>w->height-(support->y-dy)) {
            f->objects[support->object].flags|=1u;continue;
        }
        int status=cf_object(w,support->x-dx,support->y-dy,width,height,t->type,owner.net,
            tx_tile_needs_section(t->type)?2u:0u,g->anchor,(uint16_t)t->frame_x|((uint32_t)(uint16_t)t->frame_y<<16));
        if(status)return status;
    }
    return TCW_OK;
}
static void cf_support_finish(CxWorld* w) {
    CxFragments* f=w->fragments;
    /* Column-local falling chains append cached cells beyond the scanned
     * prefix; sort them once before descriptor and extraction replays. */
    if(f->support_count>1)qsort(f->supports,f->support_count,sizeof(*f->supports),cf_support_compare);
    for(uint32_t i=0;i<f->support_count;i++) {
        CxFragmentSupport* s=f->supports+i;CxFragmentObject* o=f->objects+s->object;
        s->net=o->net;s->selected=cf_support_chosen(o,s->option);
    }
    f->support_cursor=0;
}
static void cf_filter_tile(TxTile* t,uint32_t layers) {
    if(!(layers&1u)) {
        /* An unattached actuator is still a circuit layer for sparse overlay. */
        uint8_t actuator=t->actuator;
        t->type=0;t->active=0;t->frame_x=t->frame_y=0;t->inactive=0;
        t->tile_color=t->brick_style=t->invisible_block=t->fullbright_block=0;
        t->actuator=actuator;
    }
    if(!(layers&2u))t->wall=t->wall_color=t->invisible_wall=t->fullbright_wall=0;
    t->liquid_amount=t->liquid_type=0;
}
