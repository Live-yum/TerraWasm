/* Independent pre-88 source-layout fixture. No TerraWasm encoder is used.
 * This exercises Release libc paths as well as every historical open/save gate. */
#include "terra_world.h"
#include "terra_types.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern TxWorld *tx_get_world(uint32_t);
extern uint8_t *tx_alloc(uint32_t);
static uint8_t bytes[16384];static uint32_t length,header_end,tile_end,chest_end,npc_start,footer_start,wall_offset;
static void byte(uint32_t n){assert(length<sizeof bytes);bytes[length++]=(uint8_t)n;}
static void zero(uint32_t n){while(n--)byte(0);}
static void i16(uint32_t n){byte(n);byte(n>>8);}
static void i32(uint32_t n){for(unsigned i=0;i<4;i++)byte(n>>(i*8));}
static void text(const char*s){uint32_t n=(uint32_t)strlen(s);assert(n<128);byte(n);while(*s)byte((uint8_t)*s++);}
static void fixture(uint32_t v){
    length=0;i32(v);text("Legacy World");i32(87);
    i32(0);i32(1);i32(0);i32(1);i32(1);i32(1);
    if(v>=63)byte(2);if(v>=44)zero(28);if(v>=60){zero(32);if(v>=61)zero(8);}
    zero(8+24);byte(1);zero(4+1);if(v>=70)byte(0);
    zero(8);if(v>=56)byte(0);zero(3);if(v>=66)byte(0);if(v>=44)zero(4);if(v>=64)zero(2);
    if(v>=29){zero(2);if(v>=34){byte(0);if(v>=80)byte(0);}byte(0);}
    if(v>=32)byte(0);if(v>=37)byte(0);if(v>=56)byte(0);zero(3);if(v>=23)zero(5);
    zero(12+8);if(v>=53)zero(9);if(v>=54){i32(107);i32(108);i32(111);}
    if(v>=55)zero(3);if(v>=60)zero(9);if(v>=62)zero(6);
    header_end=length;
    byte(1);if(v<=77)byte(3);else i16(3);i16(12);i16(34);
    if(v>=48)byte(0);if(v<=25)byte(0);wall_offset=length;zero(2);if(v>=33)byte(0);if(v>=43)zero(2);
    if(v>=41){byte(0);if(v>=49)byte(0);}if(v>=42)zero(2);if(v>=25)i16(0);
    tile_end=length;
    byte(1);zero(8);if(v>=85)text("Chest");
    uint32_t slots=v<58?20:40;
    for(uint32_t i=0;i<slots;i++){if(v<59)byte(i?0:3);else i16(i?0:3);if(!i){if(v>=38)i32(1);else text("Dirt");if(v>=36)byte(0);}}
    zero(999);chest_end=length;byte(1);text("Legacy sign");zero(8);zero(999);npc_start=length;
    byte(1);text("Guide");if(v>=83)text("Guide");i32(0x3f800000);i32(0x40000000);zero(10);
    if(v>=31&&v<=83){uint32_t n=9+(v>=35)+(v>=65?8:0)+(v>=79);for(uint32_t i=0;i<n;i++)text(i==4?"Old Guide":"");}
    footer_start=length;if(v>=7){byte(1);text("Legacy World");i32(87);}
}
static uint8_t *save(uint32_t h,uint32_t *size){
    assert(terra_world_save_to_buffer(h,NULL,0,size)==0);uint8_t*out=malloc(*size);assert(out);
    assert(terra_world_save_to_buffer(h,out,*size,size)==0);return out;
}
static void unchanged(uint32_t h){uint32_t n;uint8_t*out=save(h,&n);assert(n==length&&!memcmp(out,bytes,n));free(out);}
static void section(uint32_t h,const char *name,char *out,uint32_t capacity){
    uint64_t n;assert(terra_section_get_json(h,name,out,capacity,&n)==0);
}
static int operation(uint32_t h,const char *name,const char *request){
    uint64_t n;char response[256];return terra_op_execute_json(h,name,request,response,sizeof response,&n);
}
static const char *chest_request(uint32_t v,uint32_t stack,uint32_t prefix,const char *name,const char *legacy_name){
    static char json[2048];uint32_t slots=v<58?20:40;
    int n=snprintf(json,sizeof json,"{\"chests\":[{\"x\":0,\"y\":0,\"name\":\"%s\",\"maxItems\":%u,\"items\":[{\"stack\":%u,\"itemType\":%u,\"prefix\":%u",name,slots,stack,v<38?0:1,prefix);
    if(v<38)n+=snprintf(json+n,sizeof json-(size_t)n,",\"legacyName\":\"%s\"",legacy_name);
    n+=snprintf(json+n,sizeof json-(size_t)n,"}");
    for(uint32_t i=1;i<slots;i++)n+=snprintf(json+n,sizeof json-(size_t)n,",null");
    snprintf(json+n,sizeof json-(size_t)n,"]}]}");return json;
}
int main(void){
    const char *fixture_dir=getenv("TERRAX_LEGACY_FIXTURE_DIR");
    if(fixture_dir)for(uint32_t v=58;v<=87;v+=29){
        fixture(v);char filename[1024];snprintf(filename,sizeof filename,"%s/legacy-v%u.wld",fixture_dir,v);
        FILE *file=fopen(filename,"wb");assert(file);assert(fwrite(bytes,1,length,file)==length);assert(fclose(file)==0);
    }
    /* Volatile calls prevent the test compiler from eliding bulk operations. */
    void*(*volatile set)(void*,int,size_t)=memset;
    void*(*volatile copy)(void*,const void*,size_t)=memcpy;
    uint8_t a[8192],b[8192];set(a,0x5a,sizeof a);copy(b,a,sizeof a);assert(!memcmp(a,b,sizeof a));
    for(uint32_t v=1;v<=87;v++){
        fixture(v);uint32_t h=0,n=0;int status=terra_world_open_from_buffer(bytes,length,&h);
        if(status){char error[512];uint64_t z;terra_info_get_last_error_json(error,sizeof error,&z);fprintf(stderr,"v%u: %s\n",v,error);}assert(status==0&&h);
        unchanged(h);
        assert(operation(h,"header_patch","{\"patch\":{\"bloodMoon\":true,\"boughtCat\":true}}")!=0);unchanged(h);
        assert(operation(h,"header_patch","{\"patch\":{\"maxTilesX\":2}}")!=0);unchanged(h);
        assert(operation(h,"replace_chests",chest_request(v,v<59?256:32768,0,v>=85?"Chest":"","Dirt"))!=0);unchanged(h);
        if(v<85){assert(operation(h,"replace_chests",chest_request(v,7,0,"Cannot store this name","Dirt"))!=0);unchanged(h);}
        if(v<36){assert(operation(h,"replace_chests",chest_request(v,7,1,"","Dirt"))!=0);unchanged(h);}
        if(v<38){assert(operation(h,"replace_chests",chest_request(v,7,0,"",""))!=0);unchanged(h);}
        char expected[8192],actual[8192];section(h,"chests",expected,sizeof expected);
        char *stack=strstr(expected,"\"stack\":3");assert(stack);stack[8]='7';
        assert(operation(h,"header_patch","{\"patch\":{\"worldName\":\"Patched Legacy\",\"worldId\":123,\"time\":12.5,\"bloodMoon\":true}}") == 0);
        assert(operation(h,"replace_chests",chest_request(v,7,0,v>=85?"Chest":"","Dirt"))==0);
        section(h,"chests",actual,sizeof actual);assert(!strcmp(actual,expected));
        char roundtrip[sizeof expected+32];snprintf(roundtrip,sizeof roundtrip,"{\"chests\":%s}",expected);
        assert(operation(h,"replace_chests",roundtrip)==0);
        uint8_t*out=save(h,&n);uint32_t delta=2;
        assert(n==length+delta+(v>=7?delta:0));assert(out[0]==v&&!out[1]&&!out[2]&&!out[3]);
        assert(!memcmp(out+header_end+delta,bytes+header_end,tile_end-header_end));
        assert(!memcmp(out+chest_end+delta,bytes+chest_end,footer_start-chest_end));
        assert(terra_world_close(h)==0);h=0;assert(terra_world_open_from_buffer(out,n,&h)==0&&h);free(out);
        section(h,"header",actual,sizeof actual);assert(strstr(actual,"\"worldName\":\"Patched Legacy\""));
        assert(strstr(actual,"\"worldId\":123"));assert(strstr(actual,"\"time\":12.5"));assert(strstr(actual,"\"bloodMoon\":true"));
        section(h,"chests",actual,sizeof actual);assert(!strcmp(actual,expected));
        if(v>=7){section(h,"footer",actual,sizeof actual);assert(strstr(actual,"Patched Legacy"));assert(strstr(actual,"123"));}
        assert(operation(h,"header_patch","{\"patch\":{\"dayTime\":false}}") == 0);
        out=save(h,&n);assert(terra_world_close(h)==0);h=0;assert(terra_world_open_from_buffer(out,n,&h)==0&&h);free(out);
        section(h,"header",actual,sizeof actual);assert(strstr(actual,"\"dayTime\":false"));
        section(h,"chests",actual,sizeof actual);assert(!strcmp(actual,expected));
        uint32_t snapshot_len;uint8_t *snapshot=save(h,&snapshot_len);
        assert(operation(h,"batch_update_tiles","{\"rules\":[{\"where\":{\"type\":2},\"patch\":{\"wall\":1}}]}")==0);
        out=save(h,&n);assert(n==snapshot_len&&!memcmp(out,snapshot,n));free(out);
        assert(operation(h,"batch_update_tiles","{\"rules\":[{\"where\":{\"type\":3},\"patch\":{\"wire_yellow\":true}}]}")!=0);
        out=save(h,&n);assert(n==snapshot_len&&!memcmp(out,snapshot,n));free(out);
        assert(operation(h,"batch_update_tiles","{\"rules\":[{\"where\":{\"type\":3},\"patch\":{\"wall\":1}}]}")==0);
        uint32_t wall_extra=v>=48?2:1;
        out=save(h,&n);assert(n==snapshot_len+wall_extra);assert(out[0]==v);
        assert(!memcmp(out+header_end+delta,bytes+header_end,wall_offset-header_end));
        assert(out[wall_offset+delta]==1&&out[wall_offset+delta+1]==1);
        if(v>=48)assert(out[wall_offset+delta+2]==0);
        assert(!memcmp(out+wall_offset+delta+1+wall_extra,bytes+wall_offset+1,tile_end-wall_offset-1));
        assert(!memcmp(out+tile_end+delta+wall_extra,snapshot+tile_end+delta,snapshot_len-tile_end-delta));free(snapshot);
        assert(terra_world_close(h)==0);h=0;assert(terra_world_open_from_buffer(out,n,&h)==0&&h);free(out);
        /* Native tests seed the same retained pixel queue without 32-bit bridge
         * pointers; the production save/metadata cleanup path remains real. */
        TxWorld *world=tx_get_world(h);TxPixelMap map={10,20,30,255,1,0,0,0,1,0};
        world->pixel_art_pixels=tx_alloc(4);world->pixel_art_maps=tx_alloc(sizeof map);
        assert(world->pixel_art_pixels&&world->pixel_art_maps);memcpy(world->pixel_art_pixels,&map,4);memcpy(world->pixel_art_maps,&map,sizeof map);
        world->pixel_art_pixels_len=4;world->pixel_art_map_count=1;world->pixel_art_width=1;world->pixel_art_height=1;
        out=save(h,&n);assert(out[0]==v);assert(terra_world_close(h)==0);h=0;
        assert(terra_world_open_from_buffer(out,n,&h)==0&&h);free(out);
        world=tx_get_world(h);
        for(unsigned section_id=2;section_id<=3;section_id++){
            assert(world->ends[section_id]-world->starts[section_id]==1000);
            for(uint32_t i=world->starts[section_id];i<world->ends[section_id];i++)assert(world->file[i]==0);
        }
        assert(world->ends[4]-world->starts[4]==footer_start-npc_start);
        assert(!memcmp(world->file+world->starts[4],bytes+npc_start,footer_start-npc_start));
        section(h,"chests",actual,sizeof actual);assert(!strcmp(actual,"[]"));
        section(h,"signs",actual,sizeof actual);assert(!strcmp(actual,"[]"));
        section(h,"header",actual,sizeof actual);assert(strstr(actual,"Patched Legacy")&&strstr(actual,"\"time\":12.5")&&strstr(actual,"\"dayTime\":false"));
        assert(terra_world_close(h)==0);
    }
    puts("Release legacy open/save/header/chests/tiles/pixels: all 87 layouts preserve bytes and reject unrepresentable edits atomically");return 0;
}
