/* JSON view of the pre-release-88 contiguous WLD tail. */
#include "terra_types.h"
#include "terra_reader.h"

extern uint32_t rd_u32le(const uint8_t*,uint32_t,uint32_t*);
extern uint16_t rd_u16le(const uint8_t*,uint32_t,uint32_t*);
extern int32_t rd_i32le(const uint8_t*,uint32_t,uint32_t*);
extern float rd_f32le(const uint8_t*,uint32_t,uint32_t*);
extern uint8_t rd_u8(const uint8_t*,uint32_t,uint32_t*);
extern void rd_string_copy(const uint8_t*,uint32_t,uint32_t*,char*,uint32_t);
extern uint32_t rd_7bit(const uint8_t*,uint32_t,uint32_t*,int*);
extern void buf_init(TxBuf*,uint32_t);
extern void buf_u8(TxBuf*,uint8_t);
extern void buf_cstr(TxBuf*,const char*);
extern void json_string(TxBuf*,const char*);
extern void json_i32(TxBuf*,int32_t);
extern void json_u32(TxBuf*,uint32_t);
extern void json_float(TxBuf*,double);
/* Generated from Terraria.ID/NPCID.cs by the parent integration. */
extern int32_t tx_legacy_npc_id(const char* legacy_name);

static int take(const uint8_t *p,uint32_t n,uint32_t *o,uint32_t e) { (void)p; return terra_reader_take(o,n,e); }
static int str(const uint8_t *p,uint32_t n,uint32_t *o,char *out,uint32_t cap) {
    uint32_t save=*o, temp; int ok=0, size=rd_7bit(p,n,o,&ok);
    if(!ok||!terra_reader_has(*o,size,n)){*o=save;return 0;}
    temp=save; rd_string_copy(p,n,&temp,out,cap); *o=temp; return 1;
}
static int boolv(const uint8_t *p,uint32_t n,uint32_t *o) { return *o<n ? p[(*o)++]!=0 : -1; }

static void legacy_given_name(TxWorld *w,int32_t id,char *name,uint32_t capacity) {
    static const int16_t ids[]={17,18,19,20,22,54,38,107,108,124,160,178,207,208,209,227,228,229,353};
    if (w->version<31u || w->version>83u) return;
    uint32_t off=w->legacy_npc_names_start;
    uint32_t count=9u+(w->version>=35u)+(w->version>=65u?8u:0u)+(w->version>=79u);
    for (uint32_t i=0;i<count;i++) {
        if (ids[i]==id) { rd_string_copy(w->file,w->legacy_footer_start,&off,name,capacity); return; }
        int ok=0; uint32_t size=rd_7bit(w->file,w->legacy_footer_start,&off,&ok);
        if (!ok || !terra_reader_take(&off,size,w->legacy_footer_start)) return;
    }
}

int serialize_legacy_section_json(TxWorld *w,int logical,TxBuf *b) {
    const uint8_t *p; uint32_t n,o,slots; int present;
    if(!w||!w->legacy_wld||!b) return 0;
    p=w->file; n=w->file_len; o=w->legacy_chest_start; slots=w->version<58u?20u:40u;
    int actual=logical==10?5:logical;
    if(actual>=2&&actual<=5&&w->section_overrides[actual].active){p=w->section_overrides[actual].data;n=w->section_overrides[actual].len;o=0;}
    if(logical==2) {
        buf_u8(b,'['); int first=1;
        for(uint32_t c=0;c<1000u;c++) { present=boolv(p,n,&o); if(present<0)return 0; if(!present)continue;
            if(!terra_reader_has(o,8,n))return 0; int32_t x=rd_i32le(p,n,&o),y=rd_i32le(p,n,&o); char name[256]={0};
            if(w->version>=85u&&!str(p,n,&o,name,sizeof(name)))return 0;
            if(!first)buf_u8(b,','); first=0; buf_cstr(b,"{\"x\":");json_i32(b,x);buf_cstr(b,",\"y\":");json_i32(b,y);buf_cstr(b,",\"name\":");json_string(b,name);buf_cstr(b,",\"maxItems\":");json_u32(b,slots);buf_cstr(b,",\"items\":[");
            for(uint32_t i=0;i<slots;i++){uint32_t stack;if(w->version<59u){if(!take(p,1,&o,n))return 0;stack=p[o-1];}else{if(!take(p,2,&o,n))return 0;stack=p[o-2]|((uint32_t)p[o-1]<<8);}if(i)buf_u8(b,',');if(!stack){buf_cstr(b,"null");continue;}buf_cstr(b,"{\"stack\":");json_u32(b,stack);buf_cstr(b,",\"itemType\":");if(w->version>=38u){json_i32(b,rd_i32le(p,n,&o));}else{char legacy[256]={0};if(!str(p,n,&o,legacy,sizeof(legacy)))return 0;json_i32(b,0);buf_cstr(b,",\"legacyName\":");json_string(b,legacy);}if(w->version>=36u){buf_cstr(b,",\"prefix\":");json_u32(b,rd_u8(p,n,&o));}buf_u8(b,'}');}
            buf_cstr(b,"]}");
        } buf_u8(b,']'); return 1;
    }
    if(logical==3) {
        o=w->section_overrides[3].active?0:w->legacy_sign_start; buf_u8(b,'['); int first=1;
        for(uint32_t i=0;i<1000u;i++){present=boolv(p,n,&o);if(present<0)return 0;if(!present)continue;char text[1024]={0};if(!str(p,n,&o,text,sizeof(text))||!terra_reader_has(o,8,n))return 0;int32_t x=rd_i32le(p,n,&o),y=rd_i32le(p,n,&o);if(!first)buf_u8(b,',');first=0;buf_cstr(b,"{\"x\":");json_i32(b,x);buf_cstr(b,",\"y\":");json_i32(b,y);buf_cstr(b,",\"text\":");json_string(b,text);buf_u8(b,'}');}buf_u8(b,']');return 1;
    }
    if(logical==4) {
        o=w->legacy_npc_start; buf_cstr(b,"{\"shimmeredTownNpcNetIds\":[],\"shimmeredTownNpcCount\":0,\"townNpcs\":[");int first=1;
        for(;;){present=boolv(p,n,&o);if(present<0)return 0;if(!present)break;char type[256]={0},name[256]={0};if(w->version>=190u){if(!take(p,4,&o,n))return 0;}else if(!str(p,n,&o,type,sizeof(type)))return 0;if(w->version>=83u&&!str(p,n,&o,name,sizeof(name)))return 0;if(!terra_reader_has(o,8,n))return 0;float x=rd_f32le(p,n,&o),y=rd_f32le(p,n,&o);int homeless=boolv(p,n,&o);if(homeless<0||!terra_reader_has(o,8,n))return 0;int32_t hx=rd_i32le(p,n,&o),hy=rd_i32le(p,n,&o);
            legacy_given_name(w,tx_legacy_npc_id(type),name,sizeof(name));
            if(!first)buf_u8(b,',');first=0;buf_cstr(b,"{\"npcNetId\":");json_i32(b,tx_legacy_npc_id(type));buf_cstr(b,",\"legacyTypeName\":");json_string(b,type);buf_cstr(b,",\"givenName\":");json_string(b,name);buf_cstr(b,",\"positionX\":");json_float(b,x);buf_cstr(b,",\"positionY\":");json_float(b,y);buf_cstr(b,",\"homeless\":");if(homeless)buf_cstr(b,"true");else buf_cstr(b,"false");buf_cstr(b,",\"homeTileX\":");json_i32(b,hx);buf_cstr(b,",\"homeTileY\":");json_i32(b,hy);buf_cstr(b,",\"homelessDespawn\":false}");}
        buf_cstr(b,"],\"persistentNpcs\":[]}");return 1;
    }
    if (logical>=5 && logical<=9) {
        buf_cstr(b,logical==8?"{\"kills\":[],\"sightings\":[],\"chats\":[]}":"[]"); return 1;
    }
    if (logical==10 && w->version<7u) { buf_cstr(b,"{\"valid\":false,\"worldName\":\"\",\"worldId\":0,\"present\":false}"); return 1; }
    if(logical==10){char footer_name[160]={0};o=w->section_overrides[5].active?0:w->legacy_footer_start;present=boolv(p,n,&o);if(present<0||!str(p,n,&o,footer_name,sizeof(footer_name))||!terra_reader_has(o,4,n))return 0;int32_t footer_id=rd_i32le(p,n,&o);buf_cstr(b,"{\"valid\":");if(present)buf_cstr(b,"true");else buf_cstr(b,"false");buf_cstr(b,",\"worldName\":");json_string(b,footer_name);buf_cstr(b,",\"worldId\":");json_i32(b,footer_id);buf_u8(b,'}');return 1;}
    return 0;
}
