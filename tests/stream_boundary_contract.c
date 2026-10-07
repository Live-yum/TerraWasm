/* On LP64 Linux the public Wasm32 bridge requires low addresses. The local
 * runner uses -no-pie and disables glibc mmap allocations. Emscripten has native
 * 32-bit addresses; no allocator accommodation is needed there. */
#include "terra_stream.h"
#include "terra_world.h"
#include "terra_types.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#if defined(__GLIBC__) && !defined(__wasm__)
#include <malloc.h>
#endif
extern uint32_t tx_malloc(uint32_t);
extern void tx_free(uint32_t);
extern uint32_t tx_native_heap_used(void);
extern TxWorld* tx_get_world(uint32_t);
extern void tx_render_stream_color(TxWorld*,const TxTile*,uint32_t,uint8_t*);
extern void txw_test_allocation_limit(uint32_t);
extern void txw_test_stream_memory_bytes(uint32_t);
extern int tx_stream_parse_tile_rules(TxWorld*,const char*,int,TxTileRule**,uint32_t*);
extern void tx_internal_free(void*);
static uint8_t* source;static uint32_t source_size,world;
static uint32_t *hp;static TxStreamEvent* ep;static TxStreamInputLease* lp;static TxStreamStats* sp;
static uint8_t* bridge;
static uint32_t get32(const uint8_t*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t be(const uint8_t*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void check(int status){if(status){char error[512];uint64_t n;terra_info_get_last_error_json(error,sizeof(error),&n);fprintf(stderr,"status %d: %s\n",status,error);abort();}}
static void* alloc(uint32_t n){uint32_t p=tx_malloc(n);assert(p);return (void*)(uintptr_t)p;}
static char* string(const char*s){char*p=alloc((uint32_t)strlen(s)+1);strcpy(p,s);return p;}
static uint32_t begin(const char*name,const char*request){char*n=string(name),*r=string(request);int status=terra_world_stream_operation_begin(world,n,r,hp);tx_free((uint32_t)(uintptr_t)n);tx_free((uint32_t)(uintptr_t)r);check(status);assert(*hp);return *hp;}
static uint8_t* pump(uint32_t id,int leased,uint32_t*size){
 uint8_t* output=NULL;uint32_t cap=0;*size=0;
 for(uint32_t step=0;step<10000000;step++){
  check(terra_world_stream_step(id,1,ep));TxStreamEvent first=*ep;
  if(ep->kind){check(terra_world_stream_step(id,1,ep));assert(!memcmp(&first,ep,sizeof(first)));}
  if(ep->kind==TX_STREAM_NEED_SOURCE){assert(ep->source_id==1&&ep->offset<=source_size&&ep->length<=source_size-ep->offset);
   if(leased){check(terra_world_stream_acquire_input(id,lp));assert(lp->abi_version==2&&lp->length==ep->length&&lp->capacity>=lp->length);assert(terra_world_stream_acquire_input(id,lp)<0);
    memcpy((void*)(uintptr_t)lp->data_ptr,source+lp->offset,lp->length);uint32_t lease=lp->lease_id;
    assert(terra_world_stream_commit_input(id,lease,2,lp->offset,lp->length)<0);
    assert(terra_world_stream_commit_input(id,lease,1,lp->offset+1,lp->length)<0);
    assert(terra_world_stream_commit_input(id,lease,1,lp->offset,lp->length-1)<0);
    check(terra_world_stream_commit_input(id,lease,1,lp->offset,lp->length));assert(terra_world_stream_commit_input(id,lease,1,lp->offset,lp->length)<0);
   }else{memcpy(bridge,source+ep->offset,ep->length);check(terra_world_stream_supply_source(id,1,ep->offset,bridge,ep->length));}
  }else if(ep->kind==TX_STREAM_OUTPUT){uint32_t end=ep->offset+ep->length;assert(end>=ep->offset);
   if(end>cap){cap=end*2+1024;output=realloc(output,cap);assert(output);}memcpy(output+ep->offset,(void*)(uintptr_t)ep->data_ptr,ep->length);
   check(terra_world_stream_ack_output(id));
  }else if(ep->kind==TX_STREAM_READY){*size=ep->result_size;check(terra_world_stream_get_stats(id,sp));return output;}
  else assert(ep->kind==TX_STREAM_MORE);
 }assert(!"stream did not finish");return NULL;
}
static void load(const char*path){FILE*f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);source_size=(uint32_t)ftell(f);rewind(f);source=malloc(source_size);assert(source);assert(fread(source,1,source_size,f)==source_size);fclose(f);
 check(terra_world_stream_open_begin(1,source_size,hp));uint32_t id=*hp,n;assert(!pump(id,1,&n));assert(sp->input_copy_bytes==0);check(terra_world_stream_adopt(id,1,hp));world=*hp;check(terra_world_stream_close(id));}
static void verify_png(const uint8_t*png,uint32_t length,int noise){
 TxWorld*w=tx_get_world(world);uint32_t stride=(uint32_t)w->maxTilesX*3+1,raw_size=stride*w->maxTilesY,compressed_size=0;
 uint8_t*compressed=malloc(length),*raw=malloc(raw_size);assert(compressed&&raw);
 for(uint32_t off=8;off<length;){assert(length-off>=12);uint32_t n=be(png+off);assert(n<=length-off-12);assert(be(png+off+n+8)==(uint32_t)crc32(0,png+off+4,n+4));if(!memcmp(png+off+4,"IDAT",4)){memcpy(compressed+compressed_size,png+off+8,n);compressed_size+=n;}off+=n+12;}
 uLongf actual=raw_size;assert(uncompress(raw,&actual,compressed,compressed_size)==Z_OK&&actual==raw_size);
 for(uint32_t y=0;y<(uint32_t)w->maxTilesY;y++){assert(raw[y*stride]==0);for(uint32_t x=0;x<(uint32_t)w->maxTilesX;x++){
  TxTile tile={0};tile.active=1;tile.type=1+((x+(noise==2?y/40:noise?y:0))&1);uint8_t rgba[4];tx_render_stream_color(w,&tile,y,rgba);assert(!memcmp(raw+y*stride+1+x*3,rgba,3));}}
 free(compressed);free(raw);
}
static void png_contract(const char*path,int noise){load(path);uint32_t baseline=tx_native_heap_used();
 txw_test_allocation_limit(3u*1024u*1024u);uint32_t id=begin("render_preview_png","{}"),n;uint8_t*png=pump(id,1,&n);txw_test_allocation_limit(UINT32_MAX);
 uint32_t tiles=get32(source+34)-get32(source+30);TxWorld*w=tx_get_world(world);
 assert(sp->tile_scan_passes==1);assert(sp->tile_records==(uint32_t)w->maxTilesX*(noise==2?(uint32_t)w->maxTilesY/40:noise?(uint32_t)w->maxTilesY:1));
 assert(sp->decoded_tile_bytes==tiles&&sp->source_bytes<=tiles*2);assert(sp->input_copy_bytes==0&&sp->cache_bytes<3u*1024u*1024u);
 if(noise==2)assert(sp->cache_copy_bytes==tiles&&sp->source_requests==2&&sp->source_bytes<tiles+1024);
 printf("PNG %s records=%u source=%u decoded=%u requests=%u cache=%u bytes\n",noise==2?"short-columns":noise?"noise":"RLE",sp->tile_records,sp->source_bytes,sp->decoded_tile_bytes,sp->source_requests,sp->cache_bytes);
 verify_png(png,n,noise);free(png);check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 id=begin("render_preview_png","{}");check(terra_world_stream_step(id,1,ep));check(terra_world_stream_cancel(id));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 check(terra_world_close(world));world=0;free(source);source=NULL;
}
static uint8_t* operation(const char*name,const char*json,uint32_t*n){uint32_t id=begin(name,json);uint8_t*b=pump(id,1,n);check(terra_world_stream_close(id));return b;}
static void edit_contract(const char*path){load(path);uint32_t n;uint8_t*out=operation("header_patch","{\"patch\":{\"worldName\":\"first\"}}",&n);
 assert(sp->tile_records==0&&sp->tile_copy_bytes==get32(source+34)-get32(source+30)&&sp->candidate_count==1);
 assert(!memcmp(source+get32(source+30),out+get32(out+30),sp->tile_copy_bytes));free(out);
 const char* plan="{\"operations\":[{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"worldName\":\"first\"}}},{\"operation\":\"save\",\"request\":{}},{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"worldName\":\"last\"}}}]}";
 out=operation("edit_plan",plan,&n);assert(sp->tile_records==0&&sp->candidate_count==1);assert(!memcmp(source+get32(source+30),out+get32(out+30),sp->tile_copy_bytes));
 uint32_t temp;check(terra_world_close(world));check(terra_world_open_from_buffer(out,n,&temp));assert(!strcmp(tx_get_world(temp)->worldName,"last"));check(terra_world_close(temp));free(out);free(source);world=0;source=NULL;
}
static uint8_t* inflate_png(const uint8_t*png,uint32_t length,uint32_t*size){
 uint32_t compressed_size=0;uint8_t*compressed=malloc(length);assert(compressed);
 *size=(be(png+16)*3+1)*be(png+20);uint8_t*raw=malloc(*size);assert(raw);
 for(uint32_t off=8;off<length;){uint32_t n=be(png+off);assert(n<=length-off-12);if(!memcmp(png+off+4,"IDAT",4)){memcpy(compressed+compressed_size,png+off+8,n);compressed_size+=n;}off+=n+12;}
 uLongf actual=*size;assert(uncompress(raw,&actual,compressed,compressed_size)==Z_OK&&actual==*size);free(compressed);return raw;
}
static void marker_contract(const char*path,const char*request){
 load(path);uint32_t n,actual_size,expected_size;
 uint8_t*png=operation("mark_tiles_and_chests_preview",request,&n),*actual=inflate_png(png,n,&actual_size);free(png);
 assert(sp->tile_records<513u*300u*4u);assert(sp->tile_scan_passes==3);assert(sp->input_copy_bytes==0);
 check(terra_world_close(world));check(terra_world_open_from_buffer(source,source_size,&world));
 char response[256];uint64_t required;check(terra_op_execute_json(world,"mark_tiles_and_chests_preview",request,response,sizeof(response),&required));
 uint32_t width,height;assert(terra_op_get_thumbnail_png(world,NULL,0,&required,&width,&height)==TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL);
 png=malloc(required);assert(png);check(terra_op_get_thumbnail_png(world,png,required,&required,&width,&height));
 uint8_t*expected=inflate_png(png,(uint32_t)required,&expected_size);assert(actual_size==expected_size&&!memcmp(actual,expected,actual_size));
 free(png);free(actual);free(expected);check(terra_world_close(world));free(source);source=NULL;world=0;
}
static void mixed_plan_contract(const char*path){
 load(path);uint32_t n;
 const char* first="{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"is_active\":false}}]}";
 const char* second="{\"rules\":[{\"where\":{\"type\":0},\"patch\":{\"wall\":1}}]}";
 char plan[2048];snprintf(plan,sizeof(plan),"{\"operations\":[{\"operation\":\"batch_update_tiles\",\"request\":%s},{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"worldName\":\"ordered\"}}},{\"operation\":\"batch_update_tiles\",\"request\":%s}]}",first,second);
 uint8_t*combined=operation("edit_plan",plan,&n);uint32_t combined_n=n;
 assert(sp->candidate_count==1&&sp->tile_scan_passes==1&&sp->tile_records==7u*270u);
 // Compare against separate stream transactions and adoption after each op.
 const char* names[]={"batch_update_tiles","header_patch","batch_update_tiles"};
 const char* requests[]={first,"{\"patch\":{\"worldName\":\"ordered\"}}",second};
 for(unsigned i=0;i<3;i++){
  uint32_t id=begin(names[i],requests[i]);uint8_t*next=pump(id,1,&n);
  if(i!=1)assert(sp->tile_scan_passes==2&&sp->tile_records==7u*270u*2u);
  check(terra_world_stream_adopt(id,1,hp));world=*hp;check(terra_world_stream_close(id));free(source);source=next;source_size=n;
 }
 assert(combined_n==source_size&&!memcmp(combined,source,source_size));free(combined);
 uint32_t baseline=tx_native_heap_used();
 const char*bad[]={"{\"operations\":[{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"worldName\":\"must rollback\"}}},{\"operation\":\"unknown\",\"request\":{}}]}",
 "{\"operations\":[{\"operation\":\"batch_update_tiles\",\"request\":{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":2},\"limit\":1}]}}]}"};
 for(unsigned i=0;i<2;i++){char*name=string("edit_plan"),*request=string(bad[i]);assert(terra_world_stream_operation_begin(world,name,request,hp)<0&&!*hp);tx_free((uint32_t)(uintptr_t)name);tx_free((uint32_t)(uintptr_t)request);assert(!strcmp(tx_get_world(world)->worldName,"ordered"));assert(tx_native_heap_used()==baseline);}
 check(terra_world_close(world));free(source);world=0;source=NULL;
}
static void reject_tile_rule(const char*request,int streaming){
 uint32_t baseline=tx_native_heap_used();TxWorld*w=tx_get_world(world);assert(w);
 if(streaming){
  char*name=string("batch_update_tiles"),*json=string(request);
  assert(terra_world_stream_operation_begin(world,name,json,hp)<0&&!*hp);
  tx_free((uint32_t)(uintptr_t)name);tx_free((uint32_t)(uintptr_t)json);
 }else{
  char response[256];uint64_t needed;
  assert(terra_op_execute_json(world,"batch_update_tiles",request,response,sizeof(response),&needed)!=TERRAX_WORLD_STATUS_OK);
 }
 assert(tx_get_world(world)==w&&tx_native_heap_used()==baseline);
 for(uint32_t i=0;i<TX_MAX_SECTION_OVERRIDES;i++)assert(!w->section_overrides[i].active);
}
static void tile_rule_validation_contract(const char*path){
 load(path);
 static const struct{const char*key;int maximum;} integers[]={
  {"type",65535},{"wall",65535},{"liquid_amount",255},{"liquid_type",4},
  {"brick_style",7},{"tile_color",255},{"wall_color",255}
 };
 static const char*booleans[]={"is_active","wire_red","wire_blue","wire_green","wire_yellow",
  "actuator","inactive","invisible_block","invisible_wall","fullbright_block","fullbright_wall"};
 static const char*bad_values[]={"\"1\"","1.5","true","[]","{}","-2"};
 static const char*bad_flags[]={"\"true\"","1.5","2","-2","[]","{}"};
 static const char*invalid[]={
  "{\"rules\":[42]}", "{\"rules\":[null]}", "{\"rules\":[{\"where\":[] ,\"patch\":{\"type\":2}}]}",
  "{\"rules\":[{\"where\":\"type=1\",\"patch\":{\"type\":2}}]}",
  "{\"rules\":[{\"where\":{\"type\":1},\"patch\":true}]}",
  "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":2},\"limit\":\"1\"}]}",
  "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":2},\"limit\":-1}]}",
  "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":2},\"limit\":2147483648}]}",
  "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":2}},{\"where\":{\"type\":\"2\"},\"patch\":{\"type\":1}}]}"
 };
 char request[512];uint32_t rejected=0;
 for(int streaming=1;streaming>=0;streaming--){
  if(!streaming){check(terra_world_close(world));check(terra_world_open_from_buffer(source,source_size,&world));}
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){reject_tile_rule(invalid[i],streaming);rejected++;}
  for(int patch=0;patch<2;patch++){
   const char*side=patch?"patch":"where";
   for(unsigned i=0;i<sizeof(integers)/sizeof(integers[0]);i++){
    for(unsigned v=0;v<sizeof(bad_values)/sizeof(bad_values[0]);v++){
     snprintf(request,sizeof(request),"{\"rules\":[{\"%s\":{\"%s\":%s}}]}",side,integers[i].key,bad_values[v]);
     reject_tile_rule(request,streaming);rejected++;
    }
    snprintf(request,sizeof(request),"{\"rules\":[{\"%s\":{\"%s\":%d}}]}",side,integers[i].key,integers[i].maximum+1);
    reject_tile_rule(request,streaming);rejected++;
   }
   for(unsigned i=0;i<sizeof(booleans)/sizeof(booleans[0]);i++)for(unsigned v=0;v<sizeof(bad_flags)/sizeof(bad_flags[0]);v++){
    snprintf(request,sizeof(request),"{\"rules\":[{\"%s\":{\"%s\":%s}}]}",side,booleans[i],bad_flags[v]);
    reject_tile_rule(request,streaming);rejected++;
   }
  }
 }
 // Rejected edits leave the exported world byte-identical, including a valid
 // first rule followed by an invalid second rule in the same transaction.
 uint32_t saved_size=0;
 check(terra_world_save_to_buffer(world,NULL,0,&saved_size));
 uint8_t*saved=malloc(saved_size);assert(saved);
 check(terra_world_save_to_buffer(world,saved,saved_size,&saved_size));
 assert(saved_size==source_size&&!memcmp(saved,source,source_size));free(saved);
 // Null/-1 sentinels and the representable WLD boundaries remain supported.
 const char*compatible="{\"rules\":[{\"where\":{\"type\":-1,\"wall\":null,\"wire_red\":-1},\"patch\":{\"type\":null,\"wire_red\":true,\"liquid_type\":4,\"brick_style\":7,\"tile_color\":255,\"wall\":65535},\"limit\":null},{\"where\":null,\"patch\":null}]}";
 TxTileRule*rules=NULL;uint32_t count=0;
 assert(tx_stream_parse_tile_rules(tx_get_world(world),compatible,(int)strlen(compatible),&rules,&count)>0&&count==2);
 assert(rules[0].type==-1&&rules[0].wall==-1&&rules[0].wire_red==-1&&rules[0].patch_type==-1&&
  rules[0].patch_wire_red==1&&rules[0].patch_liquid_type==4&&rules[0].patch_brick_style==7&&
  rules[0].patch_tile_color==255&&rules[0].patch_wall==65535&&rules[0].limit==0);
 tx_internal_free(rules);
 char response[512];uint64_t needed=0;
 check(terra_op_execute_json(world,"batch_update_tiles","{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":2}}]}",response,sizeof(response),&needed));
 assert(strstr(response,"\"total_updated\":945"));
 check(terra_world_close(world));free(source);world=0;source=NULL;
 printf("tile rule validation: %u invalid direct/stream requests rejected; original bytes and valid predicates preserved\n",rejected);
}
static void legacy_supply_contract(const char*path){
 load(path);uint32_t first_n,second_n;uint32_t id=begin("render_preview_png","{}");uint8_t*first=pump(id,0,&first_n);
 assert(sp->input_copy_bytes==sp->source_bytes&&sp->source_bytes>0);check(terra_world_stream_close(id));
 id=begin("render_preview_png","{}");uint8_t*second=pump(id,1,&second_n);assert(sp->input_copy_bytes==0);
 assert(first_n==second_n&&!memcmp(first,second,first_n));check(terra_world_stream_close(id));free(first);free(second);
 char*name=string("edit_plan"),*request=string("{\"operations\":[{\"operation\":\"save\",\"request\":{}}]}");
 TxWorld*w=tx_get_world(world);uint32_t version=w->version;w->version=TX_CURRENT_KNOWN_VERSION+1;
 assert(terra_world_stream_operation_begin(world,name,request,hp)<0&&!*hp);
 char error[512];uint64_t needed;terra_info_get_last_error_json(error,sizeof(error),&needed);assert(strstr(error,"TERRAX_FUTURE_VERSION_READ_ONLY"));w->version=version;
 tx_free((uint32_t)(uintptr_t)name);tx_free((uint32_t)(uintptr_t)request);
 check(terra_world_close(world));free(source);source=NULL;world=0;
}
static void faults_contract(const char*path){
 load(path);uint32_t baseline=tx_native_heap_used();char*name=string("render_preview_png"),*request=string("{}");
 txw_test_allocation_limit(1024u*1024u);assert(terra_world_stream_operation_begin(world,name,request,hp)<0&&!*hp);txw_test_allocation_limit(UINT32_MAX);
 assert(tx_get_world(world)&&tx_native_heap_used()==baseline);tx_free((uint32_t)(uintptr_t)name);tx_free((uint32_t)(uintptr_t)request);
 // Truncating a candidate footer must not replace the already open workspace.
 uint32_t truncated=source_size-1;check(terra_world_stream_open_begin(1,truncated,hp));uint32_t id=*hp;int failed=0;
 for(unsigned step=0;step<1000&&!failed;step++){
  int status=terra_world_stream_step(id,1,ep);if(status<0){failed=1;break;}
  assert(ep->kind!=TX_STREAM_READY);
  if(ep->kind==TX_STREAM_NEED_SOURCE){assert(ep->offset+ep->length<=truncated);check(terra_world_stream_acquire_input(id,lp));memcpy((void*)(uintptr_t)lp->data_ptr,source+lp->offset,lp->length);if(terra_world_stream_commit_input(id,lp->lease_id,1,lp->offset,lp->length)<0)failed=1;}
 }
 assert(failed&&tx_get_world(world));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 // A changed indexed source cannot smuggle malformed per-column RLE through
 // the cursor path; the caller contract still requires immutable source IDs.
 uint32_t at=get32(source+30);uint8_t old=source[at];source[at]=0xff;
 id=begin("render_preview_png","{}");failed=0;
 for(unsigned step=0;step<1000&&!failed;step++){
  if(terra_world_stream_step(id,1,ep)<0){failed=1;break;}
  if(ep->kind==TX_STREAM_NEED_SOURCE){check(terra_world_stream_acquire_input(id,lp));memcpy((void*)(uintptr_t)lp->data_ptr,source+lp->offset,lp->length);check(terra_world_stream_commit_input(id,lp->lease_id,1,lp->offset,lp->length));}
  else if(ep->kind==TX_STREAM_OUTPUT)check(terra_world_stream_ack_output(id));else assert(ep->kind!=TX_STREAM_READY);
 }
 assert(failed);check(terra_world_stream_close(id));source[at]=old;assert(tx_native_heap_used()==baseline);
 check(terra_world_close(world));world=0;free(source);source=NULL;
}
static void lease_contract(const char*path){FILE*f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);uint32_t n=ftell(f);fclose(f);
 check(terra_world_stream_open_begin(1,n,hp));uint32_t id=*hp;check(terra_world_stream_acquire_input(id,lp));uint32_t lease=lp->lease_id;
 assert(terra_world_stream_supply_source(id,1,0,bridge,lp->length)<0);check(terra_world_stream_release_input(id,lease));assert(terra_world_stream_release_input(id,lease)<0);
 check(terra_world_stream_acquire_input(id,lp));lease=lp->lease_id;
#ifndef __wasm__
 txw_test_stream_memory_bytes(65536);assert(terra_world_stream_commit_input(id,lease,1,0,lp->length)<0);
 assert(terra_world_stream_release_input(id,lease)<0);txw_test_stream_memory_bytes(0);check(terra_world_stream_acquire_input(id,lp));lease=lp->lease_id;
#endif
 check(terra_world_stream_cancel(id));assert(terra_world_stream_commit_input(id,lease,1,0,lp->length)<0);check(terra_world_stream_close(id));
 check(terra_world_stream_open_begin(1,n,hp));uint32_t next=*hp;assert(next!=id);assert(terra_world_stream_commit_input(next,lease,1,0,lp->length)<0);check(terra_world_stream_close(next));
}
#include "circuit_fragments_contract.inc"
#include "circuit_objects_contract.inc"
#include "circuit_supports_contract.inc"
int main(int argc,char**argv){assert(argc==2);
#if defined(__GLIBC__) && !defined(__wasm__)
 mallopt(M_MMAP_MAX,0);mallopt(M_MMAP_THRESHOLD,1024*1024*1024);
#endif
 hp=alloc(4);ep=alloc(sizeof(*ep));lp=alloc(sizeof(*lp));sp=alloc(sizeof(*sp));bridge=alloc(1024*1024);
 char circuit_path[1024];snprintf(circuit_path,sizeof(circuit_path),"%s/circuit-fragments.wld",argv[1]);circuit_fragments_contract(circuit_path);circuit_stamp_contract(circuit_path);
 circuit_objects_contract(argv[1]);circuit_supports_contract(argv[1]);circuit_anchor_edges_contract(argv[1]);circuit_falling_contract(argv[1]);
 char path[1024];snprintf(path,sizeof(path),"%s/high.wld",argv[1]);lease_contract(path);png_contract(path,0);
 snprintf(path,sizeof(path),"%s/noise.wld",argv[1]);png_contract(path,1);snprintf(path,sizeof(path),"%s/short-columns.wld",argv[1]);png_contract(path,2);snprintf(path,sizeof(path),"%s/small.wld",argv[1]);edit_contract(path);mixed_plan_contract(path);legacy_supply_contract(path);snprintf(path,sizeof(path),"%s/markers.wld",argv[1]);marker_contract(path,"{\"tile_markers\":[{\"tile_type\":1,\"locate\":0,\"radius\":3,\"line_width\":1,\"color\":\"#ff000080\"}]}");
 const char* marker_requests[]={"{\"tile_markers\":[{\"tile_type\":1,\"locate\":0,\"radius\":0,\"line_width\":0,\"color\":\"#ff000080\"}]}",
 "{\"tile_markers\":[{\"tile_type\":1,\"locate\":0,\"radius\":60,\"line_width\":15,\"color\":\"#ff000080\"},{\"tile_type\":2,\"locate\":0,\"radius\":3,\"line_width\":1,\"color\":\"#0000ff40\"}]}"};
 for(unsigned i=0;i<2;i++){snprintf(path,sizeof(path),"%s/marker-wide.wld",argv[1]);marker_contract(path,marker_requests[i]);snprintf(path,sizeof(path),"%s/marker-narrow.wld",argv[1]);marker_contract(path,marker_requests[i]);}
 snprintf(path,sizeof(path),"%s/small.wld",argv[1]);tile_rule_validation_contract(path);
 snprintf(path,sizeof(path),"%s/noise.wld",argv[1]);faults_contract(path);
 tx_free((uint32_t)(uintptr_t)bridge);tx_free((uint32_t)(uintptr_t)sp);tx_free((uint32_t)(uintptr_t)lp);tx_free((uint32_t)(uintptr_t)ep);tx_free((uint32_t)(uintptr_t)hp);puts("native stream boundaries passed");return 0;
}
