#include "terra_world.h"
#include "terra_types.h"
#include "terra_commands.h"
#include "terra_stream.h"
#include "terra_wld_guard_task.h"
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TERRAX_TEST_FIXTURE_DIR
#error missing fixture directory
#endif
extern TxWorld* tx_get_world(uint32_t);
extern int set_section_override_data(TxWorld*,int,uint8_t*,uint32_t);
extern int txw_queue_pixel_art(uint32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,int32_t);
extern int txw_begin_pixel_art_indexed(uint32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t);
extern int txw_add_pixel_art_chunk(uint32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t);
extern int txw_add_pixel_art_chunks_bulk(uint32_t,uint32_t,uint32_t,uint32_t);
extern int txw_add_pixel_art_chunks_bulk_fast(uint32_t,uint32_t,uint32_t,uint32_t);
extern int txw_apply_pixel_art(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,int32_t,int32_t,int32_t,int32_t,uint32_t,uint32_t);

static uint32_t get32(const uint8_t* p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static uint8_t* read_fixture(uint32_t* n){
    FILE*f=fopen(TERRAX_TEST_FIXTURE_DIR "/files/1.wld","rb");assert(f);assert(!fseek(f,0,SEEK_END));long z=ftell(f);assert(z>0&&z<200000000);rewind(f);
    uint8_t*b=malloc((size_t)z+32);assert(b);assert(fread(b,1,(size_t)z,f)==(size_t)z);fclose(f);*n=(uint32_t)z;assert(get32(b)==326);return b;
}
static void rejected(const uint8_t*b,uint32_t n){uint32_t h=0;int status=terra_world_open_from_buffer(b,n,&h);if(status==0){fprintf(stderr,"unexpected open success at %u bytes\n",n);terra_world_close(h);}assert(status!=0&&h==0);}
static void unchanged(uint32_t h,const uint8_t*b,uint32_t n){
    uint32_t required=0;assert(terra_world_save_to_buffer(h,NULL,0,&required)==0&&required==n);uint8_t*out=malloc(n);assert(out);
    assert(terra_world_save_to_buffer(h,out,n,&required)==0&&required==n&&!memcmp(b,out,n));free(out);
}
static void assert_readonly(void){char e[512];uint64_t n=0;terra_info_get_last_error_json(e,sizeof e,&n);assert(strstr(e,"TERRAX_FUTURE_VERSION_READ_ONLY"));}
static void metadata_budget(const uint8_t* source,uint32_t n){
    /* Worst short-string density: 15 MiB of empty bestiary sightings within
     * the 16 MiB metadata budget. Measure the synchronous task-begin scan. */
    const uint32_t sight_count=15u*1024u*1024u,section_size=sight_count+12u;
    uint32_t begin=get32(source+26+8*4),end=get32(source+26+9*4),old=end-begin;
    uint32_t total=n-old+section_size;
    uint8_t* bytes=malloc(total);assert(bytes);memcpy(bytes,source,begin);
    memset(bytes+begin,0,section_size);put32(bytes+begin+4,sight_count);
    memcpy(bytes+begin+section_size,source+end,n-end);put32(bytes,328);
    for(uint32_t i=9;i<11;i++)put32(bytes+26+i*4,get32(source+26+i*4)-old+section_size);
    extern int parse_format(TxWorld*);
    TxWorld w={0};TxWldGuardTask task={0};w.file=bytes;w.file_len=total;assert(parse_format(&w));
    struct timespec start,finish;timespec_get(&start,TIME_UTC);
    assert(tx_wld_guard_task_begin(&w,&task));timespec_get(&finish,TIME_UTC);
    double seconds=(double)(finish.tv_sec-start.tv_sec)+(double)(finish.tv_nsec-start.tv_nsec)/1e9;
    printf("future metadata task begin: %u empty strings, %.6f seconds (not viewer end-to-end)\n",sight_count,seconds);
    /* Inverse/oversized count must fail structurally, without trusting it as
     * an allocation size or walking outside its own section. */
    put32(bytes+begin+4,0xffffffffu);assert(!tx_validate_future_sections(&w));
    free(bytes);
}
int main(void){
    uint32_t n,h=0;uint8_t*source=read_fixture(&n),*b=malloc(n+32);assert(b);
    assert(terra_world_open_from_buffer(source,n,&h)==0);unchanged(h,source,n);assert(terra_world_close(h)==0);
    memcpy(b,source,n);put32(b,328);assert(terra_world_open_from_buffer(b,n,&h)==0);
    char format[32000];uint64_t required=0;assert(terra_section_get_json(h,"format",format,sizeof format,&required)==0);
    assert(strstr(format,"\"originalVersion\":328")&&strstr(format,"\"readOnly\":true"));
    const char*ops[]={"header_patch","replace_chests","replace_bestiary","batch_update_tiles"};
    for(unsigned i=0;i<4;i++){assert(terra_op_execute_json(h,ops[i],"{}",NULL,0,&required)==TERRAX_WORLD_STATUS_NOT_SUPPORTED);assert_readonly();}
    assert(terra_world_apply_commands(h,NULL,0)==TERRAX_WORLD_STATUS_NOT_SUPPORTED);assert_readonly();
    assert(txw_queue_pixel_art(h,0,0,0,0,0,0,0,0,0)<0);assert_readonly();
    assert(txw_begin_pixel_art_indexed(h,0,0,0,0,0,0,0,0,0,0,0,0,0)<0);assert_readonly();
    assert(txw_add_pixel_art_chunk(h,0,0,0,0,0)<0);assert_readonly();
    assert(txw_add_pixel_art_chunks_bulk(h,0,0,0)<0);assert_readonly();
    assert(txw_add_pixel_art_chunks_bulk_fast(h,0,0,0)<0);assert_readonly();
    assert(txw_apply_pixel_art(h,0,0,0,0,0,0,0,0,0,0,0,0)<0);assert_readonly();
    assert(terra_world_stream_pixel_begin(h,NULL,NULL)<0);assert_readonly();
    assert(!set_section_override_data(tx_get_world(h),0,NULL,0));assert_readonly();
    unchanged(h,b,n);assert(terra_world_close(h)==0);
    put32(b,INT32_MAX);assert(terra_world_open_from_buffer(b,n,&h)==0);unchanged(h,b,n);assert(terra_world_close(h)==0);
    put32(b,328);memcpy(b+4,"xindong",7);assert(terra_world_open_from_buffer(b,n,&h)==0);unchanged(h,b,n);assert(terra_world_close(h)==0);
    const uint32_t cuts[]={0,1,3,4,16,24,25,26,69,100,166};
    memcpy(b,source,n);put32(b,328);for(unsigned i=0;i<sizeof cuts/sizeof cuts[0];i++)rejected(b,cuts[i]);
#define RESET() do{memcpy(b,source,n);put32(b,328);}while(0)
    RESET();put32(b,0);rejected(b,n);RESET();put32(b,0xffffffff);rejected(b,n);
    RESET();b[4]='X';rejected(b,n);RESET();b[11]=3;rejected(b,n);
    RESET();b[24]=0;rejected(b,n);RESET();b[24]=12;rejected(b,n);
    RESET();put32(b+26,get32(b+26)-1);rejected(b,n);
    RESET();put32(b+26,get32(b+26)+1);rejected(b,n);
    RESET();put32(b+34,get32(b+30)-1);rejected(b,n);
    RESET();memset(b+get32(b+26),255,5);rejected(b,n);
    RESET();uint32_t tile=get32(b+30);b[tile]=128;b[tile+1]=255;b[tile+2]=127;rejected(b,n);
    RESET();uint32_t chest=get32(b+34);b[chest]=255;b[chest+1]=255;rejected(b,n);
    /* Insert an unconsumed byte at each section boundary while repairing all
     * later pointers. This catches "bounds safe but wrong layout" acceptance. */
    for(unsigned section=0;section<11;section++){
        RESET();uint32_t at=section==10?n:get32(source+26+4*(section+1));
        memmove(b+at+1,b+at,n-at);b[at]=127;
        for(unsigned j=section+1;j<11;j++)put32(b+26+j*4,get32(source+26+j*4)+1);
        rejected(b,n+1);
    }
    RESET();uint32_t te=get32(b+26+5*4),end=get32(b+26+6*4);assert(end-te==4);
    memmove(b+end+9,b+end,n-end);memset(b+end,0,9);b[end]=99;put32(b+te,1);
    for(unsigned j=6;j<11;j++)put32(b+26+j*4,get32(source+26+j*4)+9);rejected(b,n+9);
    metadata_budget(source,n);
    free(b);free(source);puts("future WLD: same layout, original bytes, all mutators and malformed structure passed");return 0;
}
