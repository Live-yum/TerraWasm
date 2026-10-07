/* Independent WorldFile/DisplayDoll bytes; exercise the shared extraction and
 * destination payload codec without a Wasm32 bridge or external fixture. */
#include "../src/terra_circuit_objects.h"
static void circuit_object_versions_contract(void) {
    const uint8_t legacy[]={1,0,2,0,1,0,0,0,2,0,0,0,6,0xe7,0xa5,0x9e,0xe5,0x9c,0xa3,
        0xcf,7,0xc9,4,0,0,0,0,0};
    const uint8_t modern[]={1,0,1,0,0,0,2,0,0,0,6,0xe7,0xa5,0x9e,0xe5,0x9c,0xa3,
        2,0,0,0,0xcf,7,0xc9,4,0,0,0,0,0};
    const uint32_t versions[]={88,115,116,121,122,269,293,294,306,307,308,311,312,326};
    for(size_t i=0;i<sizeof versions/sizeof versions[0];i++){
        TxWorld w={0};w.version=versions[i];w.pointer_count=7;
        w.file=(uint8_t*)(w.version<294?legacy:modern);
        w.file_len=w.ends[2]=w.version<294?sizeof legacy:sizeof modern;
        CoCursor c;CoItem a;uint8_t encoded[64],roundtrip[64];
        assert(co_cursor(&w,2,&c)&&co_next(&c,&a)==1);
        assert(a.x==1&&a.y==2);
        uint32_t canonical=co_encode_payload(&a,encoded,326);
        assert(canonical==sizeof modern-10&&memcmp(encoded,modern+10,canonical)==0);
        CoItem normalized=a;normalized.version=326;normalized.payload=encoded;normalized.length=canonical;
        uint32_t original=co_encode_payload(&normalized,roundtrip,w.version);
        assert(original==a.length&&memcmp(roundtrip,a.payload,original)==0);
        assert(co_next(&c,&a)==2);
        w.ends[2]--;assert(co_cursor(&w,2,&c)&&co_next(&c,&a)==0);
        if(w.version<116){assert(co_cursor(&w,5,&c)&&co_next(&c,&a)==2);}
    }
    /* The same semantic outfit: equip0, equip8, dye0, dye8, misc0. v311
     * puts equip8 last, while 308/312/326 put it before dye0. */
    const uint8_t before307[]={1,1,1,0,2,3,0,3,0,4,5,0};
    const uint8_t at307[]={1,1,3,1,0,2,3,0,3,0,4,5,0};
    const uint8_t at308[]={1,1,3,7,1,0,2,3,0,2,0,3,4,0,3,0,4,5,0,4,0,5,6,0,5,0,6,7,0};
    const uint8_t at311[]={1,1,3,7,1,0,2,3,0,3,0,4,5,0,4,0,5,6,0,5,0,6,7,0,2,0,3,4,0};
    const uint8_t *payloads[]={before307,at307,at308,at311,at308,at308};
    const uint32_t lengths[]={sizeof before307,sizeof at307,sizeof at308,sizeof at311,sizeof at308,sizeof at308};
    const uint32_t doll_versions[]={306,307,308,311,312,326};
    for(size_t i=0;i<sizeof doll_versions/sizeof doll_versions[0];i++){
        uint8_t section[64]={1,0,0,0,3,17,0,0,0,2,0,3,0},canonical[64],roundtrip[64];
        memcpy(section+13,payloads[i],lengths[i]);TxWorld w={0};
        w.version=doll_versions[i];w.pointer_count=7;w.file=section;w.file_len=w.ends[5]=13+lengths[i];
        CoCursor c;CoItem a;assert(co_cursor(&w,5,&c)&&co_next(&c,&a)==1);
        assert(a.kind==3&&a.tile==470&&a.x==2&&a.y==3);
        uint32_t n=co_encode_payload(&a,canonical,326),parsed=0;
        assert(co_payload(5,3,canonical,n,&parsed)&&parsed==n);
        if(w.version>=308)assert(n==sizeof at308&&!memcmp(canonical,at308,n));
        CoItem normalized=a;normalized.version=326;normalized.payload=canonical;normalized.length=n;
        assert(co_encode_payload(&normalized,roundtrip,w.version)==lengths[i]);
        assert(!memcmp(roundtrip,payloads[i],lengths[i]));
        if(w.version>=307)assert(!co_encode_payload(&normalized,NULL,306));
        if(w.version>=308)assert(!co_encode_payload(&normalized,NULL,307));
        assert(co_next(&c,&a)==2);
        w.ends[5]--;assert(co_cursor(&w,5,&c)&&co_next(&c,&a)==0);
    }
    /* No-data-loss downgrade adds/removes only absent pose/extra bytes. */
    uint8_t empty_doll[]={0,0,0,0},downgraded[4];
    CoItem empty={0};empty.section=5;empty.kind=3;empty.version=326;empty.payload=empty_doll;empty.length=4;
    assert(co_encode_payload(&empty,downgraded,306)==2&&!downgraded[0]&&!downgraded[1]);
    for(uint32_t version=116;version<=121;version++){
        uint8_t legacy_dummy[]={1,0,0,0,2,0,3,0},canonical[2];
        TxWorld w={0};w.version=version;w.pointer_count=7;w.file=legacy_dummy;w.file_len=w.ends[5]=sizeof legacy_dummy;
        CoCursor c;CoItem a;assert(co_cursor(&w,5,&c)&&co_next(&c,&a)==1);
        assert(a.kind==0&&a.tile==378&&a.x==2&&a.y==3);
        assert(co_encode_payload(&a,canonical,326)==2&&canonical[0]==255&&canonical[1]==255);
        assert(co_next(&c,&a)==2);
    }
    puts("circuit object versions: legacy/shared chests, legacy dummies, 307/308/311 doll layouts and lossless conversions passed");
}
