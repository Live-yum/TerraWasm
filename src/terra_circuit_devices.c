#include "terra_circuit_world_internal.h"
#include "terra_circuit_actuation.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Wiring.CheckMech uses one shared 999-position limit, independent of world
 * size and the number of wire-connected devices. A full list rejects only the
 * new registration; HitSwitch still changes the timer/button's visible frame. */
#define CX_MECH_LIMIT 999u

static int grow(CxWorld* w,void** data,uint32_t* capacity,uint32_t count,uint32_t size){if(count<=*capacity)return 1;uint32_t n=*capacity?*capacity*2u:64u;while(n<count){if(n>UINT32_MAX/2u)return 0;n*=2u;}if((uint64_t)n*size>UINT32_MAX)return 0;void* p=cx_alloc(w,n*size);if(!p)return 0;if(*data)memcpy(p,*data,*capacity*size);cx_free(w,*data);*data=p;*capacity=n;return 1;}
static int is_switch(uint32_t type){switch(type){case 132:case 135:case 136:case 144:case 314:case 411:case 423:case 428:case 440:case 441:case 442:case 467:case 468:case 476:return 1;default:return 0;}}
int cx_devices_prepare(CxWorld* w){for(uint32_t i=0;i<2;i++){w->previous_columns[i]=(TxTile*)cx_alloc(w,w->height*sizeof(TxTile));if(!w->previous_columns[i])return TCW_MEMORY;}return TCW_OK;}
static int32_t policy_reader(void* context,uint32_t x,uint32_t y,TxTile* out){CxActuationPolicy* p=(CxActuationPolicy*)context;int64_t dx=(int64_t)x-p->x+2,dy=(int64_t)y-p->y+2;if(dx<0||dx>=5||dy<0||dy>=2)return 0;uint32_t i=(uint32_t)dy*5u+(uint32_t)dx;if(!(p->valid&(1u<<i)))return 0;*out=p->above[i];return 1;}
static void fill_policy(CxWorld* w,CxActuationPolicy* p,uint32_t x,const TxTile* column){int64_t dx=(int64_t)x-p->x+2;if(dx<0||dx>=5)return;for(uint32_t k=0;k<2;k++){int64_t y=(int64_t)p->y-2+k;if(y>=0&&y<w->height){uint32_t i=k*5u+(uint32_t)dx;p->above[i]=column[y];p->valid|=1u<<i;}}}
static void finish_policy(CxWorld* w,CxActuationPolicy* p){CxDevice* d=w->devices+p->device;TxTile tile=d->tile;if(tile.type==131u)tile.type=130u;d->can_deactivate=cx_can_deactivate_with_reader(w,d->x,d->y,&tile,policy_reader,p);}
static int prepare_policy(CxWorld* w,CxDevice* d){TxTile tile=d->tile;if(tile.type==131u)tile.type=130u;int s=cx_can_deactivate(w,d->x,d->y,&tile);d->can_deactivate=s;if(s!=TCW_UNSUPPORTED)return s<0?s:TCW_OK;
    if(!grow(w,(void**)&w->policies,&w->policy_capacity,w->policy_count+1u,sizeof(CxActuationPolicy)))return TCW_MEMORY;
    CxActuationPolicy* p=w->policies+w->policy_count++;memset(p,0,sizeof(*p));p->device=(uint32_t)(d-w->devices);p->x=d->x;p->y=d->y;
    for(uint32_t back=0;back<=2u&&back<=w->x;back++){uint32_t x=w->x-back;fill_policy(w,p,x,back?w->previous_columns[x&1u]:w->column);}return TCW_OK;
}
int cx_devices_column(CxWorld* w){
    for(uint32_t i=0;i<w->policy_count;){CxActuationPolicy* p=w->policies+i;fill_policy(w,p,w->x,w->column);if(w->x>=p->x+2u||w->x+1u==w->width){finish_policy(w,p);w->policies[i]=w->policies[--w->policy_count];}else ++i;}
    w->column_device_first=w->device_count;
    for(uint32_t y=0;y<w->height;y++){TxTile* t=w->column+y;if(!t->active&&!t->actuator)continue;
        if(t->type==445u||t->type==0xfffeu){if(!grow(w,(void**)&w->pixels,&w->pixel_capacity,w->pixel_count+1u,sizeof(CxPixel)))return TCW_MEMORY;CxPixel* p=w->pixels+w->pixel_count;memset(p,0,sizeof(*p));p->x=w->x;p->y=y;p->custom=t->type==0xfffeu;p->state=p->initial=p->custom?(uint8_t)((t->frame_x/18)&3u)|((uint8_t)((t->frame_y/18)&3u)<<2):(uint8_t)(t->frame_x==18);w->pixel_at_y[y]=++w->pixel_count;}
        if(is_switch(t->type)||t->actuator||t->type==130u||t->type==131u){if(!grow(w,(void**)&w->devices,&w->device_capacity,w->device_count+1u,sizeof(CxDevice)))return TCW_MEMORY;CxDevice* d=w->devices+w->device_count++;memset(d,0,sizeof(*d));d->x=w->x;d->y=y;d->tile=d->initial=*t;if(t->type==144u)d->tile.frame_y=0;if(t->actuator||t->type==130u||t->type==131u){int s=prepare_policy(w,d);if(s)return s;}}
    }return TCW_OK;
}
void cx_devices_bind_column(CxWorld* w){for(uint32_t i=w->column_device_first;i<w->device_count;i++){CxDevice* d=w->devices+i;for(uint32_t c=0;c<4;c++)d->pulse_nets[c]=d->nets[c]=w->ids[d->y*4u+c];d->gate=w->gate_at_y[d->y];}memcpy(w->previous_columns[w->x&1u],w->column,w->height*sizeof(TxTile));}
void cx_pixel_ports(CxWorld* w,uint32_t y,uint32_t colour,uint32_t horizontal,uint32_t vertical){CxPixel* p=w->pixels+w->pixel_at_y[y]-1u;p->h[colour]=horizontal;p->v[colour]=vertical;}
static int compare_ports(const void* a,const void* b){const CxPort* x=(const CxPort*)a;const CxPort* y=(const CxPort*)b;if(x->net!=y->net)return x->net<y->net?-1:1;if(x->axis!=y->axis)return x->axis<y->axis?-1:1;return x->pixel<y->pixel?-1:x->pixel>y->pixel;}
void cx_devices_end_columns(CxWorld* w){
    for(uint32_t i=0;i<w->policy_count;i++)finish_policy(w,w->policies+i);cx_free(w,w->policies);w->policies=NULL;w->policy_count=w->policy_capacity=0;
    for(uint32_t i=0;i<2;i++){cx_free(w,w->previous_columns[i]);w->previous_columns[i]=NULL;}
}
int cx_devices_compile(CxWorld* w){
    cx_devices_end_columns(w);
    uint64_t count=0;for(uint32_t i=0;i<w->pixel_count;i++)for(uint32_t c=0;c<4;c++){CxPixel* p=w->pixels+i;if(w->twld_state&&p->custom&&p->h[c]==p->v[c])continue;count+=p->h[c]!=0;if(!w->twld_state||p->custom)count+=p->v[c]!=0;}
    for(uint32_t i=0;i<w->device_count;i++)if(w->devices[i].tile.type==144u||w->devices[i].tile.type==411u)for(uint32_t c=0;c<4;c++)count+=w->devices[i].nets[c]!=0;
    for(uint32_t i=0;i<w->device_count;i++)for(uint32_t c=0;c<4;c++)if(w->devices[i].nets[c]){count+=w->devices[i].tile.actuator!=0;count+=w->devices[i].tile.type==130u||w->devices[i].tile.type==131u;}
    if(count>UINT32_MAX/sizeof(CxPort))return TCW_MEMORY;
    w->ports=(CxPort*)cx_alloc(w,(count?count:1u)*sizeof(CxPort));w->pixel_touched=(uint32_t*)cx_alloc(w,(w->pixel_count?w->pixel_count:1u)*4u);w->pixel_snapshot=(uint8_t*)cx_alloc(w,w->pixel_count?w->pixel_count:1u);
    w->mech_capacity=w->device_count?w->device_count:1u;if(w->mech_capacity>CX_MECH_LIMIT)w->mech_capacity=CX_MECH_LIMIT;w->mechs=(uint32_t*)cx_alloc(w,w->mech_capacity*4u);w->mechs_snapshot=(uint32_t*)cx_alloc(w,w->mech_capacity*4u);
    if(!w->ports||!w->pixel_touched||!w->pixel_snapshot||!w->mechs||!w->mechs_snapshot)return TCW_MEMORY;
    for(uint32_t i=0;i<w->pixel_count;i++)for(uint32_t c=0;c<4;c++){CxPixel* p=w->pixels+i;if(w->twld_state&&p->custom&&p->h[c]==p->v[c])continue;if(p->h[c])w->ports[w->port_count++]=(CxPort){p->h[c],i,0,(uint8_t)c,0};if((!w->twld_state||p->custom)&&p->v[c])w->ports[w->port_count++]=(CxPort){p->v[c],i,1,(uint8_t)c,0};}
    for(uint32_t i=0;i<w->device_count;i++)if(w->devices[i].tile.type==144u||w->devices[i].tile.type==411u)for(uint32_t c=0;c<4;c++)if(w->devices[i].nets[c])w->ports[w->port_count++]=(CxPort){w->devices[i].nets[c],i,w->devices[i].tile.type==144u?2:3,(uint8_t)c,0};
    for(uint32_t i=0;i<w->device_count;i++)for(uint32_t c=0;c<4;c++)if(w->devices[i].nets[c]){if(w->devices[i].tile.actuator)w->ports[w->port_count++]=(CxPort){w->devices[i].nets[c],i,4,(uint8_t)c,0};if(w->devices[i].tile.type==130u||w->devices[i].tile.type==131u)w->ports[w->port_count++]=(CxPort){w->devices[i].nets[c],i,5,(uint8_t)c,0};}
    qsort(w->ports,w->port_count,sizeof(CxPort),compare_ports);return TCW_OK;
}
uint32_t cx_pixel_find(CxWorld* w,uint32_t x,uint32_t y){uint32_t lo=0,hi=w->pixel_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;CxPixel* p=w->pixels+m;if(p->x<x||(p->x==x&&p->y<y))lo=m+1;else hi=m;}return lo<w->pixel_count&&w->pixels[lo].x==x&&w->pixels[lo].y==y?lo:CX_NONE;}
CxDevice* cx_device_find(CxWorld* w,uint32_t x,uint32_t y){uint32_t lo=0,hi=w->device_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;CxDevice* p=w->devices+m;if(p->x<x||(p->x==x&&p->y<y))lo=m+1;else hi=m;}return lo<w->device_count&&w->devices[lo].x==x&&w->devices[lo].y==y?w->devices+lo:NULL;}
static int check_mech(CxWorld* w,CxDevice* d,uint32_t time){if(d->cooldown||w->mech_count>=CX_MECH_LIMIT)return 0;if(w->mech_count>=w->mech_capacity)return -1;d->cooldown=time;w->mechs[w->mech_count++]=(uint32_t)(d-w->devices);return 1;}
static int toggle_timer(CxWorld* w,CxDevice* d){if(d->tile.frame_y==0){d->tile.frame_y=18;if(check_mech(w,d,18000u)<0)return -1;}else d->tile.frame_y=0;return 0;}
int cx_trip_begin(void* context){CxWorld* w=(CxWorld*)context;++w->vm_trip_index;TerraVmGateRef source;w->vm_source_gate=terra_vm_current_gate(w->vm,&source)==1&&source.group==0?source.offset:0;if(!w->twld_state)w->pixel_touched_count=0;for(uint32_t i=0;i<w->device_count;i++)w->devices[i].wire_hit_mask=0;return 0;}
int cx_net_hit(void* context,uint32_t net){
    CxWorld* w=(CxWorld*)context;uint32_t group=net+1u,lo=0,hi=w->port_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(w->ports[m].net<group)lo=m+1;else hi=m;}
    while(lo<w->port_count&&w->ports[lo].net==group){CxPort* port=w->ports+lo++;
        if(port->axis>=4u){CxDevice* d=w->devices+port->pixel;if(cx_seed_contains(w,d->x,d->y)||(w->vm_source_gate&&d->gate==w->vm_source_gate))continue;
            if(port->axis==4u){if(d->tile.inactive)d->tile.inactive=0;else if(cx_actuatable_type(d->tile.type)){if(d->can_deactivate<0)return cx_fail(w,TCW_UNSUPPORTED,"actuator support neighborhood is not representable for this malformed tile frame");if(d->can_deactivate)d->tile.inactive=1;}}
            else if(d->tile.type==131u)d->tile.type=130u;else{if(d->can_deactivate<0)return cx_fail(w,TCW_UNSUPPORTED,"active stone support neighborhood is unavailable");/* Unlike DeActive, ActiveStone always calls CanKillTile, even with an empty cell above. */if(d->can_deactivate&&d->tile.wall!=350u)d->tile.type=131u;}continue;
        }
        if(port->axis==2u){CxDevice* d=w->devices+port->pixel;if(w->vm_trip_index==1u&&d->x>=w->seed_x&&d->x-w->seed_x<w->seed_width&&d->y>=w->seed_y&&d->y-w->seed_y<w->seed_height)continue;if(toggle_timer(w,d)<0)return -1;continue;}
        if(port->axis==3u){CxDevice* d=w->devices+port->pixel;uint32_t x=d->x-(uint32_t)(d->tile.frame_x%36/18),y=d->y-(uint32_t)(d->tile.frame_y%36/18);CxDevice* base=cx_device_find(w,x,y);if(!base)continue;uint32_t bit=1u<<port->colour;if(base->wire_hit_mask&bit)continue;
            if(w->vm_trip_index==1u&&d->x>=w->seed_x&&d->x-w->seed_x<w->seed_width&&d->y>=w->seed_y&&d->y-w->seed_y<w->seed_height)continue;
            base->wire_hit_mask|=bit;int shift=base->tile.frame_x>=36?-36:36;for(uint32_t a=x;a<x+2u;a++)for(uint32_t b=y;b<y+2u;b++){CxDevice* part=cx_device_find(w,a,b);if(part&&part->tile.type==411u)part->tile.frame_x=(int16_t)(part->tile.frame_x+shift);}continue;
        }
        CxPixel* p=w->pixels+port->pixel;if(!p->marked){p->marked=1;w->pixel_touched[w->pixel_touched_count++]=port->pixel;}uint8_t bit=(uint8_t)(1u<<port->colour);
        if(w->twld_state){if(port->axis)p->hit_v^=bit;else p->hit_h^=bit;}else if(port->axis)p->hit_v|=bit;else p->hit_h|=bit;
    }return 0;
}
static int pixel_pass(CxWorld* w){
    for(uint32_t i=0;i<w->pixel_touched_count;i++){CxPixel* p=w->pixels+w->pixel_touched[i];
        if(p->custom){uint8_t both=p->hit_h&p->hit_v;for(uint32_t c=0;c<4;c++)if(both&(1u<<c))p->state^=(uint8_t)(1u<<(3u-c));}
        else if(w->twld_state){uint32_t n=(uint32_t)__builtin_popcount((unsigned)p->hit_h);p->state^=(uint8_t)((n*(n-1u)/2u)&1u);}
        else if(p->hit_h&&p->hit_v)p->state^=1u;
        p->hit_h=p->hit_v=p->marked=0;
    }w->pixel_touched_count=0;return 0;
}
int cx_trip_end(void* context){CxWorld* w=(CxWorld*)context;return w->twld_state?0:pixel_pass(w);}
int cx_wave_begin(void* context){(void)context;return 0;}
int cx_wave_end(void* context){CxWorld* w=(CxWorld*)context;return w->twld_state?pixel_pass(w):0;}
int cx_interact(CxWorld* w){
    CxDevice* d=cx_device_find(w,w->command.x,w->command.y);if(!d)return 0;
    uint32_t type=d->tile.type;
    if(type==144u){if(toggle_timer(w,d)<0)return -1;return 0;}
    if(type==136u){d->tile.frame_y=d->tile.frame_y==0?18:0;return 1;}
    if(type==132u||type==411u){int32_t fx=d->tile.frame_x/18,dx=(-fx)%4,dy=-(d->tile.frame_y/18),shift=36;if(dx<-1){dx+=2;shift=-36;}int32_t x=(int32_t)d->x+dx,y=(int32_t)d->y+dy;if(x<0||y<0)return -1;
        if(type==411u){CxDevice* base=cx_device_find(w,(uint32_t)x,(uint32_t)y);if(base&&check_mech(w,base,60u)<0)return -1;}
        for(uint32_t a=(uint32_t)x;a<(uint32_t)x+2u;a++)for(uint32_t b=(uint32_t)y;b<(uint32_t)y+2u;b++){CxDevice* part=cx_device_find(w,a,b);if(part&&(part->tile.type==132u||part->tile.type==411u))part->tile.frame_x=(int16_t)(part->tile.frame_x+shift);}
        w->seed_x=(uint32_t)x;w->seed_y=(uint32_t)y;w->seed_width=w->seed_height=2u;return 1;
    }
    return type==135u||type==314u||type==423u||type==428u||type==442u||type==476u||type==440u||type==441u||type==468u||(type==467u&&d->tile.frame_x/36==4)?1:0;
}
int cx_interaction_rect(CxWorld* w,TerraCircuitWorldCommand* cmd){
    CxDevice* d=cx_device_find(w,cmd->x,cmd->y);if(!d)return 0;
    uint32_t type=d->tile.type;if(!is_switch(type))return 0;cmd->width=cmd->height=1u;
    if(type==132u||type==411u||type==441u||type==468u||type==467u){
        if(type==467u&&d->tile.frame_x/36!=4)return 0;
        int32_t dx=-(d->tile.frame_x/18)%4,dy=-(d->tile.frame_y/18);if(dx<-1)dx+=2;
        int32_t x=(int32_t)d->x+dx,y=(int32_t)d->y+dy;if(x<0||y<0||(uint32_t)x+2u>w->width||(uint32_t)y+2u>w->height)return -1;cmd->x=(uint32_t)x;cmd->y=(uint32_t)y;cmd->width=cmd->height=2u;
    }else if(type==440u){int32_t x=(int32_t)d->x-d->tile.frame_x/18%3,y=(int32_t)d->y-d->tile.frame_y/18%3;if(x<0||y<0||(uint32_t)x+3u>w->width||(uint32_t)y+3u>w->height)return -1;cmd->x=(uint32_t)x;cmd->y=(uint32_t)y;cmd->width=cmd->height=3u;}
    return 1;
}
int cx_seed_reserve(CxWorld* w,uint32_t count){
    if(count<=w->trigger_capacity)return TCW_OK;if((uint64_t)count*4u>UINT32_MAX)return TCW_MEMORY;
    uint32_t* p=(uint32_t*)cx_alloc(w,count*4u);if(!p)return TCW_MEMORY;cx_free(w,w->trigger_nets);w->trigger_nets=p;w->trigger_capacity=count;return TCW_OK;
}
static uint32_t timer_interval(const CxDevice* d){switch(d->tile.frame_x/18){case 0:return 60;case 1:return 180;case 2:return 300;case 3:return 30;case 4:return 15;default:return 60;}}
int cx_ticks_step(CxWorld* w,uint32_t* work){
    while(*work&&w->tick_remaining){
        if(w->tick_stage==0){++w->ticks;w->tick_cursor=w->mech_count;w->tick_stage=1;--*work;}
        if(w->tick_stage==2){int s=terra_vm_step(w->vm,*work);*work=0;if(s<0)return cx_fail(w,TCW_STATE,"timer circuit pulse failed and was rolled back");if(s==TERRA_VM_MORE)return TCW_CONTINUE;w->tick_stage=1;continue;}
        while(*work&&w->tick_cursor){uint32_t at=--w->tick_cursor,id=w->mechs[at];CxDevice* d=w->devices+id;--*work;if(d->cooldown)d->cooldown--;
            if(d->tile.type==144u){if(d->tile.frame_y==0)d->cooldown=0;else if(d->cooldown%timer_interval(d)==0u){d->cooldown=18000u;w->trigger_count=0;for(uint32_t c=0;c<4;c++)if(d->pulse_nets[c])w->trigger_nets[w->trigger_count++]=d->pulse_nets[c]-1u;w->seed_x=d->x;w->seed_y=d->y;w->seed_width=w->seed_height=1;
                    uint32_t avail=cx_vm_available(w);if(!avail||terra_vm_set_budget(w->vm,avail)<0||terra_vm_begin(w->vm,w->trigger_nets,w->trigger_count)<0)return TCW_MEMORY;w->tick_stage=2;return TCW_CONTINUE;}}
            if(!d->cooldown){if(d->tile.type==144u)d->tile.frame_y=0;else if(d->tile.type==411u){int shift=d->tile.frame_x>=36?-36:36;for(uint32_t x=d->x;x<d->x+2u;x++)for(uint32_t y=d->y;y<d->y+2u;y++){CxDevice* part=cx_device_find(w,x,y);if(part&&part->tile.type==411u)part->tile.frame_x=(int16_t)(part->tile.frame_x+shift);}}memmove(w->mechs+at,w->mechs+at+1u,(w->mech_count-at-1u)*4u);--w->mech_count;}
        }
        if(w->tick_stage==1u&&!w->tick_cursor){--w->tick_remaining;w->tick_stage=0;}
    }
    if(w->tick_remaining)return TCW_CONTINUE;cx_operation_commit(w);w->phase=CX_IDLE;return TCW_OK;
}
