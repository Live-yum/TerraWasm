#include "terra_world.h"
#include "terra_pixel_workspace.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
extern uint32_t tx_native_heap_used(void);
static void run(int rollback) {
    uint32_t pixel=0,world=0,token=0,color=0,other=99;
    assert(terra_pixel_workspace_create(64,64,8u*1024u*1024u,1024u*1024u,256,&pixel)==0);
    assert(terra_pixel_workspace_palette_add(pixel,0xabcdef,&color)==0&&color);
    assert(terra_pixel_workspace_tx_begin(pixel)==0);
    TerraPixelCell cell={2,3,color};
    assert(terra_pixel_workspace_cells(pixel,&cell,1)==0);
    assert(terra_pixel_workspace_tx_commit(pixel)==0);
    assert(terra_pixel_workspace_tx_begin(pixel)==0);
    cell.x=4;assert(terra_pixel_workspace_cells(pixel,&cell,1)==0);
    assert(terra_world_create(&world)==0);
    assert(terra_world_workspace_begin(world,UINT32_MAX,&token)==0);
    TerraPixelWorkspaceStats before,after;
    assert(terra_pixel_workspace_stats(pixel,&before)==0&&before.transaction_open);
    uint32_t heap=tx_native_heap_used();
    assert(terra_pixel_workspace_create(64,64,8u*1024u*1024u,1024u*1024u,256,&other)==TERRA_PIXEL_TRANSACTION&&other==0);
    assert(terra_pixel_workspace_close(pixel)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_tx_begin(pixel)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_tx_commit(pixel)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_tx_rollback(pixel)==TERRA_PIXEL_TRANSACTION);
    /* Invalid mutation requests must not enter px_fail and roll back pixels. */
    assert(terra_pixel_workspace_palette_add(pixel,UINT32_MAX,NULL)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_cells(pixel,NULL,1)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_stroke(pixel,NULL,1,1,1)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_fill(pixel,-1,-1,UINT32_MAX)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_replace(pixel,UINT32_MAX,0)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_transform(pixel,-1,-1,1,1,99)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_import_rgba(pixel,-1,-1,1,1,NULL,0,0)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_undo(pixel)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_redo(pixel)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_checkpoint(pixel)==TERRA_PIXEL_TRANSACTION);
    assert(terra_pixel_workspace_clear_history(pixel)==TERRA_PIXEL_TRANSACTION);
    uint16_t value=0;
    assert(terra_pixel_workspace_read_rect(pixel,4,3,1,1,&value,1)==0&&value==color);
    assert(terra_pixel_workspace_read_rect(pixel,-1,0,1,1,&value,1)==TERRA_PIXEL_BOUNDS);
    uint32_t palette[2];assert(terra_pixel_workspace_palette_read(pixel,0,2,palette)==0&&palette[1]==0xabcdef);
    uint32_t rgb=0xabcdef;assert(terra_pixel_workspace_nearest(pixel,&rgb,1,&value)==0&&value==color);
    TerraPixelCandidate candidates[2]={{0,0},{0xabcdef,0}};uint32_t matched=99;
    assert(terra_pixel_workspace_match_colors(candidates,2,&rgb,1,0,&matched)==0&&matched==1);
    assert(terra_pixel_workspace_stats(pixel,&after)==0&&!memcmp(&before,&after,sizeof(before)));
    assert(tx_native_heap_used()==heap);
    assert((rollback?terra_world_workspace_rollback(world,token):terra_world_workspace_commit(world,token))==0);
    assert(terra_pixel_workspace_stats(pixel,&after)==0&&!memcmp(&before,&after,sizeof(before)));
    assert(terra_pixel_workspace_tx_commit(pixel)==0);
    assert(terra_pixel_workspace_undo(pixel)==0);
    assert(terra_pixel_workspace_read_rect(pixel,4,3,1,1,&value,1)==0&&value==0);
    assert(terra_pixel_workspace_redo(pixel)==0);
    assert(terra_pixel_workspace_read_rect(pixel,4,3,1,1,&value,1)==0&&value==color);
    assert(terra_pixel_workspace_close(pixel)==0);
    assert(terra_world_close(world)==0);
    assert(tx_native_heap_used()==0);
}
int main(void) { run(0);run(1);return 0; }
