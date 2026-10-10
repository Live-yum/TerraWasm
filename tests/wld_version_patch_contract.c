/* Exercise the internal header-only migration policy without constructing
 * unrelated header/tile payloads. The boundaries come from WorldFile and
 * BannerSystem loaders, not from the table under test. */
#include "../src/terra_mutators.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void serialize_chests_json(TxWorld*,TxBuf*);
extern void tx_reset_heap(void);

static int replace_chests(TxWorld *w,const char *request) {
    TxBuf response;buf_init(&response,128u);
    int result=tx_mutate_replace_chests(w,request,(uint32_t)strlen(request),&response);
    tx_internal_free(response.data);
    return result;
}

static void chest_version_contract(uint32_t version) {
    uint8_t original[]={0xaa,0xbb,0,0,2,0,0xcc,0xdd};
    uint8_t snapshot[sizeof original];memcpy(snapshot,original,sizeof original);
    TxWorld w={0};w.version=w.original_version=version;w.pointer_count=4u;
    w.maxTilesX=w.maxTilesY=100;w.file=original;w.file_len=sizeof original;
    w.starts[0]=0;w.ends[0]=1;w.starts[1]=1;w.ends[1]=2;
    w.starts[2]=2;w.ends[2]=6;w.starts[3]=6;w.ends[3]=8;
    const char *request="{\"chests\":["
        "{\"x\":1,\"y\":2,\"name\":\"\u795e\u5723\",\"maxItems\":2,\"items\":[{\"stack\":1999,\"itemType\":1225,\"prefix\":0},null]},"
        "{\"x\":3,\"y\":4,\"name\":\"\",\"maxItems\":2,\"items\":[null,{\"stack\":1,\"itemType\":1,\"prefix\":255}]}]}";
    /* Independently specified BinaryWriter bytes, including empty slots. */
    const uint8_t legacy[]={2,0,2,0,
        1,0,0,0,2,0,0,0,6,0xe7,0xa5,0x9e,0xe5,0x9c,0xa3,0xcf,7,0xc9,4,0,0,0,0,0,
        3,0,0,0,4,0,0,0,0,0,0,1,0,1,0,0,0,255};
    const uint8_t modern[]={2,0,
        1,0,0,0,2,0,0,0,6,0xe7,0xa5,0x9e,0xe5,0x9c,0xa3,2,0,0,0,0xcf,7,0xc9,4,0,0,0,0,0,
        3,0,0,0,4,0,0,0,0,2,0,0,0,0,0,1,0,1,0,0,0,255};
    assert(replace_chests(&w,request)>=0);
    TxSectionOverride *section=&w.section_overrides[2];
    const uint8_t *expected=version<294u?legacy:modern;
    uint32_t expected_len=version<294u?sizeof legacy:sizeof modern;
    assert(section->active&&section->len==expected_len);
    assert(memcmp(section->data,expected,expected_len)==0);
    assert(w.version==version&&w.original_version==version);
    assert(memcmp(w.file,snapshot,sizeof snapshot)==0);
    assert(w.starts[2]==2&&w.ends[2]==6);
    for(uint32_t i=0;i<TX_MAX_SECTION_OVERRIDES;i++)if(i!=2u)assert(!w.section_overrides[i].active);
    TxWorld reopened=w;reopened.file=section->data;reopened.file_len=section->len;
    reopened.starts[2]=0;reopened.ends[2]=section->len;memset(reopened.section_overrides,0,sizeof reopened.section_overrides);
    TxBuf json;buf_init(&json,512u);serialize_chests_json(&reopened,&json);buf_u8(&json,0);
    assert(strstr((char*)json.data,"\"stack\":1999"));
    assert(strstr((char*)json.data,"\"itemType\":1225"));
    assert(strstr((char*)json.data,"\u795e\u5723"));
    tx_internal_free(json.data);

    const char *bad_second[]={
        "{\"x\":-1,\"y\":0,\"name\":\"\",\"maxItems\":2,\"items\":[null,null]}",
        "{\"x\":0,\"y\":0,\"name\":\"\",\"maxItems\":2,\"items\":[null]}",
        "{\"x\":0,\"y\":0,\"name\":\"\",\"maxItems\":2,\"items\":[{\"stack\":32768,\"itemType\":1,\"prefix\":0},null]}",
        "{\"x\":0,\"y\":0,\"name\":\"\",\"maxItems\":2,\"items\":[{\"stack\":1,\"itemType\":0,\"prefix\":0},null]}",
        "{\"x\":0,\"y\":0,\"name\":\"\",\"maxItems\":2,\"items\":[{\"stack\":1,\"itemType\":1,\"prefix\":256},null]}",
        "{\"x\":0,\"y\":0,\"name\":\"\",\"maxItems\":2,\"items\":[nul,null]}",
    };
    uint8_t *previous=section->data;
    for(size_t i=0;i<sizeof bad_second/sizeof bad_second[0];i++){
        char invalid[1024];snprintf(invalid,sizeof invalid,
            "{\"chests\":[{\"x\":1,\"y\":2,\"name\":\"\",\"maxItems\":2,\"items\":[null,null]},%s]}",bad_second[i]);
        assert(replace_chests(&w,invalid)<0);
        assert(section->data==previous&&section->len==expected_len);
        assert(memcmp(section->data,expected,expected_len)==0);
    }
    const char *mixed="{\"chests\":[{\"x\":1,\"y\":2,\"name\":\"\",\"maxItems\":2,\"items\":[null,null]},"
        "{\"x\":3,\"y\":4,\"name\":\"\",\"maxItems\":1,\"items\":[null]}]}";
    if(version<294u){
        assert(replace_chests(&w,mixed)<0);
        assert(section->data==previous&&section->len==expected_len);
        assert(memcmp(section->data,expected,expected_len)==0);
        /* Even equally resized legacy chests must not change source capacity. */
        assert(replace_chests(&w,"{\"chests\":[{\"x\":0,\"y\":0,\"name\":\"\",\"maxItems\":1,\"items\":[null]}]}")<0);
    }else assert(replace_chests(&w,mixed)>=0);
    assert(replace_chests(&w,"{\"chests\":[]}")>=0);
    assert(section->len==(version<294u?4u:2u));
    assert(section->data[0]==0&&section->data[1]==0);
    if(version<294u)assert(section->data[2]==2&&section->data[3]==0);
    tx_internal_free(section->data);
    memset(section,0,sizeof *section);w.starts[2]=w.ends[2]=0;
    assert(replace_chests(&w,"{\"chests\":[]}")>=0);
    if(version<294u)assert(section->len==4&&section->data[2]==40&&section->data[3]==0);
    tx_internal_free(section->data);
    tx_reset_heap();
}

static void header_208_modes_contract(void) {
    TxWorld w={0};w.maxTilesX=w.maxTilesY=100;
    uint8_t names[]={0,0};w.file=names;w.file_len=sizeof names;
    w.pointer_count=1;w.ends[0]=sizeof names;
    strcpy(w.uuid,"00000000-0000-0000-0000-000000000000");
    w.creationTime=UINT64_C(0x0807060504030201);w.moonType=9;
    TxJsonParser request={"",0u,0u};
    /* Empty worldName/seed: 1 + 1 + generator(8) + uuid(16) + seven Int32s. */
    const uint32_t mode_offset=1u+1u+8u+16u+7u*4u;
    for(int mode=0;mode<=2;mode++){
        w.gameMode=mode;TxBuf header;buf_init(&header,1024u);
        assert(encode_header_model(&w,&request,NULL,0u,208u,&header));
        assert(header.data[mode_offset]==(mode==1));
        assert(header.data[mode_offset+1u]==(mode==2));
        for(uint32_t i=0;i<8u;i++)assert(header.data[mode_offset+2u+i]==i+1u);
        assert(header.data[mode_offset+10u]==9u);
        tx_internal_free(header.data);
    }
    tx_reset_heap();
}

static void legacy_chest_source_contract(void) {
    uint8_t source[]={0,0,0xff,0xff};
    TxWorld w={0};w.version=w.original_version=269;w.pointer_count=3;
    w.file=source;w.file_len=sizeof source;
    for(uint32_t len=1;len<=4;len++){
        w.ends[2]=len;
        assert(replace_chests(&w,"{\"chests\":[]}")<0);
        assert(!w.section_overrides[2].active);
    }
    w.ends[2]=sizeof source+1u;
    assert(replace_chests(&w,"{\"chests\":[]}")<0);
    assert(!w.section_overrides[2].active);
    w.starts[2]=4;w.ends[2]=3;
    assert(replace_chests(&w,"{\"chests\":[]}")<0);
    assert(!w.section_overrides[2].active);
    tx_reset_heap();
}

static char *empty_chests_request(uint32_t count,uint32_t slots) {
    size_t capacity=32u+(size_t)count*(80u+5u*slots);
    char *request=malloc(capacity);assert(request);
    size_t used=(size_t)snprintf(request,capacity,"{\"chests\":[");
    for(uint32_t i=0;i<count;i++){
        used+=(size_t)snprintf(request+used,capacity-used,
            "%s{\"x\":%u,\"y\":0,\"name\":\"\",\"maxItems\":%u,\"items\":[",i?",":"",i,slots);
        for(uint32_t item=0;item<slots;item++)used+=(size_t)snprintf(request+used,capacity-used,"%snull",item?",":"");
        used+=(size_t)snprintf(request+used,capacity-used,"]}");
    }
    used+=(size_t)snprintf(request+used,capacity-used,"]}");
    assert(used<capacity&&used<1024u*1024u);
    return request;
}

/* Independent file-layout checks: current WorldFile supports 8000 counted
 * entries; LoadWorld_Version1_Old_BeforeRelease88 reads 1000 presence slots.
 * Exercise source capacity, not a copy of the mutator's constants. */
static void chest_capacity_contract(uint32_t version,int legacy) {
    uint8_t original[]={0xaa,0xbb,0,0,2,0,0xcc,0xdd};
    uint8_t snapshot[sizeof original];memcpy(snapshot,original,sizeof original);
    TxWorld w={0};w.version=w.original_version=version;w.legacy_wld=(uint8_t)legacy;
    w.pointer_count=4u;w.maxTilesX=8400;w.maxTilesY=2400;w.file=original;w.file_len=sizeof original;
    w.starts[2]=2;w.ends[2]=6;
    uint32_t slots=legacy?(version<58u?20u:40u):(version<294u?2u:0u);
    const uint32_t counted_counts[]={0,1,1000,1001,1467,8000};
    const uint32_t legacy_counts[]={0,1,999,1000};
    const uint32_t *counts=legacy?legacy_counts:counted_counts;
    uint32_t cases=legacy?4u:6u;
    TxSectionOverride *section=&w.section_overrides[2];
    for(uint32_t c=0;c<cases;c++){
        uint32_t count=counts[c];char *request=empty_chests_request(count,slots);
        assert(replace_chests(&w,request)>=0);free(request);
        uint32_t offset=0u;
        if(!legacy){
            assert(section->data[offset++]==(uint8_t)count);
            assert(section->data[offset++]==(uint8_t)(count>>8u));
            if(version<294u){assert(section->data[offset++]==slots);assert(section->data[offset++]==0);}
        }
        for(uint32_t i=0;i<count;i++){
            if(legacy)assert(section->data[offset++]==1);
            for(uint32_t byte=0;byte<4;byte++)assert(section->data[offset++]==(uint8_t)(i>>(byte*8u)));
            for(uint32_t byte=0;byte<4;byte++)assert(section->data[offset++]==0); /* y */
            if(version>=85u)assert(section->data[offset++]==0); /* empty name */
            if(version>=294u){assert(slots==0);for(uint32_t byte=0;byte<4;byte++)assert(section->data[offset++]==0);}
            for(uint32_t byte=0;byte<slots*(version<59u?1u:2u);byte++)assert(section->data[offset++]==0);
        }
        if(legacy)for(uint32_t i=count;i<1000u;i++)assert(section->data[offset++]==0);
        assert(section->active&&section->len==offset);
        assert(w.version==version&&w.original_version==version);
        assert(w.starts[2]==2&&w.ends[2]==6&&!memcmp(original,snapshot,sizeof original));
    }
    uint8_t *previous=section->data;uint32_t previous_len=section->len;
    uint8_t *previous_bytes=malloc(previous_len);assert(previous_bytes);memcpy(previous_bytes,previous,previous_len);
    char *over_limit=empty_chests_request(legacy?1001u:8001u,slots);
    assert(replace_chests(&w,over_limit)<0);free(over_limit);
    assert(section->data==previous&&section->len==previous_len);
    assert(!memcmp(section->data,previous_bytes,previous_len));
    assert(!memcmp(original,snapshot,sizeof original));
    free(previous_bytes);tx_internal_free(section->data);tx_reset_heap();
}

int main(void) {
    const uint32_t boundaries[][2]={{194,195},{195,196},{267,268},{288,289},{311,312},{322,323}};
    for (uint32_t i=0;i<sizeof(boundaries)/sizeof(boundaries[0]);i++) {
        assert(!header_versions_compatible(boundaries[i][0],boundaries[i][1]));
        assert(!header_versions_compatible(boundaries[i][1],boundaries[i][0]));
    }
    assert(header_versions_compatible(196,197));
    assert(header_versions_compatible(315,322));
    assert(header_versions_compatible(323,326));
    assert(!header_versions_compatible(326,327));
    const uint32_t chest_versions[]={88,269,293,294,326};
    for(size_t i=0;i<sizeof chest_versions/sizeof chest_versions[0];i++){
        chest_version_contract(chest_versions[i]);chest_capacity_contract(chest_versions[i],0);
    }
    const uint32_t legacy_versions[]={1,35,37,38,57,58,59,84,85,87};
    for(size_t i=0;i<sizeof legacy_versions/sizeof legacy_versions[0];i++)chest_capacity_contract(legacy_versions[i],1);
    header_208_modes_contract();
    legacy_chest_source_contract();
    puts("versioned chest encoding, 8000 counted / 1000 legacy capacity, validation atomicity and v208 mode bytes: ok");
    return 0;
}
