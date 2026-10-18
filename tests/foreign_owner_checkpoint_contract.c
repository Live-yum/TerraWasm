#include "terra_world.h"
#include "terra_plr.h"
#include "terra_circuit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern uint32_t tx_native_heap_used(void);
extern void terrax_test_plr_fail_alloc_after(int32_t);
static char* fixture(void) {
    FILE*f=fopen(TERRAWASM_PLR_JSON_FIXTURE_PATH,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    char*s=malloc((size_t)n+1);assert(s&&fread(s,1,(size_t)n,f)==(size_t)n);s[n]=0;assert(!fclose(f));return s;
}
static void run(int rollback,int cached) {
    char*json=fixture();uint32_t player=0,world=0,circuit=0,token=0,n=0,size=0,other=99;
    assert(terra_plr_open_json(json,&player)==0);free(json);
    assert(terra_plr_get_json(player,NULL,0,&n)==0);char*expected=malloc(n),*actual=malloc(n);assert(expected&&actual);
    assert(terra_plr_get_json(player,expected,n,&n)==0);uint32_t json_size=n;
    assert(terra_plr_save_to_buffer(player,NULL,0,&size)==0);uint8_t*encoded=malloc(size),*output=malloc(size);assert(encoded&&output);
    assert(terra_plr_save_to_buffer(player,encoded,size,&size)==0);
    if(!cached)assert(terra_plr_release_caches(player)==0);
    assert(terra_circuit_create(8,8,8,1024u*1024u,&circuit)==0);
    TerraCircuitCell cells[2]={{2,3,1,TERRA_CIRCUIT_WIRE},{3,3,1,TERRA_CIRCUIT_TILE}};
    assert(terra_circuit_load(circuit,cells,2)==0);assert(terra_circuit_compile(circuit,8,&n)==0);
    TerraCircuitPoint seed={2,3};assert(terra_circuit_begin(circuit,&seed,1,0,0,64)==0);
    assert(terra_world_create(&world)==0);assert(terra_world_workspace_begin(world,UINT32_MAX,&token)==0);
    uint32_t heap=tx_native_heap_used();TerraCircuitStats before,after;assert(terra_circuit_stats(circuit,&before)==0&&before.active&&before.queued);
    assert(terra_plr_open_json("{}",&other)==TERRAX_WORLD_STATUS_STATE_ERROR&&other==0);
    other=99;assert(terra_plr_open_from_buffer(NULL,0,&other)==TERRAX_WORLD_STATUS_STATE_ERROR&&other==0);
    other=99;assert(terra_plr_open("not-read.plr",&other)==TERRAX_WORLD_STATUS_STATE_ERROR&&other==0);
    assert(terra_plr_close(player)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_set(player,NULL,NULL)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_set_many(player,NULL)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_replace_json(player,NULL)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_apply_patch_json(player,NULL)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_release_caches(player)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_save(player,"not-written.plr")==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_plr_validate_handle(player)==0);
    assert(terra_plr_get_json(player,NULL,0,&n)==0&&n==json_size);assert(tx_native_heap_used()==heap);
    assert(terra_plr_get_json(player,actual,json_size,&n)==0&&!memcmp(actual,expected,json_size));assert(tx_native_heap_used()==heap);
    assert(terra_plr_get_json(player,actual,1,&n)==TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL);assert(tx_native_heap_used()==heap);
    assert(terra_plr_get(player,"/name",NULL,0,&n)==0&&n);assert(tx_native_heap_used()==heap);
    assert(terra_plr_get_keys(player,"",NULL,0,&n)==0&&n);assert(tx_native_heap_used()==heap);
    assert(terra_plr_save_to_buffer(player,NULL,0,&n)==0&&n==size);assert(tx_native_heap_used()==heap);
    assert(terra_plr_save_to_buffer(player,output,size,&n)==0&&!memcmp(output,encoded,size));assert(tx_native_heap_used()==heap);
    assert(terra_plr_save_to_buffer(player,output,1,&n)==TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL);assert(tx_native_heap_used()==heap);
    terrax_test_plr_fail_alloc_after(0);
    assert(terra_plr_get(player,"/name",NULL,0,&n)==TERRAX_WORLD_STATUS_INTERNAL_ERROR);
    if(!cached){assert(terra_plr_get_json(player,NULL,0,&n)==TERRAX_WORLD_STATUS_INTERNAL_ERROR);assert(terra_plr_save_to_buffer(player,NULL,0,&n)==TERRAX_WORLD_STATUS_INTERNAL_ERROR);}
    terrax_test_plr_fail_alloc_after(-1);assert(tx_native_heap_used()==heap);
    other=99;assert(terra_circuit_create(8,8,8,1024u*1024u,&other)==TERRA_CIRCUIT_STATE&&other==0);
    assert(terra_circuit_load(circuit,NULL,1)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_compile(circuit,1,&n)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_patch(circuit,NULL,1)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_begin(circuit,NULL,1,1,0,1)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_step(circuit,1,NULL,0,NULL)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_cancel(circuit)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_close(circuit)==TERRA_CIRCUIT_STATE);
    assert(terra_circuit_stats(circuit,&after)==0&&!memcmp(&before,&after,sizeof(before)));assert(tx_native_heap_used()==heap);
    assert((rollback?terra_world_workspace_rollback(world,token):terra_world_workspace_commit(world,token))==0);
    assert(terra_circuit_stats(circuit,&after)==0&&!memcmp(&before,&after,sizeof(before)));
    assert(terra_plr_get_json(player,actual,json_size,&n)==0&&!memcmp(actual,expected,json_size));
    assert(terra_plr_save_to_buffer(player,output,size,&n)==0&&!memcmp(output,encoded,size));
    TerraCircuitEvent events[8];TerraCircuitStep step;int status=1;for(int i=0;i<64&&status==1;i++)status=terra_circuit_step(circuit,8,events,8,&step);assert(status==0);
    assert(terra_circuit_close(circuit)==0);assert(terra_plr_close(player)==0);assert(terra_world_close(world)==0);
    free(expected);free(actual);free(encoded);free(output);assert(tx_native_heap_used()==0);
}
int main(void){for(int r=0;r<2;r++)for(int c=0;c<2;c++)run(r,c);return 0;}
