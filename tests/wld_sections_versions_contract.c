#include "terra_types.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int json_validate_document(const char*,int);
extern int serialize_section_json(TxWorld*, int, TxBuf*);

static void run_npc_legacy(void) {
    /* v139: town type and given name are both strings; no persistent loop. */
    static uint8_t data[] = { 1,5,'G','u','i','d','e',3,'B','o','b',
        0,0,32,65,0,0,0,0,0,10,0,0,0,20,0,0,0,0 };
    uint8_t out[512]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.file=data; w.file_len=sizeof data; w.version=139; w.pointer_count=5;
    w.starts[4]=0; w.ends[4]=sizeof data;
    assert(serialize_section_json(&w,4,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
    assert(strstr((char*)out,"Guide") && strstr((char*)out,"Bob"));
    assert(strstr((char*)out,"\"npcNetId\":22"));
}

static void run_int32_tile_entity_count(void) {
    /* v279: count is Int32, followed by one training dummy entity. */
    static uint8_t data[] = {1,0,0,0, 0, 7,0,0,0, 2,0,3,0, 9,0};
    uint8_t out[512]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.file=data; w.file_len=sizeof data; w.version=279; w.pointer_count=11;
    w.starts[5]=0; w.ends[5]=sizeof data;
    assert(serialize_section_json(&w,5,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
    assert(strstr((char*)out,"entityType") && strstr((char*)out,"\"npc\":9"));
}

static void run_footer_uses_versioned_pointer(void) {
    static uint8_t data[] = {1,1,'W',1,2,3,4};
    uint8_t out[512]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.file=data; w.file_len=sizeof data; w.version=139; w.pointer_count=7;
    w.starts[6]=0; w.ends[6]=sizeof data; w.worldName[0]='H'; w.worldName[1]=0;
    w.worldId=99; /* must not be used in place of the footer tuple */
    assert(serialize_section_json(&w,10,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
    assert(strstr((char*)out,"\"worldName\":\"W\""));
    assert(strstr((char*)out,"\"worldId\":67305985"));
}

static void run_footer_truncation_and_override(void) {
    static uint8_t truncated[] = {1};
    static uint8_t replacement[] = {1,3,'N','e','w',9,0,0,0};
    uint8_t out[512]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.file=truncated; w.file_len=sizeof truncated; w.version=139; w.pointer_count=7;
    w.starts[6]=0; w.ends[6]=sizeof truncated;
    assert(serialize_section_json(&w,10,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
    assert(strstr((char*)out,"\"valid\":false"));
    b.len=0; w.section_overrides[6].active=1; w.section_overrides[6].data=replacement;
    w.section_overrides[6].len=sizeof replacement;
    assert(serialize_section_json(&w,10,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
    assert(strstr((char*)out,"\"worldName\":\"New\""));
    assert(strstr((char*)out,"\"worldId\":9"));
}

static void run_legacy_footer_full_span(void) {
    /* Independent bytes/JSON: a Chinese code point crosses byte 159, followed
     * by embedded NUL, another control byte, quote, slash and newline. */
    static const uint8_t tail[]="界\0\1\"\\\n";
    const uint32_t name_length=158u+sizeof(tail)-1u;
    uint8_t data[512]={0xa5,0x80,0xff,1},out[1024];char expected[1024];
    data[4]=(uint8_t)((name_length&127u)|128u);data[5]=(uint8_t)(name_length>>7u);
    memset(data+6,'a',158);memcpy(data+164,tail,sizeof tail-1u);
    uint32_t length=6u+name_length;data[length++]=17;data[length++]=0;data[length++]=0;data[length++]=0;
    TxWorld w={0};w.legacy_wld=1;w.pointer_count=6;w.file=data;w.file_len=length;w.legacy_footer_start=3;
    strcpy(w.worldName,"Not the physical footer");w.worldId=99;
    for(uint32_t version=7;version<=87;version+=80){
        w.version=version;
        for(int overridden=0;overridden<2;overridden++){
            w.section_overrides[5].active=(uint8_t)overridden;w.section_overrides[5].data=data+3;w.section_overrides[5].len=length-3;
            for(int flag=0;flag<=1;flag++){
                data[3]=(uint8_t)flag;TxBuf b={out,0,sizeof out-1,1};
                int n=snprintf(expected,sizeof expected,"{\"valid\":%s,\"worldName\":\"",flag?"true":"false");
                memset(expected+n,'a',158);strcpy(expected+n+158,"界\\u0000\\u0001\\\"\\\\\\n\",\"worldId\":17}");
                assert(serialize_section_json(&w,10,&b)&&b.ok);out[b.len]=0;
                assert(json_validate_document((char*)out,(int)b.len)&&!strcmp((char*)out,expected));
            }
        }
        /* The backing array remains complete; only the active range is short. */
        const uint32_t truncated[]={0,1,2,3,name_length+2u,name_length+6u};
        for(unsigned i=0;i<sizeof truncated/sizeof truncated[0];i++){
            TxBuf b={out,0,sizeof out,1};w.section_overrides[5].len=truncated[i];
            assert(!serialize_section_json(&w,10,&b)&&b.ok);
        }
        uint8_t bad[]={1,0x80,0x80,0x80,0x80,0x10};
        w.section_overrides[5].data=bad;w.section_overrides[5].len=sizeof bad;
        TxBuf b={out,0,sizeof out,1};assert(!serialize_section_json(&w,10,&b)&&b.ok);
        w.section_overrides[5].data=NULL;b.len=0;assert(!serialize_section_json(&w,10,&b)&&b.ok);
    }
    /* Versions 1..6 have no footer, even if arbitrary trailing bytes resemble one. */
    w.version=6;w.section_overrides[5].active=0;TxBuf b={out,0,sizeof out-1,1};
    assert(serialize_section_json(&w,10,&b));out[b.len]=0;
    assert(!strcmp((char*)out,"{\"valid\":false,\"worldName\":\"\",\"worldId\":0,\"present\":false}"));
}

static void run_display_doll_version_gates(void) {
    static const uint8_t v307[] = {2,0,0,0,3,1,0,0,0,2,0,3,0,0,0,7,7,2,0,0,0,4,0,5,0};
    static const uint8_t v308[] = {2,0,0,0,3,1,0,0,0,2,0,3,0,0,0,7,1,100,0,2,1,0,7,2,0,0,0,4,0,5,0};
    /* v311 stores equip slot 8 after misc; v312 stores it with equip items. */
    static const uint8_t v311[] = {2,0,0,0,3,1,0,0,0,2,0,3,0,0,0,7,2,100,0,2,1,0,7,2,0,0,0,4,0,5,0};
    static const uint8_t v312[] = {2,0,0,0,3,1,0,0,0,2,0,3,0,0,0,7,2,100,0,2,1,0,7,2,0,0,0,4,0,5,0};
    const uint8_t *cases[] = {v307,v308,v311,v312};
    const uint32_t lengths[] = {sizeof v307,sizeof v308,sizeof v311,sizeof v312};
    const uint32_t versions[] = {307,308,311,312};
    for (int i=0; i<4; ++i) {
        uint8_t out[1024]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
        w.file=(uint8_t*)cases[i]; w.file_len=lengths[i]; w.version=versions[i]; w.pointer_count=11;
        w.starts[5]=0; w.ends[5]=lengths[i];
        assert(serialize_section_json(&w,5,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
        assert(json_validate_document((char*)out,(int)b.len));
        assert(strstr((char*)out,"\"entityId\":1"));
        assert(strstr((char*)out,"\"entityId\":2"));
    }
}

static void run_creative_power_sequence(void) {
    static uint8_t data[] = {1,0,0,1, 1,8,0,0,0,192,63, 0};
    uint8_t out[1024]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.file=data; w.file_len=sizeof data; w.version=279; w.pointer_count=11;
    w.starts[9]=0; w.ends[9]=sizeof data;
    assert(serialize_section_json(&w,9,&b)); out[b.len < sizeof out ? b.len : sizeof out-1]=0;
    assert(strstr((char*)out,"\"powerId\":0") && strstr((char*)out,"\"enabled\":true"));
    assert(strstr((char*)out,"\"powerId\":8") && strstr((char*)out,"1.5"));
}

static void run_creative_rejects_unknown_and_truncated_payloads(void) {
    static uint8_t unknown[]={1,99,0,0};
    static uint8_t truncated[]={1,8,0,0};
    uint8_t out[256]; TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.version=279; w.pointer_count=11; w.file=unknown;
    w.file_len=sizeof unknown; w.ends[9]=sizeof unknown;
    assert(!serialize_section_json(&w,9,&b));
    b.len=0; w.file=truncated; w.file_len=sizeof truncated; w.ends[9]=sizeof truncated;
    assert(!serialize_section_json(&w,9,&b));
}

static void run_legacy_dummies(void) {
    uint8_t data[]={1,0,0,0,2,0,3,0},out[256];
    TxWorld w={0}; TxBuf b={out,0,sizeof out,1};
    w.file=data; w.file_len=sizeof data; w.version=116; w.pointer_count=7; w.ends[5]=sizeof data;
    assert(serialize_section_json(&w,5,&b)); out[b.len]=0;
    assert(json_validate_document((char*)out,(int)b.len));
    assert(strstr((char*)out,"\"positionX\":2"));
    assert(strstr((char*)out,"\"positionY\":3"));
}

int main(void) {
    run_npc_legacy();
    run_legacy_dummies();
    run_int32_tile_entity_count();
    run_footer_uses_versioned_pointer();
    run_footer_truncation_and_override();
    run_legacy_footer_full_span();
    run_display_doll_version_gates();
    run_creative_power_sequence();
    run_creative_rejects_unknown_and_truncated_payloads();
    return 0;
}
