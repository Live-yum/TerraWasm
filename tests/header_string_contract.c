/* Long header strings must remain source spans, not TxWorld summaries.
 * Goldens are hand-encoded by generate-header-string-fixtures.js. */
#include "terra_world.h"
#include "terra_types.h"
#include "terra_header.h"
#include "terra_stream.h"
#include "terra_stream_map.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__) && defined(TERRAX_HEADER_STRING_STREAM_TESTS)
#include <malloc.h>
#endif

#ifndef TERRAX_HEADER_STRING_FIXTURE_DIR
#define TERRAX_HEADER_STRING_FIXTURE_DIR "build-native-stream/header-string-fixtures"
#endif
extern TxWorld *tx_get_world(uint32_t);
extern uint32_t tx_native_heap_used(void);
extern int json_find_key(const char*,int,const char*);
extern int json_skip_value(const char*,int,int);
extern void buf_init(TxBuf*,uint32_t);
extern void tx_internal_free(void*);
extern void tx_set_error(const char*,const char*);
extern char tx_last_error[];
extern uint32_t tx_heap_used(void);
extern void txw_test_live_allocation_limit(uint64_t);
extern void txw_test_map_runtime_write_header(TxBuf*,TxWorld*);

/* Inspect the owned candidate only to corrupt a header after the preceding
 * scan was accepted. No production hook or public ABI is needed to verify
 * the encoder's specific error survives the outer cooperative dispatch. */
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
#define event header_stream_emit
#define u32 header_stream_u32
#include "../src/terra_stream.c"
#undef u32
#undef event
#endif

static void check(int status){
    if(status){char message[512];uint64_t n=0;terra_info_get_last_error_json(message,sizeof message,&n);
        fprintf(stderr,"native status %d: %s\n",status,message);abort();}
}
static uint8_t *read_path(const char *path,uint32_t *length){
    FILE *file=fopen(path,"rb");assert(file);assert(!fseek(file,0,SEEK_END));
    long n=ftell(file);assert(n>=0&&n<UINT32_MAX);rewind(file);
    uint8_t *bytes=malloc((size_t)n+1u);assert(bytes);assert(fread(bytes,1,(size_t)n,file)==(size_t)n);fclose(file);
    bytes[n]=0;*length=(uint32_t)n;return bytes;
}
static uint8_t *read_fixture(const char *name,uint32_t *length){
    char path[2048];snprintf(path,sizeof path,"%s/%s",TERRAX_HEADER_STRING_FIXTURE_DIR,name);
    return read_path(path,length);
}
static void fixture_name(char *name,uint32_t version,const char *stage,const char *extension){
    snprintf(name,96,"v%u-%s.%s",version,stage,extension);
}
static char *section_json(uint32_t world,const char *section){
    uint64_t length=0;check(terra_section_get_json(world,section,NULL,0,&length));
    assert(length&&length<=UINT32_MAX);char *json=malloc((size_t)length);assert(json);
    check(terra_section_get_json(world,section,json,length,&length));return json;
}
static void same_json_value(const char *actual,const char *expected,const char *key){
    int alen=(int)strlen(actual),elen=(int)strlen(expected);
    int a=json_find_key(actual,alen,key),e=json_find_key(expected,elen,key);assert(a>=0&&e>=0);
    int ae=json_skip_value(actual,alen,a),ee=json_skip_value(expected,elen,e);assert(ae>a&&ee>e);
    if(ae-a!=ee-e||memcmp(actual+a,expected+e,(size_t)(ae-a))){
        fprintf(stderr,"JSON value mismatch: %s (%d vs %d encoded bytes)\n",key,ae-a,ee-e);abort();
    }
}
static uint32_t read_u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t assert_map_name(uint32_t world,const char *stem){
    char name[128];snprintf(name,sizeof name,"%s.map-name",stem);
    uint32_t length;uint8_t *expected=read_fixture(name,&length);TxWorld *w=tx_get_world(world);
    TxBuf header={0};buf_init(&header,256);txw_test_map_runtime_write_header(&header,w);
    if(!header.ok||header.len<24u+length+12u||memcmp(header.data+24u,expected,length)){
        fprintf(stderr,"MAP name mismatch: %s (expected %u encoded bytes)\n",stem,length);abort();
    }
    assert(read_u32(header.data+24u+length)==(uint32_t)w->worldId);
    assert(read_u32(header.data+28u+length)==(uint32_t)w->maxTilesY);
    assert(read_u32(header.data+32u+length)==(uint32_t)w->maxTilesX);
    uint32_t header_length=header.len;tx_internal_free(header.data);free(expected);return header_length;
}
static void assert_json(uint32_t world,const char *expected_name){
    uint32_t length;char *expected=(char*)read_fixture(expected_name,&length),*actual=section_json(world,"header");
    const char *keys[]={"worldName","seed","anglerWhoFinishedToday","manifestJson"};
    for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++)same_json_value(actual,expected,keys[i]);
    char *footer=section_json(world,"footer");same_json_value(footer,expected,"worldName");
    char stem[128];snprintf(stem,sizeof stem,"%s",expected_name);*strrchr(stem,'.')=0;assert_map_name(world,stem);
    free(footer);free(actual);free(expected);
}
static uint8_t *save(uint32_t world,uint32_t *length){
    check(terra_world_save_to_buffer(world,NULL,0,length));uint8_t *bytes=malloc(*length);assert(bytes);
    check(terra_world_save_to_buffer(world,bytes,*length,length));return bytes;
}
static void assert_bytes(const uint8_t *bytes,uint32_t length,const char *expected_name){
    uint32_t expected_length;uint8_t *expected=read_fixture(expected_name,&expected_length);
    if(length!=expected_length||memcmp(bytes,expected,length)){
        fprintf(stderr,"binary mismatch: %s (%u vs %u bytes)\n",expected_name,length,expected_length);abort();
    }
    free(expected);
}
static void assert_saved(uint32_t world,uint32_t version,const char *stage){
    char name[96];uint32_t length;uint8_t *bytes=save(world,&length);
    fixture_name(name,version,stage,"wld");assert_bytes(bytes,length,name);
    fixture_name(name,version,stage,"json");assert_json(world,name);
    free(bytes);
}
static int patch(uint32_t world,const char *request){
    char response[256];uint64_t length;
    return terra_op_execute_json(world,"header_patch",request,response,sizeof response,&length);
}
static void buffered_contract(uint32_t version){
    char name[96];fixture_name(name,version,"original","wld");uint32_t length,world;
    uint8_t *source=read_fixture(name,&length);check(terra_world_open_from_buffer(source,length,&world));
    assert_saved(world,version,"original");
    check(patch(world,"{\"patch\":{\"spawnTileX\":1}}"));assert_saved(world,version,"moved");
    // Second patch reads an override, whose absolute metadata offsets differ
    // from its borrowed source pointer. All long strings must survive again.
    check(patch(world,"{\"patch\":{\"spawnTileY\":2}}"));assert_saved(world,version,"twice");
    check(patch(world,"{\"patch\":{\"worldId\":2}}"));assert_saved(world,version,"identity");
    fixture_name(name,version,"patch","json");char *request=(char*)read_fixture(name,&length);
    check(patch(world,request));free(request);assert_saved(world,version,"changed");
    const char *invalid[]={"{\"patch\":{\"seed\":\"bad\\u0000\"}}",
        "{\"patch\":{\"worldName\":\"bad\\ud800\"}}",
        "{\"patch\":{\"worldName\":\"must rollback\",\"unknownField\":true}}"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
        assert(patch(world,invalid[i])!=0);assert_saved(world,version,"changed");
    }
    uint8_t *saved=save(world,&length);check(terra_world_close(world));free(source);
    check(terra_world_open_from_buffer(saved,length,&world));fixture_name(name,version,"changed","json");assert_json(world,name);
    check(terra_world_close(world));free(saved);assert(tx_native_heap_used()==0);
    printf("v%u long strings: complete JSON, exact binary, repeated override, identity/footer and rollback: ok\n",version);
}
static void padded_contract(void){
    uint32_t length,world;uint8_t *source=read_fixture("padded-original.wld",&length);
    check(terra_world_open_from_buffer(source,length,&world));assert_json(world,"padded-original.json");
    check(patch(world,"{\"patch\":{\"spawnTileX\":1}}"));
    uint8_t *bytes=save(world,&length);assert_bytes(bytes,length,"padded-moved.wld");
    assert_json(world,"padded-original.json");free(bytes);free(source);check(terra_world_close(world));
    assert(tx_native_heap_used()==0);
}
static void bounded_view_contract(void){
    uint8_t data[]={0xaa,0xbb,5,'a','b','c','d','e',0xff};TxWorld world={0};TxHeaderString view;
    world.file=data;world.file_len=sizeof data;world.pointer_count=1;world.starts[0]=2;world.ends[0]=8;
    assert(tx_header_string_view(&world,0,&view)&&view.len==5&&view.prefix_len==1&&view.encoded==data+2);
    assert(!tx_header_string_view(&world,UINT32_MAX,&view));world.ends[0]=7;
    assert(!tx_header_string_view(&world,0,&view));world.ends[0]=10;
    assert(!tx_header_string_view(&world,0,&view));world.ends[0]=8;
    uint8_t bad[]={0x80,0x80,0x80,0x80,0x10};world.section_overrides[0].active=1;
    world.section_overrides[0].data=bad;world.section_overrides[0].len=sizeof bad;
    assert(!tx_header_string_view(&world,0,&view));world.section_overrides[0].data=NULL;
    assert(!tx_header_string_view(&world,0,&view));
}

static void map_error_contract(void){
    uint32_t length,world;uint8_t *source=read_fixture("map-growth-original.wld",&length);
    check(terra_world_open_from_buffer(source,length,&world));TxWorld *w=tx_get_world(world);
    uint32_t baseline=tx_native_heap_used();
    tx_set_error("TERRAX_NOT_FOUND","deliberately stale error");
    /* Aggregate headroom also covers realloc: tables + initial 4096-byte
     * header fit in 5000 bytes; the 8192-byte header cannot. */
    txw_test_live_allocation_limit((uint64_t)tx_heap_used()+5000u);
    char response[256];uint64_t response_length;
    assert(terra_op_execute_json(world,"render_lit_map","{}",response,sizeof response,&response_length)!=0);
    txw_test_live_allocation_limit(UINT64_MAX);
    if(!strstr(tx_last_error,"TERRAX_WASM_OOM")||!strstr(tx_last_error,"failed to allocate header encoder")){
        fprintf(stderr,"unexpected header-growth error: %s\n",tx_last_error);abort();
    }
    assert(tx_native_heap_used()==baseline);
    uint32_t end=w->ends[0];w->ends[0]=w->starts[0]+1u;
    assert(terra_generate_map(w)<0&&strstr(tx_last_error,"TERRAX_STATE_ERROR"));
    assert(!tx_stream_map_begin(w,NULL,0,NULL,0)&&strstr(tx_last_error,"TERRAX_STATE_ERROR"));
    assert(tx_native_heap_used()==baseline);w->ends[0]=end;
    uint8_t malformed[]={0x80};uint32_t count=w->pointer_count;w->pointer_count=0;
    w->section_overrides[0].active=1;w->section_overrides[0].data=malformed;w->section_overrides[0].len=1;
    assert(terra_generate_map(w)<0&&strstr(tx_last_error,"TERRAX_STATE_ERROR"));
    assert(!tx_stream_map_begin(w,NULL,0,NULL,0)&&strstr(tx_last_error,"TERRAX_STATE_ERROR"));
    memset(&w->section_overrides[0],0,sizeof w->section_overrides[0]);w->pointer_count=count;
    assert(tx_native_heap_used()==baseline);check(terra_world_close(world));free(source);
    /* The deliberate internal, no-header compatibility path stays usable. */
    TxWorld synthetic={0};TxBuf header={0};buf_init(&header,4096);
    txw_test_map_runtime_write_header(&header,&synthetic);
    assert(header.ok&&!memcmp(header.data+24,"\5World",6));tx_internal_free(header.data);
    assert(tx_native_heap_used()==0);
    puts("MAP malformed spans: STATE; real 4096->8192 header growth failure: OOM; synthetic fallback: ok");
}

static void legacy_name_contract(uint32_t version){
    char name[128],stem[96];uint32_t length,world;
    snprintf(stem,sizeof stem,"legacy-v%u-original",version);snprintf(name,sizeof name,"%s.wld",stem);
    uint8_t *source=read_fixture(name,&length);check(terra_world_open_from_buffer(source,length,&world));
    const char *stages[]={"original","changed","twice"};
    for(unsigned i=0;i<3;i++){
        snprintf(stem,sizeof stem,"legacy-v%u-%s",version,stages[i]);
        if(i){snprintf(name,sizeof name,"%s.patch.json",stem);char *request=(char*)read_fixture(name,&length);check(patch(world,request));free(request);}
        snprintf(name,sizeof name,"%s.json",stem);char *expected=(char*)read_fixture(name,&length),*actual=section_json(world,"header");
        same_json_value(actual,expected,"worldName");same_json_value(actual,expected,"worldId");free(expected);free(actual);
        snprintf(name,sizeof name,"%s.footer.json",stem);expected=(char*)read_fixture(name,&length);actual=section_json(world,"footer");
        same_json_value(actual,expected,"worldName");same_json_value(actual,expected,"worldId");same_json_value(actual,expected,"valid");
        if(version<7)same_json_value(actual,expected,"present");free(expected);free(actual);
        uint8_t *bytes=save(world,&length);snprintf(name,sizeof name,"%s.wld",stem);assert_bytes(bytes,length,name);free(bytes);
    }
    uint8_t *bytes=save(world,&length);check(terra_world_close(world));free(source);
    check(terra_world_open_from_buffer(bytes,length,&world));char *actual=section_json(world,"footer");
    snprintf(name,sizeof name,"%s.footer.json",stem);char *expected=(char*)read_fixture(name,&length);
    same_json_value(actual,expected,"worldName");same_json_value(actual,expected,"worldId");
    free(actual);free(expected);free(bytes);check(terra_world_close(world));assert(tx_native_heap_used()==0);
    printf("v%u full legacy footer: original tuple, two identity overrides, exact WLD and reopen: ok\n",version);
}

#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
extern uint32_t tx_malloc(uint32_t);
extern void tx_free(uint32_t);
static uint32_t *handle;
static TxStreamEvent *event;
static TxStreamInputLease *lease;
static void *bridge_alloc(uint32_t length){uint32_t pointer=tx_malloc(length);assert(pointer);return (void*)(uintptr_t)pointer;}
static int stream_begin(uint32_t world,const char *operation,const char *request){
    char *name=bridge_alloc((uint32_t)strlen(operation)+1),*json=bridge_alloc((uint32_t)strlen(request)+1);
    strcpy(name,operation);strcpy(json,request);
    int status=terra_world_stream_operation_begin(world,name,json,handle);
    tx_free((uint32_t)(uintptr_t)name);tx_free((uint32_t)(uintptr_t)json);return status;
}
static uint8_t *pump(uint32_t task,const uint8_t *source,uint32_t source_len,uint32_t *out_len){
    uint8_t *output=NULL;uint32_t capacity=0;*out_len=0;
    for(uint32_t steps=0;steps<10000000;steps++){
        check(terra_world_stream_step(task,1024,event));
        if(event->kind==TX_STREAM_NEED_SOURCE){
            assert(event->source_id==1&&event->offset<=source_len&&event->length<=source_len-event->offset);
            check(terra_world_stream_acquire_input(task,lease));
            memcpy((void*)(uintptr_t)lease->data_ptr,source+event->offset,event->length);
            check(terra_world_stream_commit_input(task,lease->lease_id,1,lease->offset,lease->length));
        }else if(event->kind==TX_STREAM_OUTPUT){
            uint32_t end=event->offset+event->length;assert(end>=event->offset);
            if(end>capacity){capacity=end;output=realloc(output,capacity);assert(output);}
            memcpy(output+event->offset,(void*)(uintptr_t)event->data_ptr,event->length);
            check(terra_world_stream_ack_output(task));
        }else if(event->kind==TX_STREAM_READY){*out_len=event->result_size;return output;}
        else assert(event->kind==TX_STREAM_MORE);
    }
    assert(!"stream task did not finish");return NULL;
}
static void complete_map_contract(uint32_t world,const uint8_t *source,uint32_t source_length){
    assert(terra_generate_map(tx_get_world(world))>0);uint32_t expected_length=tx_last_len;
    uint8_t *expected=malloc(expected_length);assert(expected);
    uint8_t *result=(uint8_t*)(uintptr_t)tx_last_ptr;memcpy(expected,result,expected_length);tx_internal_free(result);
    check(stream_begin(world,"render_lit_map","{}"));uint32_t task=*handle,length;
    uint8_t *actual=pump(task,source,source_length,&length);check(terra_world_stream_close(task));
    assert(length==expected_length&&!memcmp(actual,expected,length));free(actual);free(expected);
}
static void stream_map_error_contract(void){
    uint32_t length,world;uint8_t *source=read_fixture("map-ascii-160-original.wld",&length);
    check(terra_world_open_from_buffer(source,length,&world));uint32_t baseline=tx_native_heap_used();
    check(stream_begin(world,"render_lit_map","{}"));uint32_t task=*handle;
    assert(current&&current->stage==OP_SCAN);current->candidate->ends[0]=current->candidate->starts[0]+1u;
    tx_set_error("TERRAX_NOT_FOUND","deliberately stale error before media creation");
    int status=0;
    for(unsigned steps=0;steps<10000;steps++){
        status=terra_world_stream_step(task,1024,event);if(status<0)break;
        if(event->kind==TX_STREAM_NEED_SOURCE){
            assert(event->offset<=length&&event->length<=length-event->offset);
            check(terra_world_stream_acquire_input(task,lease));
            memcpy((void*)(uintptr_t)lease->data_ptr,source+event->offset,event->length);
            check(terra_world_stream_commit_input(task,lease->lease_id,1,lease->offset,lease->length));
        }else assert(event->kind==TX_STREAM_MORE);
    }
    assert(status<0&&strstr(tx_last_error,"TERRAX_STATE_ERROR"));
    check(terra_world_stream_cancel(task));check(terra_world_stream_close(task));
    assert(tx_native_heap_used()==baseline);check(terra_world_close(world));free(source);
    puts("stream OP_SCAN -> MAP: specific malformed-header STATE survives dispatch and cancel releases candidate: ok");
}
static uint32_t stream_open(const uint8_t *source,uint32_t length){
    check(terra_world_stream_open_begin(1,length,handle));uint32_t task=*handle,n;
    assert(!pump(task,source,length,&n));check(terra_world_stream_adopt(task,1,handle));
    uint32_t world=*handle;check(terra_world_stream_close(task));return world;
}
static void stream_contract(uint32_t version){
    char name[96];fixture_name(name,version,"original","wld");uint32_t length,n;
    uint8_t *source=read_fixture(name,&length);uint32_t world=stream_open(source,length);
    fixture_name(name,version,"original","json");assert_json(world,name);
    const char *plan="{\"operations\":["
        "{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"spawnTileX\":1}}},"
        "{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"spawnTileY\":2}}},"
        "{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"worldId\":2}}}]}";
    check(stream_begin(world,"edit_plan",plan));uint32_t task=*handle;
    uint8_t *bytes=pump(task,source,length,&n);fixture_name(name,version,"identity","wld");assert_bytes(bytes,n,name);
    check(terra_world_stream_close(task));fixture_name(name,version,"original","json");assert_json(world,name);
    uint32_t baseline=tx_native_heap_used();
    const char *invalid="{\"operations\":[{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"worldName\":\"discard\"}}},{\"operation\":\"header_patch\",\"request\":{\"patch\":{\"unknownField\":true}}}]}";
    assert(stream_begin(world,"edit_plan",invalid)<0&&!*handle);
    assert(tx_native_heap_used()==baseline);assert_json(world,name);
    check(stream_begin(world,"header_patch","{\"patch\":{\"spawnTileX\":1}}"));
    task=*handle;check(terra_world_stream_cancel(task));check(terra_world_stream_close(task));
    assert(tx_native_heap_used()==baseline);assert_json(world,name);
    check(terra_world_close(world));free(source);
    check(terra_world_open_from_buffer(bytes,n,&world));fixture_name(name,version,"identity","json");assert_json(world,name);
    check(terra_world_close(world));free(bytes);assert(tx_native_heap_used()==0);
    printf("v%u streaming edit_plan: exact binary, complete reopen, cancel and rollback: ok\n",version);
}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t skip_string(const uint8_t *p,uint32_t length,uint32_t offset){
    uint32_t n=0,shift=0;uint8_t byte;
    do{assert(offset<length&&shift<=28);byte=p[offset++];n|=(uint32_t)(byte&127)<<shift;shift+=7;}while(byte&128);
    assert(n<=length-offset);return offset+n;
}
static void external_contract(const char *path){
    uint32_t length,n;uint8_t *source=read_path(path,&length);uint32_t world=stream_open(source,length);
    TxWorld *w=tx_get_world(world);assert(w&&w->version>=209&&w->version<=326);
    uint32_t offset=skip_string(source,length,w->starts[0]);offset=skip_string(source,length,offset)+8+16+7*4+4;
    const uint32_t flags[]={222,227,238,239,241,249,266,267,302};
    for(unsigned i=0;i<sizeof flags/sizeof flags[0];i++)if(w->version>=flags[i])offset++;
    offset+=8;if(w->version>=284)offset+=8;offset+=1+(3+4+3+4+3)*4;
    assert(offset+4<=length&&u32(source+offset)==(uint32_t)w->spawnTileX&&w->spawnTileX+1<w->maxTilesX);
    uint32_t next=(uint32_t)w->spawnTileX+1;char *request=malloc(96);assert(request);
    snprintf(request,96,"{\"patch\":{\"spawnTileX\":%u}}",next);
    check(stream_begin(world,"header_patch",request));uint32_t task=*handle;
    uint8_t *bytes=pump(task,source,length,&n);check(terra_world_stream_close(task));
    assert(n==length&&u32(bytes+offset)==next&&!memcmp(source,bytes,offset)&&!memcmp(source+offset+4,bytes+offset+4,length-offset-4));
    TxHeaderString name,seed;assert(tx_header_string_view(w,0,&name));assert(tx_header_string_view(w,name.prefix_len+name.len,&seed));
    printf("external WLD v%u %ux%u: %u bytes preserved except spawn X's 4-byte field; original seed %u bytes retained\n",w->version,w->maxTilesX,w->maxTilesY,n,seed.len);
    check(terra_world_close(world));check(terra_world_open_from_buffer(bytes,n,&world));
    assert((uint32_t)tx_get_world(world)->spawnTileX==next);check(terra_world_close(world));
    free(source);free(bytes);free(request);assert(tx_native_heap_used()==0);
}
#endif

static void map_names_contract(void){
    const char *labels[]={"ascii-160","utf8-boundary","short","empty","growth"};
    const char *stages[]={"original","changed","twice"};
    for(unsigned i=0;i<sizeof labels/sizeof labels[0];i++){
        char name[128],stem[96];snprintf(stem,sizeof stem,"map-%s-original",labels[i]);snprintf(name,sizeof name,"%s.wld",stem);
        uint32_t source_length,length,world;uint8_t *source=read_fixture(name,&source_length);
        check(terra_world_open_from_buffer(source,source_length,&world));
        for(unsigned j=0;j<3;j++){
            snprintf(stem,sizeof stem,"map-%s-%s",labels[i],stages[j]);
            if(j){snprintf(name,sizeof name,"%s.patch.json",stem);char *request=(char*)read_fixture(name,&length);check(patch(world,request));free(request);}
            assert_map_name(world,stem);
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
            complete_map_contract(world,source,source_length);
#endif
            uint8_t *bytes=save(world,&length);snprintf(name,sizeof name,"%s.wld",stem);assert_bytes(bytes,length,name);free(bytes);
        }
        check(terra_world_close(world));free(source);assert(tx_native_heap_used()==0);
        printf("MAP %s: independent canonical bytes, two active overrides, exact WLD",labels[i]);
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
        printf(", complete buffered/stream comparison");
#endif
        puts(": ok");
    }
}

int main(int argc,char **argv){
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
#if defined(__GLIBC__)
    mallopt(M_MMAP_MAX,0);
#endif
    handle=bridge_alloc(sizeof *handle);event=bridge_alloc(sizeof *event);lease=bridge_alloc(sizeof *lease);
#endif
    if(argc==2){
        if(!strcmp(argv[1],"--map-only"))map_names_contract();
        else if(!strcmp(argv[1],"--legacy-footer-only"))legacy_name_contract(7);
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
        else external_contract(argv[1]);
#else
        else assert(!"unknown contract argument");
#endif
        goto finished;
    }
    bounded_view_contract();
    map_names_contract();map_error_contract();
    const uint32_t legacy_versions[]={6,7,37,58,87};
    for(unsigned i=0;i<sizeof legacy_versions/sizeof legacy_versions[0];i++)legacy_name_contract(legacy_versions[i]);
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
    stream_map_error_contract();
#endif
    const uint32_t versions[]={88,179,293,326};
    for(unsigned i=0;i<sizeof versions/sizeof versions[0];i++){
        buffered_contract(versions[i]);
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
        stream_contract(versions[i]);
#endif
    }
    padded_contract();
finished:
#ifdef TERRAX_HEADER_STRING_STREAM_TESTS
    tx_free((uint32_t)(uintptr_t)handle);tx_free((uint32_t)(uintptr_t)event);tx_free((uint32_t)(uintptr_t)lease);
#endif
    puts("long header strings and non-canonical length prefixes: ok");return 0;
}
