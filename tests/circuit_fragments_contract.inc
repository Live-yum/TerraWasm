/* Included by the already enabled native stream boundary contract. Synthetic
 * WLD fixture is encoded independently by helpers/circuit-world.js. */
#include "terra_circuit_world.h"
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);
static uint8_t* fragment_scratch;static uint32_t fragment_scratch_size;
static uint32_t fragment_rows[4096],fragment_row_count,fragment_total;
static uint8_t* fragment_objects;static uint32_t fragment_objects_size;
static int fragment_cancel_objects;
static const uint8_t* stamp_objects;static uint32_t stamp_objects_size;
static void fragment_pump(uint32_t handle){
 TerraCircuitWorldEvent e;fragment_row_count=fragment_total=0;
 for(uint32_t n=0;n<1000000;n++){
  int s=terra_circuit_world_step(handle,17,&e);assert(s==0||s==1);
  if(fragment_cancel_objects&&e.kind==TCW_WRITE&&e.source_id==11){assert(e.offset==0&&e.length>=32);fragment_cancel_objects=0;assert(!terra_circuit_world_cancel(handle));return;}
  if(e.kind==TCW_READ){const uint8_t* b=e.source_id==1?source:fragment_scratch;uint32_t size=e.source_id==1?source_size:fragment_scratch_size;assert(e.offset<=size&&e.length<=size-e.offset);memcpy(bridge,b+e.offset,e.length);assert(!terra_circuit_world_supply(handle,e.source_id,e.offset,bridge,e.length));}
  else if(e.kind==TCW_WRITE){if(e.source_id==2){uint32_t end=e.offset+e.length;if(end>fragment_scratch_size){fragment_scratch=realloc(fragment_scratch,end);assert(fragment_scratch);fragment_scratch_size=end;}memcpy(fragment_scratch+e.offset,(void*)(uintptr_t)e.data_ptr,e.length);}else if(e.source_id==11){assert(e.offset==fragment_objects_size&&e.length<=65536);fragment_objects_size+=e.length;fragment_objects=realloc(fragment_objects,fragment_objects_size);assert(fragment_objects);memcpy(fragment_objects+e.offset,(void*)(uintptr_t)e.data_ptr,e.length);}else assert(e.source_id==9);assert(!terra_circuit_world_ack(handle));}
  else if(e.kind==TCW_RESULT){assert(e.length==e.result_count*32u&&e.length<=sizeof(fragment_rows));memcpy(fragment_rows,(void*)(uintptr_t)e.data_ptr,e.length);fragment_row_count=e.result_count;assert(!terra_circuit_world_ack(handle));}
  else if(e.kind==TCW_READY){fragment_total=e.result_count;return;}
 }assert(!"fragment command exceeded bounded execution");
}
static void circuit_fragments_contract(const char* path){
 load(path);uint32_t baseline=tx_native_heap_used(),handle;assert(!terra_circuit_world_begin(world,2,0,0,32u*1024u*1024u,hp));handle=*hp;fragment_pump(handle);
 uint32_t* geometry=alloc(64);for(uint32_t x=0;x<2;x++)for(uint32_t y=0;y<2;y++){uint32_t* g=geometry+(x*2+y)*4;g[0]=132;g[1]=x*18|(y*18<<16);g[2]=x|(y<<8)|(2<<16)|(2<<24);g[3]=0;}
 TerraCircuitWorldCommand command={0};command.abi_version=1;command.kind=TCW_FRAGMENTS;command.count=2;command.data_ptr=(uint32_t)(uintptr_t)geometry;command.data_count=4;
 assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total==6&&fragment_row_count==2);uint32_t total=fragment_total,all[48];memcpy(all,fragment_rows,64);
 for(uint32_t offset=2;offset<total;offset+=2){command.x=offset;command.data_ptr=command.data_count=0;assert(terra_circuit_world_command(handle,&command)>=0);fragment_pump(handle);assert(fragment_total==total);memcpy(all+offset*8,fragment_rows,fragment_row_count*32u);}
 uint32_t pure=0,multi=0,stack=0;for(uint32_t i=0;i<total;i++){uint32_t* d=all+i*8;if(d[1]==3&&d[2]==4){pure=d[0];assert(d[5]==7&&d[6]==7&&!d[7]);}if(d[1]==11&&d[2]==10){multi=d[0];assert(d[3]==4&&d[4]==2&&d[5]==6&&d[6]==4&&!d[7]);}if(d[1]==20){stack=d[0];assert(d[5]==3&&!d[7]);}}
 assert(pure&&multi&&stack);command=(TerraCircuitWorldCommand){0};command.abi_version=1;command.kind=TCW_EXTRACT;command.mask=pure;command.count=32;
 assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total==7&&fragment_row_count==7);for(uint32_t i=0;i<7;i++)assert(fragment_rows[i*8+5]>>24==1);
 command.mask=multi;assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total==6);uint32_t objects=0;
 for(uint32_t i=0;i<6;i++){uint32_t* r=fragment_rows+i*8;if((r[2]&65535u)==132){objects++;assert((r[4]&65535u)==0&&((r[4]>>16)&255u)==7&&(r[4]>>24)==0);}}assert(objects==4);
 command.count=5;assert(terra_circuit_world_command(handle,&command)==TCW_MEMORY);
 command.kind=TCW_SAVE;command.source_id=9;assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total>0);
 command.kind=TCW_EXTRACT;command.count=32;assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total==6);assert(!terra_circuit_world_cancel(handle));assert(!terra_circuit_world_close(handle));assert(tx_native_heap_used()==baseline);
 tx_free((uint32_t)(uintptr_t)geometry);free(fragment_scratch);fragment_scratch=NULL;fragment_scratch_size=0;check(terra_world_close(world));world=0;free(source);source=NULL;
 puts("circuit fragments: color crossings, full 2x2 objects, lamp stacks, pagination, bounds and release passed");
}
static uint8_t* stamp_pump(uint32_t id,const uint32_t* records,uint32_t bytes,uint32_t* size,int expect_failure,int lease){
 uint8_t* output=NULL;uint32_t capacity=0;*size=0;
 for(uint32_t step=0;step<1000000;step++){
  int status=terra_world_stream_step(id,7,ep);if(status<0){assert(expect_failure);if(expect_failure==2)assert(!capacity&&!output);uint32_t retained=tx_native_heap_used();assert(terra_world_stream_step(id,7,ep)<0);assert(terra_world_stream_step(id,7,ep)<0);assert(tx_native_heap_used()==retained);free(output);return NULL;}
  if(ep->kind==TX_STREAM_NEED_SOURCE){const uint8_t* b=ep->source_id==1?source:ep->source_id==10?stamp_objects:(const uint8_t*)records;uint32_t n=ep->source_id==1?source_size:ep->source_id==10?stamp_objects_size:bytes;assert(ep->source_id==1||ep->source_id==9||ep->source_id==10);assert(ep->offset<=n&&ep->length<=n-ep->offset);
   if(lease){check(terra_world_stream_acquire_input(id,lp));memcpy((void*)(uintptr_t)lp->data_ptr,b+lp->offset,lp->length);check(terra_world_stream_commit_input(id,lp->lease_id,lp->source_id,lp->offset,lp->length));}
   else{memcpy(bridge,b+ep->offset,ep->length);check(terra_world_stream_supply_source(id,ep->source_id,ep->offset,bridge,ep->length));}
  }else if(ep->kind==TX_STREAM_OUTPUT){assert(!expect_failure||ep->offset>=0);uint32_t end=ep->offset+ep->length;if(end>capacity){capacity=end+1024;output=realloc(output,capacity);assert(output);}memcpy(output+ep->offset,(void*)(uintptr_t)ep->data_ptr,ep->length);check(terra_world_stream_ack_output(id));}
  else if(ep->kind==TX_STREAM_READY){assert(!expect_failure);*size=ep->result_size;return output;}
 }assert(!"stamp did not complete");return NULL;
}
static void circuit_stamp_contract(const char* path){
 load(path);uint32_t baseline=tx_native_heap_used(),records[24]={0};records[0]=0;records[1]=0;records[5]=1u<<24;records[8]=1;records[9]=0;records[10]=1u|(1u<<16);records[12]=2u|(8u<<16)|(9u<<24);records[13]=4u<<24;
 records[16]=2;records[18]=314u|(1u<<16);records[19]=0xffff0000u;
 const char* request="{\"x\":30,\"y\":25,\"width\":3,\"height\":1,\"recordCount\":3,\"recordSourceId\":9,\"mode\":\"overlay\"}";
 uint32_t id=begin("stamp_tiles",request),n;uint8_t* output=stamp_pump(id,records,sizeof(records),&n,0,1);assert(output&&n);check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 uint32_t tail=get32(source+34),new_tail=get32(output+34);assert(source_size-tail==n-new_tail&&!memcmp(source+tail,output+new_tail,n-new_tail));
 records[8]=0;id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,sizeof(records),&n,1,0));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);records[8]=1;
 /* Container/sign/TileEntitiesManager registrations at the pinned game source.
  * Every ID is in-range and otherwise valid: reject before writing a candidate,
  * without losing its inventory, text, display item or anchored entity state. */
 const uint32_t section_tiles[]={21,55,85,88,378,395,423,425,467,470,471,475,520,573,597,698,723,724};
 assert(tx_get_world(world)->tile_type_count>724);
 for(uint32_t i=0;i<sizeof(section_tiles)/sizeof(section_tiles[0]);i++){
  records[10]=section_tiles[i]|(1u<<16);id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,sizeof(records),&n,2,i&1));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 }records[10]=1u|(1u<<16);
 records[2]=1u|(1u<<16);id=begin("stamp_tiles","{\"x\":12,\"y\":10,\"width\":3,\"height\":1,\"recordCount\":3,\"recordSourceId\":9,\"mode\":\"overlay\"}");assert(!stamp_pump(id,records,sizeof(records),&n,1,1));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);records[2]=0;
 id=begin("stamp_tiles",request);check(terra_world_stream_step(id,1,ep));check(terra_world_stream_cancel(id));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 uint32_t saved_size=new_tail+source_size-tail;check(terra_world_close(world));world=0;check(terra_world_open_from_buffer(output,saved_size,&world));TxWorld* w=tx_get_world(world);uint32_t cursor=w->starts[1];int found=0;
 for(uint32_t x=0;x<(uint32_t)w->maxTilesX;x++)for(uint32_t y=0;y<(uint32_t)w->maxTilesY;){TxTile tile;assert(read_tile_at(w,&cursor,w->ends[1],&tile));uint32_t run=tile.same+1;if(x==30&&y<=25&&25<y+run){assert(tile.active&&tile.type==1&&tile.tile_color==4&&tile.wall==3&&tile.wall_color==6&&tile.wire_red&&tile.wire_blue);found++;}if(x==31&&y<=25&&25<y+run){assert(tile.active&&tile.type==1&&tile.tile_color==8&&tile.wall==2&&tile.wall_color==9&&tile.wire_green);found++;}if(x==32&&y<=25&&25<y+run){assert(tile.active&&tile.type==314&&tile.frame_x==0&&tile.frame_y==-1);found++;}y+=run;}assert(found==3);
 check(terra_world_close(world));world=0;free(output);free(source);source=NULL;assert(!tx_native_heap_used());
 puts("circuit stamp: overlay terrain preservation, exact fields, metadata bytes, invalid source rollback and cancel passed");
}
