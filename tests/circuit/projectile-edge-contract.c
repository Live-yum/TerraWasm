#include <assert.h>
#include <stdio.h>
#include "terra_circuit_devices.c"
/* Error reporting is outside these helper-level contracts. */
int cx_fail(CxWorld* w, int code, const char* reason) {(void)w;(void)reason;return code;}
int main(void) {
    CxWorld w={0}; CxProjectile pool[1000]={0}; uint32_t columns[81]={0};
    w.projectiles=pool;w.width=w.height=80;w.solid_columns=columns;
    assert(projectile_slot(&w)==0);
    for(unsigned i=0;i<1000;i++){pool[i].active=1;pool[i].remaining=100;}
    pool[500].remaining=3;pool[300].remaining=3;
    assert(projectile_slot(&w)==300); /* lowest remaining, earliest index on tie */
    pool[900].active=0;assert(projectile_slot(&w)==900); /* any free slot wins */
    pool[900].active=1;pool[2].remaining=2;assert(projectile_slot(&w)==2);
    CxDevice pad={0};pad.x=10;pad.y=12;pad.tile.active=1;pad.tile.type=442;pad.tile.frame_x=44;
    w.devices=&pad;w.device_count=1;
    CxProjectile p={.x=177,.y=192,.vx=-8,.vy=0,.origin_x=178,.origin_y=199,.remaining=230,.type=186,.active=1,.returning=1};
    assert(projectile_move(&w,&p)==TCW_OK);
    assert(!p.active&&p.x==169&&p.y==192); /* AI Kill still allows final move. */
    assert(projectile_pad(&w,&p,177,192)==&pad); /* final entry still trips pad */
    p=(CxProjectile){.x=150,.y=192,.type=601,.active=1};
    assert(projectile_pad(&w,&p,149,192)==&pad); /* exact edge contact is entry */
    p.x=151;
    assert(projectile_pad(&w,&p,150,192)==NULL); /* old edge contact suppresses another entry */
    /* Collision.SwitchTiles, fixed Terraria source 8255d346, lines 3316-3320:
       HitSwitch is followed by return true inside the x/y scan. A movement
       crossing two 442 rectangles triggers only the first newly entered pad. */
    CxDevice adjacent[2]={0};
    for(unsigned i=0;i<2;i++) {
        adjacent[i].x=10+i;adjacent[i].y=12;
        adjacent[i].tile.active=1;adjacent[i].tile.type=442;
        adjacent[i].tile.frame_x=22;
    }
    w.devices=adjacent;w.device_count=2;
    p=(CxProjectile){.x=171,.y=182,.type=601,.active=1};
    assert(pad_intersect(160,192,16,10,p.x,p.y,10,10));
    assert(pad_intersect(176,192,16,10,p.x,p.y,10,10));
    assert(projectile_pad(&w,&p,171,181)==&adjacent[0]);
    p.y=183;
    assert(projectile_pad(&w,&p,171,182)==NULL); /* both old rectangles now touch */
    puts("projectile edge contracts: slot selection, final movement, inclusive pad entry and no duplicate edge entry, first-only adjacent pad entry passed");
    return 0;
}
