/* Circuit-only Portal Gun Station / Projectile 601 transport.
 * Source: TerrariaDecompiledSource 8255d346; Wiring.HitWireSingle(209),
 * WorldGen.ShootFromCannon, Projectile.Update/HandleMovement and
 * Collision.SwitchTiles. Entity damage, portals and visual particles are
 * deliberately outside this circuit transport. Unsupported collision shapes
 * fail the atomic operation rather than silently pretending to simulate them.
 */
#define CX_PROJECTILES 1000u
static int projectile_index_column(CxWorld* w) {
    if (!w->solid_columns) {
        if(w->width>UINT32_MAX/4u-1u)return TCW_MEMORY;
        w->solid_columns=cx_alloc(w,(w->width+1u)*4u);
        w->projectiles=cx_alloc(w,CX_PROJECTILES*sizeof(CxProjectile));
        w->projectile_snapshot=cx_alloc(w,CX_PROJECTILES*sizeof(CxProjectile));
        if(!w->solid_columns||!w->projectiles||!w->projectile_snapshot)return TCW_MEMORY;
        memset(w->projectiles,0,CX_PROJECTILES*sizeof(CxProjectile));
    }
    w->solid_columns[w->x]=w->solid_count;
    for(uint32_t y=0;y<w->height;y++) {
        TxTile* t=w->column+y;
        if(!t->active||!cx_material_has(t->type,CX_MATERIAL_SOLID)||
           (cx_support_properties(t->type)&CX_SUPPORT_SOLID_TOP)||
           (t->inactive&&!t->actuator&&t->type!=131u))continue;
        uint8_t shape=(uint8_t)(t->brick_style+1u),dynamic=t->actuator||t->type==130u||t->type==131u;
        if(w->solid_count>w->solid_columns[w->x]) {
            CxSolidSpan* last=w->solid_spans+w->solid_count-1u;
            if(last->last==y&&last->shape==shape&&last->dynamic==dynamic){last->last=y+1u;continue;}
        }
        if(!grow(w,(void**)&w->solid_spans,&w->solid_capacity,w->solid_count+1u,sizeof(CxSolidSpan)))return TCW_MEMORY;
        w->solid_spans[w->solid_count++]=(CxSolidSpan){y,y+1u,shape,dynamic,0};
    }
    w->solid_columns[w->x+1u]=w->solid_count;
    return TCW_OK;
}
static uint32_t projectile_slot(CxWorld* w) {
    uint32_t oldest=0;
    for(uint32_t i=0;i<CX_PROJECTILES;i++) {
        if(!w->projectiles[i].active)return i;
        if(w->projectiles[i].remaining<w->projectiles[oldest].remaining)oldest=i;
    }
    /* Both supported types have netImportant=false. */
    return oldest;
}
static int portal_spawn(CxWorld* w,uint32_t x,uint32_t y,int angle) {
    static const float xs[9]={10,7.5f,5,2.75f,0,-2.75f,-5,-7.5f,-10};
    static const float ys[9]={0,-2.5f,-5,-6,-10,-6,-5,-2.5f,0};
    if(angle<0||angle>8)return cx_fail(w,TCW_UNSUPPORTED,"portal station angle is invalid");
    {uint32_t i=projectile_slot(w);
        float scale=3.0f/sqrtf(xs[angle]*xs[angle]+ys[angle]*ys[angle]);
        if(i>=w->projectile_limit)w->projectile_limit=i+1u;
        /* NewProjectile receives the centre and subtracts half its 10x10 size. */
        w->projectiles[i]=(CxProjectile){(float)((x+2u)*16u)+(angle==4?5.0f:0.0f)-5.0f,
            (float)((y+2u)*16u),xs[angle]*scale,ys[angle]*scale,0,0,3600u,601u,1,0};
        return TCW_OK;
    }
}
/* Spear Trap: Wiring type 137 style 4, Projectile 186 / AI 37. */
static int trap_wire_hit(CxWorld* w,CxDevice* d) {
    if(cx_seed_contains(w,d->x,d->y)||(w->vm_source_gate&&d->gate==w->vm_source_gate))return TCW_OK;
    if(d->tile.frame_y/18!=4)return cx_fail(w,TCW_UNSUPPORTED,"this trap projectile type is not supported by circuit transport");
    int dir=d->tile.frame_x/18,dx=dir==3?-1:dir==4?1:0,dy=dir<=1?1:dir==2?-1:0;
    if(!dx&&!dy)return TCW_UNSUPPORTED;
    int registered=check_mech(w,d,90u);if(registered<0)return TCW_MEMORY;if(!registered)return TCW_OK;
    {uint32_t i=projectile_slot(w);
        float cx=(float)(d->x*16u+8u)+18.0f*dx,cy=(float)(d->y*16u+8u)+18.0f*dy;
        w->projectiles[i]=(CxProjectile){cx-5,cy-7,8.0f*dx,8.0f*dy,cx-12.0f*dx,cy-12.0f*dy,300u,186u,1,0};
        if(i>=w->projectile_limit)w->projectile_limit=i+1u;
    }return TCW_OK;
}
static int portal_wire_hit(CxWorld* w,CxDevice* d,uint32_t colour) {
    if(cx_seed_contains(w,d->x,d->y)||(w->vm_source_gate&&d->gate==w->vm_source_gate))return TCW_OK;
    int partX=d->tile.frame_x%72/18,partY=d->tile.frame_y%54/18;
    if(partX<0||partY<0||d->x<(uint32_t)partX||d->y<(uint32_t)partY)return TCW_UNSUPPORTED;
    uint32_t x=d->x-(uint32_t)partX,y=d->y-(uint32_t)partY;
    CxDevice* base=cx_device_find(w,x,y);if(!base)return TCW_UNSUPPORTED;
    uint32_t bit=1u<<colour;if(base->wire_hit_mask&bit)return TCW_OK;
    int angle=d->tile.frame_y/54,style=d->tile.frame_x/72;
    int row=(partX==1||partX==2)?partY:-1;
    int turn=partX==0?54:partX==3?-54:0;
    if((angle>=8&&turn>0)||(angle==0&&turn<0))turn=0;
    int flip=(style==3||style==4)&&(row==0||row==1)?(style==3?72:-72):0;
    if(turn||flip){base->wire_hit_mask|=bit;for(uint32_t a=x;a<x+4u;a++)for(uint32_t b=y;b<y+3u;b++){
        CxDevice* part=cx_device_find(w,a,b);if(part&&part->tile.type==209u){part->tile.frame_x=(int16_t)(part->tile.frame_x+flip);part->tile.frame_y=(int16_t)(part->tile.frame_y+turn);}
    }}
    if(row<0)return TCW_OK;
    if(style!=3&&style!=4)return cx_fail(w,TCW_UNSUPPORTED,"this cannon projectile type is not supported by circuit transport");
    /* CheckMech happens even for the style-changing top/middle cells. */
    int registered=check_mech(w,base,30u);if(registered<0)return TCW_MEMORY;
    return registered&&row>=2?portal_spawn(w,x,y,angle):TCW_OK;
}
static int intersect(float ax,float ay,float aw,float ah,float bx,float by,float bw,float bh) {
    return ax<bx+bw&&ax+aw>bx&&ay<by+bh&&ay+ah>by;
}
/* Collision.SwitchTiles uses Utils.FloatIntersect: touching an edge counts.
 * SolidCollision above/below continues to require positive-area overlap. */
static int pad_intersect(float ax,float ay,float aw,float ah,float bx,float by,float bw,float bh) {
    return ax<=bx+bw&&ax+aw>=bx&&ay<=by+bh&&ay+ah>=by;
}
static int solid_shape(CxWorld* w,int32_t x,int32_t y) {
    if(x<0||y<0||(uint32_t)x>=w->width||(uint32_t)y>=w->height)return 1;
    uint32_t lo=w->solid_columns[x],hi=w->solid_columns[x+1];
    while(lo<hi){uint32_t m=lo+(hi-lo)/2u;CxSolidSpan* s=w->solid_spans+m;if(s->last<=(uint32_t)y)lo=m+1u;else hi=m;}
    if(lo>=w->solid_columns[x+1]||w->solid_spans[lo].first>(uint32_t)y)return 0;
    CxSolidSpan* s=w->solid_spans+lo;
    if(s->dynamic){CxDevice* d=cx_device_find(w,(uint32_t)x,(uint32_t)y);if(d&&(d->tile.inactive||d->tile.type==131u))return 0;}
    return s->shape;
}
static int spear_ai(CxWorld* w,CxProjectile* p) {
    int solid=0;
    for(int32_t x=(int32_t)(p->x/16.0f);x<=(int32_t)((p->x+10)/16.0f);x++)
    for(int32_t y=(int32_t)(p->y/16.0f);y<=(int32_t)((p->y+14)/16.0f);y++) {
        int shape=solid_shape(w,x,y);if(!shape)continue;
        if(intersect(p->x,p->y,10,14,x*16.0f,y*16.0f+(shape==2?8:0),16,shape==2?8:16))solid=1;
    }
    float dx=p->x+5-p->origin_x,dy=p->y+7-p->origin_y,dist=sqrtf(dx*dx+dy*dy);
    if(!p->returning){if(solid||dist>300){p->vx=-p->vx;p->vy=-p->vy;p->returning=1;}}
    else if(solid||dist<sqrtf(p->vx*p->vx+p->vy*p->vy))p->active=0;
    return TCW_OK;
}
static int projectile_move(CxWorld* w,CxProjectile* p) {
    if(p->type==186u){spear_ai(w,p);p->x+=p->vx;p->y+=p->vy;return TCW_OK;}
    float dx=p->vx,dy=p->vy;
    int32_t x0=(int32_t)(p->x/16.0f)-1,y0=(int32_t)(p->y/16.0f)-1;
    int32_t x1=(int32_t)((p->x+10.0f)/16.0f)+2,y1=(int32_t)((p->y+10.0f)/16.0f)+2;
    for(int32_t x=x0;x<x1;x++)for(int32_t y=y0;y<y1;y++) {
        int shape=solid_shape(w,x,y);if(!shape)continue;
        float tx=(float)(x*16),ty=(float)(y*16)+(shape==2?8.0f:0.0f),height=shape==2?8.0f:16.0f;
        if(!intersect(p->x+p->vx,p->y+p->vy,10,10,tx,ty,16,height))continue;
        if(shape>2)return cx_fail(w,TCW_UNSUPPORTED,"projectile collision with slopes is not supported by circuit transport");
        if(p->y+10.0f<=ty&&p->vy>0){float v=ty-p->y-10.0f;if(v<dy)dy=v;}
        else if(p->x+10.0f<=tx&&p->vx>0){float v=tx-p->x-10.0f;if(v<dx)dx=v;}
        else if(p->x>=tx+16.0f&&p->vx<0){float v=tx+16.0f-p->x;if(v>dx)dx=v;}
        else if(p->y>=ty+height&&p->vy<0){float v=ty+height-p->y;if(v>dy)dy=v;}
    }
    p->x+=dx;p->y+=dy;
    if(dx!=p->vx||dy!=p->vy)p->active=0;
    return TCW_OK;
}
static CxDevice* projectile_pad(CxWorld* w,CxProjectile* p,float ox,float oy) {
    float height=p->type==186u?14.0f:10.0f;
    int32_t x0=(int32_t)(p->x/16.0f)-1,y0=(int32_t)(p->y/16.0f)-1;
    int32_t x1=(int32_t)((p->x+10.0f)/16.0f)+2,y1=(int32_t)((p->y+height)/16.0f)+2;
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>(int32_t)w->width)x1=(int32_t)w->width;
    if(y1>(int32_t)w->height-40)y1=(int32_t)w->height-40;
    for(int32_t x=x0;x<x1;x++)for(int32_t y=y0;y<y1;y++) {
        CxDevice* d=cx_device_find(w,(uint32_t)x,(uint32_t)y);
        if(!d||!d->tile.active||d->tile.type!=442u)continue;
        int style=d->tile.frame_x/22;float tx=(float)(x*16),ty=(float)(y*16),tw=16,th=16;
        if(style==0){ty+=6;th=10;}else if(style==1)th=10;else if(style==2)tw=10;else if(style==3){tx+=6;tw=10;}else continue;
        if(pad_intersect(tx,ty,tw,th,p->x,p->y,10,height)&&!pad_intersect(tx,ty,tw,th,ox,oy,10,height))return d;
    }
    return NULL;
}
static int projectiles_step(CxWorld* w,uint32_t* work) {
    if(w->tick_stage==4u){int s=terra_vm_step(w->vm,*work);*work=0;
        if(s<0)return cx_fail(w,TCW_STATE,"projectile circuit pulse failed and was rolled back");
        if(s==TERRA_VM_MORE)return TCW_CONTINUE;w->tick_stage=3;}
    while(*work&&w->projectile_cursor<w->projectile_limit) {
        CxProjectile* p=w->projectiles+w->projectile_cursor;
        if(!p->active||w->projectile_substep>=(p->type==186u?1u:31u)){++w->projectile_cursor;w->projectile_substep=0;--*work;continue;}
        --*work;++w->projectile_substep;float ox=p->x,oy=p->y;
        int s=projectile_move(w,p);if(s<0)return s;
        CxDevice* pad=projectile_pad(w,p,ox,oy);
        if(p->remaining&&!--p->remaining)p->active=0;
        if(pad){w->trigger_count=0;for(uint32_t c=0;c<4;c++)if(pad->pulse_nets[c])w->trigger_nets[w->trigger_count++]=pad->pulse_nets[c]-1u;
            w->seed_x=pad->x;w->seed_y=pad->y;w->seed_width=w->seed_height=1;
            uint32_t avail=cx_vm_available(w);if(!avail||terra_vm_set_budget(w->vm,avail)<0||terra_vm_begin(w->vm,w->trigger_nets,w->trigger_count)<0)return TCW_MEMORY;
            w->tick_stage=4;return TCW_CONTINUE;
        }
    }
    if(w->projectile_cursor<w->projectile_limit)return TCW_CONTINUE;
    while(w->projectile_limit&&!w->projectiles[w->projectile_limit-1u].active)--w->projectile_limit;
    return TCW_OK;
}
