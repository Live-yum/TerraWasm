/* * terra_wld.c -- WLD binary parser for TerraWasm. * * Pure C implementation that parses raw .wld bytes int o TxWorld metadata, * streams tiles on-demand, and provides JSON serializers/deserializers * for all 11 world sections matching the TerraX V2 API. * * Uses bump allocator from terra_mem.c (tx_malloc, tx_alloc, TxBuf). * Uses json_string, json_u32, etc. from terra_json.c for JSON output. */

#include "terra_types.h"
#include "terra_reader.h"
#include "terra_legacy.h"
#if defined(__clang__) && defined(__EMSCRIPTEN__)
#define TX_COLD_PARSER __attribute__((minsize))
#else
#define TX_COLD_PARSER
#endif
extern int serialize_legacy_section_json(TxWorld*,int,TxBuf*);
/* ==================================================================== * Extern declarations from terra_mem.c * ==================================================================== */extern uint32_t tx_strlen(const char *s);
extern int tx_streq_c(const char *a,const char *b);
extern int tx_streq_n(const char *a,uint32_t alen,const char *b);
extern void buf_init(TxBuf *b,uint32_t cap);
extern int buf_reserve(TxBuf *b,uint32_t extra);
extern void buf_u8(TxBuf *b,uint8_t v);
extern void buf_bytes(TxBuf *b,const void *p,uint32_t n);
extern void buf_u16le(TxBuf *b,uint32_t v);
extern void buf_u32le(TxBuf *b,uint32_t v);
extern void buf_u64le(TxBuf *b,uint64_t v);
extern void buf_cstr(TxBuf *b,const char *s);
extern void tx_clear_error(void);
extern void tx_set_error(const char *code,const char *message);
extern int set_result_buf(TxBuf *b);
extern int set_result_bytes(uint8_t *p,uint32_t len);
extern void *memcpy(void *dst,const void *src,unsigned long n);
extern void *memset(void *dst,int value,unsigned long n);
/* Heap management */extern uint8_t *tx_alloc(uint32_t size);
extern void tx_internal_free(void *ptr);
extern uint32_t tx_mark(void);
extern uint32_t tx_global_heap_mark;
/* ==================================================================== * Extern declarations from terra_json.c (parser helpers) * ==================================================================== */extern int json_skip_value(const char *s,int len,int pos);
extern int json_find_key(const char *json,int jlen,const char *key);
extern int json_value_eq(const char *json,int jlen,int pos,const char *val);
extern int json_is_null(const char *json,int jlen,int pos);
extern int json_extract_str(const char *json,int jlen,int pos,char *out,int ocap);
extern int json_extract_int(const char *json,int jlen,int pos,int32_t *out);
extern int json_extract_u64(const char *json,int jlen,int pos,uint64_t *out);
extern int json_extract_bool(const char *json,int jlen,int pos,int *out);
extern int json_extract_float(const char *json,int jlen,int pos,double *out);
extern int json_array_count(const char *json,int jlen,int pos);
extern int json_array_element(const char *json,int jlen,int pos,int index);
/* ==================================================================== * Extern declarations from terra_json.c (builder functions) * ==================================================================== */extern void json_string(TxBuf *b,const char *s);
extern void json_u32(TxBuf *b,uint32_t v);
extern void json_i32(TxBuf *b,int32_t v);
extern void json_u64(TxBuf *b,uint64_t v);
extern void json_bool(TxBuf *b,int v);
extern void json_null(TxBuf *b);
extern void json_float(TxBuf *b,double v);
/* ==================================================================== * Binary reader functions * ==================================================================== */uint8_t rd_u8(const uint8_t *p,uint32_t len,uint32_t *off){
    if (*off>=len)return 0;
    return p[(*off)++];
    }
uint16_t rd_u16le(const uint8_t *p,uint32_t len,uint32_t *off){
    uint32_t o=*off;
    if (!terra_reader_has(o,2u,len)){
        *off=len;
        return 0;
        }
    *off=o+2u;
    return (uint16_t)(p[o]|((uint32_t)p[o+1]<<8));
    }
uint32_t rd_u32le(const uint8_t *p,uint32_t len,uint32_t *off){
    uint32_t o=*off;
    if (!terra_reader_has(o,4u,len)){
        *off=len;
        return 0;
        }
    *off=o+4u;
    return (uint32_t)p[o]|((uint32_t)p[o+1]<<8)|((uint32_t)p[o+2]<<16)|((uint32_t)p[o+3]<<24);
    }
uint32_t rd_i32le(const uint8_t *p,uint32_t len,uint32_t *off){
    return (int32_t)rd_u32le(p,len,off);
    }
uint64_t rd_u64le(const uint8_t *p,uint32_t len,uint32_t *off){
    uint64_t v=0;
    uint32_t o=*off;
    if (!terra_reader_has(o,8u,len)){
        *off=len;
        return 0;
        }
    for (uint32_t i=0;
    i<8;
    i++)v|=((uint64_t)p[o+i])<<(i*8u);
    *off=o+8u;
    return v;
    }
double rd_f64le(const uint8_t *p,uint32_t len,uint32_t *off){
    union{
        uint64_t u;
        double d;
        }
    v;
    v.u=rd_u64le(p,len,off); return v.d;
    }
float rd_f32le(const uint8_t *p,uint32_t len,uint32_t *off){
    union{
        uint32_t u;
        float f;
        }
    v;
    v.u=rd_u32le(p,len,off); return v.f;
    }
void rd_skip(const uint8_t *p,uint32_t len,uint32_t *off,uint32_t n){
    (void)p;
    (void)terra_reader_take(off,n,len);
    }
uint32_t rd_7bit(const uint8_t *p,uint32_t len,uint32_t *off,int *ok){
    uint32_t result=0;
    uint32_t shift=0;
    *ok=0;
    for (uint32_t i=0;
    i<5u;
    i++){
        if (*off>=len)return 0;
        uint8_t b=p[(*off)++];
        if (i==4u&&(b&0x7fu)>0x0fu)return 0;
        result|=(uint32_t)(b&0x7fu)<<shift;
    if ((b&0x80u)==0u){
            *ok=1;
            return result;
            }
        shift+=7u;
        }
    return 0;
    }
void rd_string_copy(const uint8_t *p,uint32_t len,uint32_t *off,char *out,uint32_t cap){
    if (!off){
        if (out&&cap)out[0]=0;
        return;
        }
    if (!p){
        if (out&&cap)out[0]=0;
        *off=len;
        return;
        }
    int ok=0;
    uint32_t slen=rd_7bit(p,len,off,&ok);
    if (!ok||!terra_reader_has(*off,slen,len)){
        if (out&&cap)out[0]=0;
        *off=len;
        return;
        }
    uint32_t n=slen;
    if (!out||cap==0u)n=0u;
    else if (n>=cap)n=cap-1u;
    for (uint32_t i=0;
    i<n;
    i++)out[i]=(char)p[*off+i];
    if (out&&cap)out[n]=0;
    (void)terra_reader_take(off,slen,len);
    }
void uuid_to_string(const uint8_t *p,char *out){
    static const char hex[]="0123456789abcdef";
    uint32_t order[16]={
        3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15}
    ;
    uint32_t k=0;
    for (uint32_t i=0;
    i<16;
    i++){
        if (i==4||i==6||i==8||i==10)out[k++]='-';
        uint8_t b=p[order[i]];
        out[k++]=hex[b>>4];
        out[k++]=hex[b&15u];
        }
    out[k]=0;
    }
void rd_skip_string_value(const uint8_t *p,uint32_t len,uint32_t *off){
    if (!off)return;
    if (!p){
        *off=len;
        return;
        }
    int ok=0;
    uint32_t slen=rd_7bit(p,len,off,&ok);
    if (!ok||!terra_reader_has(*off,slen,len)){
        *off=len;
        return;
        }
    (void)terra_reader_take(off,slen,len);
    }
/* ==================================================================== * Tile importance lookup * ==================================================================== */static int tile_important(TxWorld *w,uint16_t type){
    if (!w||type>=w->tile_type_count)return 0;
    uint32_t idx=type>>3;
    if (idx>=w->important_len)return 0;
    return (w->important[idx]>>(type&7u))&1u;
    }
/* ==================================================================== * parse_format -- Extract format metadata from raw .wld bytes * ==================================================================== */int parse_format(TxWorld *w){
    uint32_t off=0;
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    w->version=rd_u32le(p,len,&off);
    if (w->version==0u||w->version>326u){
        tx_set_error("TERRAX_UNSUPPORTED_VERSION","unsupported .wld version");
        return 0;
        }
    /* Releases 1..87 predate the section table entirely.  The old loader
     * starts reading the header immediately after the version word; the
     * legacy header decoder below discovers the tile stream boundary. */
    if (w->version < 88u) {
        w->legacy_wld=1u;
        w->magic[0]=0;
        w->file_type=2u;
        w->pointer_count=2u;
        w->starts[0]=4u;
        w->ends[0]=w->file_len;
        w->starts[1]=w->file_len;
        w->ends[1]=w->file_len;
        w->format_len=4u;
        /* v78+ writes UInt16 tile ids. Keep the bitmap wide enough for
         * historical ids instead of silently treating ids >=256 as plain
         * tiles during frame decoding. */
        w->tile_type_count=65535u;
        w->important_len=(w->tile_type_count+7u)/8u;
        w->important=(uint8_t*)tx_alloc(w->important_len);
        if (!w->important) { tx_set_error("TERRAX_WASM_OOM","legacy tile metadata allocation failed"); return 0; }
        memset(w->important,0,w->important_len);
        /* Ancient worlds only contain the original tile ids.  This list is
         * the stable frame-important set used by Terraria's old loader. */
        { static const uint16_t ids[]={3,4,5,10,11,12,13,14,15,16,17,18,19,20,21,24,26,27,28,29,31,33,34,35,36,42,49,50,55,61,71,72,73,74,77,78,79,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,110,113,114,125,126,128,129,132,133,134,135,136,137,138,139,141,142,143,144,149,165,171,172,173,174,178,184,185,186,187,201,207,209,210,212,215,216,217,218,219,220,227,228,231,233,235,236,237,238,239,240,241,242,243,244,245,246,247};
          for (uint32_t i=0;i<sizeof(ids)/sizeof(ids[0]);i++) w->important[ids[i]>>3]|=(uint8_t)(1u<<(ids[i]&7u));
          /* Exact high ids are generated from Terraria.Main.tileFrameImportant
           * and kept here because release 78+ can persist UInt16 ids. */
          static const uint16_t high[]={254,269,270,271,275,276,277,278,279,280,281,282,283,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,314,316,317,318,319,320,323,324,334,335,337,338,339,349,354,355,356,358,359,360,361,362,363,364,372,373,374,375,376,377,378,380,386,387,388,389,390,391,392,393,394,395,405,406,410,411,412,413,414,419,420,423,424,425,427,428,429,440,441,442,443,444,445,452,453,454,455,456,457,461,462,463,464,465,466,467,468,469,470,471,475,476,480,484,485,486,487,488,489,490,491,493,494,497,499,505,506,509,510,511,518,519,520,521,522,523,524,525,526,527,529,530,531,532,533,538,542,543,544,545,547,548,549,550,551,552,553,554,555,556,558,559,560,564,565,567,568,569,570,571,572,573,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,619,620,621,622,623,624,629,630,631,632,634,637,639,640,642,643,644,645,646,653,654,656,657,658,660,663,664,665,695,696,698,699,700,701,702,703,704,705,707,709,710,711,712,713,714,715,716,720,721,723,724,725,726,733,751,752,753};
          for (uint32_t i=0;i<sizeof(high)/sizeof(high[0]);i++) w->important[high[i]>>3]|=(uint8_t)(1u<<(high[i]&7u)); }
        return 1;
    }
    w->magic[0]=0;
    w->file_type=2;
    if (w->version>=135u){
        if (!terra_reader_has(off,20u,len)){
            tx_set_error("TERRAX_TRUNCATED_FORMAT","format metadata truncated");
            return 0;
            }
        for (uint32_t i=0;
        i<7u;
        i++)w->magic[i]=(char)p[off++];
        w->magic[7]=0;
        w->file_type=p[off++];
        w->revision=rd_u32le(p,len,&off);
        w->favorite=rd_u64le(p,len,&off);
        }
    w->pointer_count=rd_u16le(p,len,&off);
    if (w->pointer_count==0u||w->pointer_count>TX_MAX_SECTIONS){
        tx_set_error("TERRAX_BAD_POINTERS","unsupported section pointer count");
        return 0;
        }
    for (uint32_t i=0;
    i<w->pointer_count;
    i++)w->positions[i]=rd_u32le(p,len,&off);
    w->tile_type_count=rd_u16le(p,len,&off);
    w->important_len=(w->tile_type_count+7u)/8u;
    if (!terra_reader_has(off,w->important_len,len)){
        tx_set_error("TERRAX_TRUNCATED_FORMAT","tile importance bitmap truncated");
        return 0;
        }
    w->important=p+off;
    off+=w->important_len;
    w->format_len=off;
    for (uint32_t i=0;
    i<w->pointer_count;
    i++){
        w->starts[i]=w->positions[i];
        w->ends[i]=(i+1u<w->pointer_count)?w->positions[i+1u]:w->file_len;
        if (w->starts[i]>w->file_len||w->ends[i]>w->file_len||w->starts[i]>w->ends[i]){
            tx_set_error("TERRAX_BAD_POINTERS","section pointers out of range");
            return 0;
            }
        }
    return 1;
    }
static void tx_record_header_bool(TxWorld *w,const char *json_name,uint32_t absolute_offset,
                                  uint32_t world_member_offset){
    if (!w||!json_name||absolute_offset<w->starts[0]||
        w->header_bool_field_count>=TX_MAX_HEADER_BOOL_FIELDS)return;
    TxHeaderBoolField *field=&w->header_bool_fields[w->header_bool_field_count++];
    field->json_name=json_name;
    field->section_offset=absolute_offset-w->starts[0];
    field->world_member_offset=world_member_offset;
    }

#define TX_RD_HEADER_BOOL(member,json_name) do { \
    tx_record_header_bool(w,json_name,off+header_offset_base,(uint32_t)((uint8_t*)&w->member-(uint8_t*)w)); \
    w->member=rd_u8(p,len,&off); \
    } while(0)

/* ==================================================================== * parse_header -- Extract header metadata from raw .wld bytes * ==================================================================== */static TX_COLD_PARSER int parse_header_layout(TxWorld *w,uint8_t claimable_banners_present){
    uint32_t header_offset_base=0u;
    uint32_t off=0u;
    uint32_t len=0u;
    uint8_t *p=NULL;
    if (!w) {
        tx_set_error("TERRAX_INVALID_ARGUMENT","null world");
        return 0;
        }
    if (w->section_overrides[0].active){
        p=w->section_overrides[0].data;
        len=w->section_overrides[0].len;
        off=0u;
        header_offset_base=w->starts[0];
        }
    else{
        if (!w->file||w->pointer_count==0u||w->starts[0]>w->ends[0]||
            w->ends[0]>w->file_len){
            tx_set_error("TERRAX_BAD_POINTERS","header section bounds are invalid");
            return 0;
            }
        p=w->file;
        off=w->starts[0];
        len=w->ends[0];
        }
    int ok;
    w->claimableBannersPresent=claimable_banners_present;
    w->header_bool_field_count=0u;
    /* name */rd_string_copy(p,len,&off,w->worldName,TX_MAX_NAME);
    /* seed + worldGenVersion (>=179) */if (w->version>=179u){
        if (w->version==179u){
            uint32_t seed_i=rd_u32le(p,len,&off);
            TxBuf tmp;
            buf_init(&tmp,32);
            json_u32(&tmp,seed_i);
            uint32_t n=tmp.len<TX_MAX_NAME-1u?tmp.len:TX_MAX_NAME-1u;
            memcpy(w->seed,tmp.data,n);
            w->seed[n]=0;
            }
        else{
            rd_string_copy(p,len,&off,w->seed,TX_MAX_NAME);
            }
        w->worldGeneratorVersion=rd_u64le(p,len,&off);
        }
    /* uuid (>=181) */if (w->version>=181u){
        if (terra_reader_has(off,16u,len)){
            uuid_to_string(p+off,w->uuid);
            off+=16u;
            }
        }
    /* worldId, left/right/top/bottom, height, width */w->worldId=rd_i32le(p,len,&off);
    w->leftWorld=rd_i32le(p,len,&off);
    w->rightWorld=rd_i32le(p,len,&off);
    w->topWorld=rd_i32le(p,len,&off);
    w->bottomWorld=rd_i32le(p,len,&off);
    w->maxTilesY=rd_i32le(p,len,&off);
    w->maxTilesX=rd_i32le(p,len,&off);
    /* gameMode */if (w->version>=209u){
        w->gameMode=rd_i32le(p,len,&off);
        }
    else if (w->version>=112u){
        w->gameMode=rd_u8(p,len,&off)?1:0;
        }
    /* Seed flags (version >= 209 path) */if (w->version>=209u){
        if (w->version>=222u)TX_RD_HEADER_BOOL(drunkWorld,"drunkWorld");
        if (w->version>=227u)TX_RD_HEADER_BOOL(ftwWorld,"getGoodWorld");
        if (w->version>=238u)TX_RD_HEADER_BOOL(tenthAnniversaryWorld,"tenthAnniversaryWorld");
        if (w->version>=239u)TX_RD_HEADER_BOOL(dontStarveWorld,"dontStarveWorld");
        if (w->version>=241u)TX_RD_HEADER_BOOL(notTheBeesWorld,"notTheBeesWorld");
        if (w->version>=249u)TX_RD_HEADER_BOOL(remixWorld,"remixWorld");
        if (w->version>=266u)TX_RD_HEADER_BOOL(noTrapsWorld,"noTrapsWorld");
        if (w->version>=267u){
            TX_RD_HEADER_BOOL(zenithWorld,"zenithWorld");
            }
        else{
            w->zenithWorld=w->remixWorld&&w->drunkWorld;
            }
        if (w->version>=302u)TX_RD_HEADER_BOOL(skyblockWorld,"skyblockWorld");
        }
    else if (w->version==208u){
        uint8_t tempMode=rd_u8(p,len,&off);
        w->gameMode=(tempMode==1)?2:0;
        }
    /* Timestamps */if (w->version>=141u)w->creationTime=rd_u64le(p,len,&off);
    if (w->version>=284u)w->lastPlayed=rd_u64le(p,len,&off);
    /* moonType */w->moonType=rd_u8(p,len,&off);
    /* treeX[3] */w->treeX[0]=rd_u32le(p,len,&off);
    w->treeX[1]=rd_u32le(p,len,&off);
    w->treeX[2]=rd_u32le(p,len,&off);
    /* treeStyle[4] */w->treeStyle[0]=rd_u32le(p,len,&off);
    w->treeStyle[1]=rd_u32le(p,len,&off);
    w->treeStyle[2]=rd_u32le(p,len,&off);
    w->treeStyle[3]=rd_u32le(p,len,&off);
    /* caveBackX[3] */w->caveBackX[0]=rd_u32le(p,len,&off);
    w->caveBackX[1]=rd_u32le(p,len,&off);
    w->caveBackX[2]=rd_u32le(p,len,&off);
    /* caveBackStyle[4] */w->caveBackStyle[0]=rd_u32le(p,len,&off);
    w->caveBackStyle[1]=rd_u32le(p,len,&off);
    w->caveBackStyle[2]=rd_u32le(p,len,&off);
    w->caveBackStyle[3]=rd_u32le(p,len,&off);
    /* ice/jungle/hell back styles */w->iceBackStyle=rd_u32le(p,len,&off);
    w->jungleBackStyle=rd_u32le(p,len,&off);
    w->hellBackStyle=rd_u32le(p,len,&off);
    /* spawn */w->spawnTileX=rd_i32le(p,len,&off);
    w->spawnTileY=rd_i32le(p,len,&off);
    /* worldSurface / rockLayer (full double precision) */w->worldSurface=rd_f64le(p,len,&off);
    w->rockLayer=rd_f64le(p,len,&off);
    /* gameTime, isDayTime, moonPhase, isBloodMoon, isEclipse */w->gameTime=rd_f64le(p,len,&off);
    TX_RD_HEADER_BOOL(isDayTime,"dayTime");
    w->moonPhase=rd_u32le(p,len,&off);
    TX_RD_HEADER_BOOL(isBloodMoon,"bloodMoon");
    TX_RD_HEADER_BOOL(isEclipse,"eclipse");
    /* dungeonX/Y, isCrimson */w->dungeonX=rd_i32le(p,len,&off);
    w->dungeonY=rd_i32le(p,len,&off);
    TX_RD_HEADER_BOOL(isCrimson,"crimson");
    /* Boss progress (10 booleans) */TX_RD_HEADER_BOOL(downedEye,"downedEyeOfCthulhu");
    TX_RD_HEADER_BOOL(downedEaterBrain,"downedEaterOfWorldsOrBrainOfCthulhu");
    TX_RD_HEADER_BOOL(downedSkeletron,"downedSkeletron");
    TX_RD_HEADER_BOOL(downedQueenBee,"downedQueenBee");
    TX_RD_HEADER_BOOL(downedDestroyer,"downedDestroyer");
    TX_RD_HEADER_BOOL(downedTwins,"downedTwins");
    TX_RD_HEADER_BOOL(downedSkeletronPrime,"downedSkeletronPrime");
    TX_RD_HEADER_BOOL(downedAnyMech,"downedAnyMechBoss");
    TX_RD_HEADER_BOOL(downedPlantera,"downedPlantera");
    TX_RD_HEADER_BOOL(downedGolem,"downedGolem");
    if (w->version>=118u)TX_RD_HEADER_BOOL(downedKingSlime,"downedKingSlime");
    /* Saved NPCs */TX_RD_HEADER_BOOL(savedGoblin,"savedGoblin");
    TX_RD_HEADER_BOOL(savedWizard,"savedWizard");
    TX_RD_HEADER_BOOL(savedMech,"savedMech");
    TX_RD_HEADER_BOOL(downedGoblins,"downedGoblins");
    TX_RD_HEADER_BOOL(downedClown,"downedClown");
    TX_RD_HEADER_BOOL(downedFrost,"downedFrost");
    TX_RD_HEADER_BOOL(downedPirates,"downedPirates");
    /* World state */TX_RD_HEADER_BOOL(shadowOrbSmashed,"shadowOrbSmashed");
    TX_RD_HEADER_BOOL(spawnMeteor,"spawnMeteor");
    w->shadowOrbCount=rd_u8(p,len,&off);
    w->altarCount=rd_u32le(p,len,&off);
    TX_RD_HEADER_BOOL(hardMode,"hardMode");
    if (w->version>=257u)TX_RD_HEADER_BOOL(afterPartyOfDoom,"afterPartyOfDoom");
    /* Invasion */w->invasionDelay=rd_u32le(p,len,&off);
    w->invasionSize=rd_u32le(p,len,&off);
    w->invasionType=rd_u32le(p,len,&off);
    w->invasionX=rd_f64le(p,len,&off);
    if (w->version>=118u)w->slimeRainTime=rd_f64le(p,len,&off);
    if (w->version>=113u)w->sundialCooldown=rd_u8(p,len,&off);
    /* Weather */TX_RD_HEADER_BOOL(isRaining,"raining");
    w->rainTime=rd_u32le(p,len,&off);
    w->maxRain=rd_f32le(p,len,&off);
    /* Ore tiers */w->oreTierCobalt=rd_i32le(p,len,&off);
    w->oreTierMythril=rd_i32le(p,len,&off);
    w->oreTierAdamantite=rd_i32le(p,len,&off);
    /* Backgrounds (8 x u8) */w->bgTree=rd_u8(p,len,&off);
    w->bgCorruption=rd_u8(p,len,&off);
    w->bgJungle=rd_u8(p,len,&off);
    w->bgSnow=rd_u8(p,len,&off);
    w->bgHallow=rd_u8(p,len,&off);
    w->bgCrimson=rd_u8(p,len,&off);
    w->bgDesert=rd_u8(p,len,&off);
    w->bgOcean=rd_u8(p,len,&off);
    w->cloudBgActive=rd_i32le(p,len,&off);
    w->numClouds=rd_u16le(p,len,&off);
    w->windSpeedSet=rd_f32le(p,len,&off);
    /* Angler (>=95) */if (w->version>=95u){
        w->anglerFinishedSize=rd_u32le(p,len,&off);
        w->anglersOff=off+header_offset_base;
        for (uint32_t i=0;
        i<w->anglerFinishedSize;
        i++)rd_skip_string_value(p,len,&off);
        }
    if (w->version>=99u)TX_RD_HEADER_BOOL(savedAngler,"savedAngler");
    if (w->version>=101u)w->anglerQuest=rd_u32le(p,len,&off);
    if (w->version>=104u)TX_RD_HEADER_BOOL(savedStylist,"savedStylist");
    if (w->version>=129u)TX_RD_HEADER_BOOL(savedTaxCollector,"savedTaxCollector");
    if (w->version>=201u)TX_RD_HEADER_BOOL(savedGolfer,"savedGolfer");
    if (w->version>=107u)w->invasionSizeStart=rd_u32le(p,len,&off);
    if (w->version>=108u)w->cultistDelay=rd_u32le(p,len,&off);
    /* Kill counts (>=109) */if (w->version>=109u){
        w->numMobs=rd_u16le(p,len,&off);
        w->mobsOff=off+header_offset_base;
        if (!terra_reader_take_count(&off,w->numMobs,4u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","mob data exceeds section bounds");
            return 0;
        }
        }
    /* TerraWasm's historical writer stores a claimable-banner block here,
     * while native Terraria WLD files continue directly with late-event
     * booleans. The caller probes both layouts before committing the result. */
    if (w->version>=109u){
        if (claimable_banners_present){
            w->numClaimableBanners=rd_u16le(p,len,&off);
            w->claimableBannersOff=off+header_offset_base;
            if (!terra_reader_take_count(&off,w->numClaimableBanners,2u,len)) {
                tx_set_error("TERRAX_TRUNCATED_HEADER","banner data exceeds section bounds");
                return 0;
            }
        } else {
            w->numClaimableBanners=0u;
            w->claimableBannersOff=0u;
        }
    }
    /* fastForwardTime (>=140, but gate starts at >=128) */if (w->version>=128u){
        TX_RD_HEADER_BOOL(fastForwardTime,"fastForwardTimeToDawn");
        if (w->version>=131u){
            TX_RD_HEADER_BOOL(downedFishron,"downedFishron");
            TX_RD_HEADER_BOOL(downedMartians,"downedMartians");
            TX_RD_HEADER_BOOL(downedLunaticCultist,"downedAncientCultist");
            TX_RD_HEADER_BOOL(downedMoonlord,"downedMoonlord");
            TX_RD_HEADER_BOOL(downedHalloweenKing,"downedHalloweenKing");
            TX_RD_HEADER_BOOL(downedHalloweenTree,"downedHalloweenTree");
            TX_RD_HEADER_BOOL(downedChristmasIceQueen,"downedChristmasIceQueen");
            TX_RD_HEADER_BOOL(downedSanta,"downedChristmasSantank");
            TX_RD_HEADER_BOOL(downedChristmasTree,"downedChristmasTree");
            }
        }
    /* Celestial (>=140) */if (w->version>=140u){
        TX_RD_HEADER_BOOL(downedCelestialSolar,"downedTowerSolar");
        TX_RD_HEADER_BOOL(downedCelestialVortex,"downedTowerVortex");
        TX_RD_HEADER_BOOL(downedCelestialNebula,"downedTowerNebula");
        TX_RD_HEADER_BOOL(downedCelestialStardust,"downedTowerStardust");
        TX_RD_HEADER_BOOL(downedTowerSolar,"towerActiveSolar");
        TX_RD_HEADER_BOOL(downedTowerVortex,"towerActiveVortex");
        TX_RD_HEADER_BOOL(downedTowerNebula,"towerActiveNebula");
        TX_RD_HEADER_BOOL(downedTowerStardust,"towerActiveStardust");
        TX_RD_HEADER_BOOL(downedTowerAncient,"lunarApocalypseIsUp");
        }
    /* Party (>=170) */if (w->version>=170u){
        TX_RD_HEADER_BOOL(partyManual,"partyManual");
        TX_RD_HEADER_BOOL(partyGenuine,"partyGenuine");
        w->partyCooldown=rd_u32le(p,len,&off);
        w->partyCelebratingNPCSize=rd_u32le(p,len,&off);
        w->partyCelebratingNPCsOff=off+header_offset_base;
        if (!terra_reader_take_count(&off,w->partyCelebratingNPCSize,4u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","party data exceeds section bounds");
            return 0;
        }
        }
    /* Sandstorm (>=174) */if (w->version>=174u){
        TX_RD_HEADER_BOOL(sandstormHappening,"sandstormHappening");
        w->sandStormTime=rd_u32le(p,len,&off);
        w->sandStormSeverity=rd_f32le(p,len,&off);
        w->sandstormIntendedSeverity=rd_f32le(p,len,&off);
        }
    /* DD2 (>=178) */if (w->version>=178u){
        TX_RD_HEADER_BOOL(savedBartender,"savedBartender");
        TX_RD_HEADER_BOOL(downedInvasionT1,"dd2DownedT1");
        TX_RD_HEADER_BOOL(downedInvasionT2,"dd2DownedT2");
        TX_RD_HEADER_BOOL(downedInvasionT3,"dd2DownedT3");
        }
    /* mushroomBg (>194) */if (w->version>194u)w->mushroomBg=rd_u8(p,len,&off);
    if (w->version>=215u)w->undergroundDesertBg=rd_u8(p,len,&off);
    if (w->version>195u){
        w->bgTree2=rd_u8(p,len,&off);
        w->bgTree3=rd_u8(p,len,&off);
        w->bgTree4=rd_u8(p,len,&off);
        }
    /* combatBook (>=204) */if (w->version>=204u)TX_RD_HEADER_BOOL(combatBookUsed,"combatBookWasUsed");
    /* lanternNight (>=207) */if (w->version>=207u){
        w->lanternNightCooldown=rd_u32le(p,len,&off);
        TX_RD_HEADER_BOOL(lanternNightGenuine,"lanternNightGenuine");
        TX_RD_HEADER_BOOL(lanternNightManual,"lanternNightManual");
        TX_RD_HEADER_BOOL(lanternNightNextNightIsGenuine,"lanternNightNextNightIsGenuine");
        }
    /* treeTopVariations (>=211) */if (w->version>=211u){
        w->treetopSize=rd_u32le(p,len,&off);
        w->treeTopVariationsOff=off+header_offset_base;
        if (!terra_reader_take_count(&off,w->treetopSize,4u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","tree data exceeds section bounds");
            return 0;
        }
        }
    /* forceHalloween/forceXMas (>=212) */if (w->version>=212u){
        TX_RD_HEADER_BOOL(forceHalloweenForToday,"forceHalloweenForToday");
        TX_RD_HEADER_BOOL(forceXMasForToday,"forceXMasForToday");
        }
    /* Extra ore tiers (>=216) */if (w->version>=216u){
        w->savedOreTiersCopper=rd_u32le(p,len,&off);
        w->savedOreTiersIron=rd_u32le(p,len,&off);
        w->savedOreTiersSilver=rd_u32le(p,len,&off);
        w->savedOreTiersGold=rd_u32le(p,len,&off);
        }
    /* boughtCat/Dog/Bunny (>=217) */if (w->version>=217u){
        TX_RD_HEADER_BOOL(boughtCat,"boughtCat");
        TX_RD_HEADER_BOOL(boughtDog,"boughtDog");
        TX_RD_HEADER_BOOL(boughtBunny,"boughtBunny");
        }
    /* 1.4 bosses (>=223) */if (w->version>=223u){
        TX_RD_HEADER_BOOL(downedEmpressOfLight,"downedEmpressOfLight");
        TX_RD_HEADER_BOOL(downedQueenSlime,"downedQueenSlime");
        }
    if (w->version>=240u)TX_RD_HEADER_BOOL(downedDeerclops,"downedDeerclops");
    /* Unlocked spawns (>=250-261) */if (w->version>=250u)TX_RD_HEADER_BOOL(unlockedSlimeBlueSpawn,"unlockedSlimeBlueSpawn");
    if (w->version>=251u){
        TX_RD_HEADER_BOOL(unlockedMerchantSpawn,"unlockedMerchantSpawn");
        TX_RD_HEADER_BOOL(unlockedDemolitionistSpawn,"unlockedDemolitionistSpawn");
        TX_RD_HEADER_BOOL(unlockedPartyGirlSpawn,"unlockedPartyGirlSpawn");
        TX_RD_HEADER_BOOL(unlockedDyeTraderSpawn,"unlockedDyeTraderSpawn");
        TX_RD_HEADER_BOOL(unlockedTruffleSpawn,"unlockedTruffleSpawn");
        TX_RD_HEADER_BOOL(unlockedArmsDealerSpawn,"unlockedArmsDealerSpawn");
        TX_RD_HEADER_BOOL(unlockedNurseSpawn,"unlockedNurseSpawn");
        TX_RD_HEADER_BOOL(unlockedPrincessSpawn,"unlockedPrincessSpawn");
        }
    if (w->version>=259u)TX_RD_HEADER_BOOL(combatBookVolumeTwoWasUsed,"combatBookVolumeTwoWasUsed");
    if (w->version>=260u)TX_RD_HEADER_BOOL(peddlersSatchelWasUsed,"peddlersSatchelWasUsed");
    if (w->version>=261u){
        TX_RD_HEADER_BOOL(unlockedSlimeGreenSpawn,"unlockedSlimeGreenSpawn");
        TX_RD_HEADER_BOOL(unlockedSlimeOldSpawn,"unlockedSlimeOldSpawn");
        TX_RD_HEADER_BOOL(unlockedSlimePurpleSpawn,"unlockedSlimePurpleSpawn");
        TX_RD_HEADER_BOOL(unlockedSlimeRainbowSpawn,"unlockedSlimeRainbowSpawn");
        TX_RD_HEADER_BOOL(unlockedSlimeRedSpawn,"unlockedSlimeRedSpawn");
        TX_RD_HEADER_BOOL(unlockedSlimeYellowSpawn,"unlockedSlimeYellowSpawn");
        TX_RD_HEADER_BOOL(unlockedSlimeCopperSpawn,"unlockedSlimeCopperSpawn");
        }
    /* fastForwardDusk + moondialCooldown (>=264) */if (w->version>=264u){
        TX_RD_HEADER_BOOL(fastForwardTimeToDusk,"fastForwardTimeToDusk");
        w->moondialCooldown=rd_u8(p,len,&off);
        }
    /* forceHalloween/forceXMas forever (>=287) */if (w->version>=287u){
        TX_RD_HEADER_BOOL(forceHalloweenForever,"forceHalloweenForever");
        TX_RD_HEADER_BOOL(forcexmasForever,"forceXMasForever");
        }
    /* Seeds (>=288/296) */if (w->version>=288u)TX_RD_HEADER_BOOL(vampireSeed,"vampireSeed");
    if (w->version>=296u)TX_RD_HEADER_BOOL(infectedSeed,"infectedSeed");
    /* meteorShowerCount, coinRain (>=291) */if (w->version>=291u){
        w->tempmeteorShowerCount=rd_u32le(p,len,&off);
        w->tempcoinRain=rd_u32le(p,len,&off);
        }
    /* teamBasedSpawnsSeed + spawnPointManager (>=297) */if (w->version>=297u){
        TX_RD_HEADER_BOOL(teambasedSpawnsSeed,"teamBasedSpawnsSeed");
        w->numExtradSpawnPointManager=rd_u8(p,len,&off);
        w->extradSpawnPointManagerOff=off+header_offset_base;
        if (!terra_reader_take_count(&off,(uint32_t)w->numExtradSpawnPointManager,4u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","spawn point data exceeds section bounds");
            return 0;
        }
        }
    /* dualDungeonsSeed (>=304) */if (w->version>=304u)TX_RD_HEADER_BOOL(dualdungeonsSeed,"dualDungeonsSeed");
    /* legacySkip (>=299 && <313) */if (w->version>=299u&&w->version<313u)w->legacySkip=rd_u32le(p,len,&off);
    /* manifestJson (>=299) */if (w->version>=299u){
        w->maniFestOff=off+header_offset_base;
        uint32_t slen=0;
        ok=0;
        slen=rd_7bit(p,len,&off,&ok);
        if (!ok || !terra_reader_take(&off,slen,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","manifest string exceeds section bounds");
            return 0;
        }
        w->maniFestLen=slen;
        }
    if (w->maxTilesX<=0||w->maxTilesY<=0){
        tx_set_error("TERRAX_BAD_HEADER","invalid world dimensions");
        return 0;
        }
    if (w->starts[1]>w->file_len||w->starts[1]<w->starts[0]){
        tx_set_error("TERRAX_BAD_HEADER","invalid tile section pointer");
        return 0;
        }
    return 1;
    }
#undef TX_RD_HEADER_BOOL
/* Decode the contiguous header used by world versions 1..87.  Defaults match
 * WorldFile.LoadWorld_Version1_Old_BeforeRelease88 when a field did not yet
 * exist in that release. */
int read_tile_at(TxWorld *w,uint32_t *off,uint32_t end,TxTile *t);
static int legacy_skip_string(const uint8_t *p,uint32_t len,uint32_t *off) {
    int ok=0; uint32_t n=rd_7bit(p,len,off,&ok);
    return ok && terra_reader_has(*off,n,len) && (terra_reader_take(off,n,len),1);
}
static int legacy_take(const uint8_t *p,uint32_t len,uint32_t *off,uint32_t n) {
    (void)p; return terra_reader_take(off,n,len);
}
static int validate_legacy_tail(TxWorld *w,uint32_t off) {
    const uint8_t *p=w->file; uint32_t len=w->file_len; uint8_t present;
    uint32_t slots=(w->version<58u)?20u:40u;
    w->legacy_chest_start=off;
    for(uint32_t i=0;i<1000u;i++) {
        if(!legacy_take(p,len,&off,1u)) return 0; present=p[off-1u];
        if(!present) continue;
        if(!legacy_take(p,len,&off,8u) || (w->version>=85u&&!legacy_skip_string(p,len,&off))) return 0;
        for(uint32_t j=0;j<slots;j++) {
            uint32_t stack;
            if(w->version<59u) { if(!legacy_take(p,len,&off,1u)) return 0; stack=p[off-1u]; }
            else { if(!legacy_take(p,len,&off,2u)) return 0; stack=(uint32_t)(p[off-2u]|((uint32_t)p[off-1u]<<8)); }
            if(stack) { if(w->version>=38u ? !legacy_take(p,len,&off,4u) : !legacy_skip_string(p,len,&off)) return 0; if(w->version>=36u&&!legacy_take(p,len,&off,1u)) return 0; }
        }
    }
    w->legacy_sign_start=off;
    for(uint32_t i=0;i<1000u;i++) {
        if(!legacy_take(p,len,&off,1u)) return 0; present=p[off-1u];
        if(present && (!legacy_skip_string(p,len,&off)||!legacy_take(p,len,&off,8u))) return 0;
    }
    w->legacy_npc_start=off;
    /* Town NPC list: the first byte of each record is its continuation flag. */
    for(;;) {
        if(!legacy_take(p,len,&off,1u)) return 0; present=p[off-1u]; if(!present) break;
        if((w->version>=190u&&!legacy_take(p,len,&off,4u))||(w->version<190u&&!legacy_skip_string(p,len,&off))||
           (w->version>=83u&&!legacy_skip_string(p,len,&off))||!legacy_take(p,len,&off,17u)) return 0;
    }
    w->legacy_npc_names_start=off;
    if(w->version>=31u&&w->version<=83u) {
        uint32_t count=9u; if(w->version>=35u)count++; if(w->version>=65u)count+=8u; if(w->version>=79u)count++;
        for(uint32_t i=0;i<count;i++) if(!legacy_skip_string(p,len,&off)) return 0;
    }
    w->legacy_footer_start=off;
    if(w->version>=7u && (!legacy_take(p,len,&off,1u)||!legacy_skip_string(p,len,&off)||!legacy_take(p,len,&off,4u))) return 0;
    return off<=len;
}
static TX_COLD_PARSER int parse_legacy_header(TxWorld *w) {
    uint8_t *p; uint32_t len, off=4u;
    if (!w || !w->file || w->file_len<4u) return 0;
    p=w->file; len=w->file_len;
#define L_U8(x) do { if (!terra_reader_has(off,1u,len)) goto truncated; (x)=p[off++]; } while(0)
#define L_I32(x) do { if (!terra_reader_has(off,4u,len)) goto truncated; (x)=(int32_t)rd_u32le(p,len,&off); } while(0)
#define L_U32(x) do { if (!terra_reader_has(off,4u,len)) goto truncated; (x)=rd_u32le(p,len,&off); } while(0)
#define L_F32(x) do { if (!terra_reader_has(off,4u,len)) goto truncated; (x)=rd_f32le(p,len,&off); } while(0)
#define L_F64(x) do { if (!terra_reader_has(off,8u,len)) goto truncated; (x)=rd_f64le(p,len,&off); } while(0)
#define L_BOOL(x) do { uint8_t _v; L_U8(_v); (x)=_v?1u:0u; } while(0)
    rd_string_copy(p,len,&off,w->worldName,TX_MAX_NAME);
    if (off>len) goto truncated;
    L_I32(w->worldId); L_I32(w->leftWorld); L_I32(w->rightWorld);
    L_I32(w->topWorld); L_I32(w->bottomWorld); L_I32(w->maxTilesY); L_I32(w->maxTilesX);
    if (w->version>=63u) L_U8(w->moonType);
    if (w->version>=44u) { L_I32(w->treeX[0]); L_I32(w->treeX[1]); L_I32(w->treeX[2]); for(int i=0;i<4;i++) L_I32(w->treeStyle[i]); }
    if (w->version>=60u) { L_I32(w->caveBackX[0]); L_I32(w->caveBackX[1]); L_I32(w->caveBackX[2]); for(int i=0;i<4;i++) L_I32(w->caveBackStyle[i]); L_I32(w->iceBackStyle); if(w->version>=61u){L_I32(w->jungleBackStyle);L_I32(w->hellBackStyle);} }
    L_I32(w->spawnTileX); L_I32(w->spawnTileY); L_F64(w->worldSurface); L_F64(w->rockLayer);
    L_F64(w->gameTime); L_BOOL(w->isDayTime); L_I32(w->moonPhase); L_BOOL(w->isBloodMoon);
    if (w->version>=70u) L_BOOL(w->isEclipse);
    L_I32(w->dungeonX); L_I32(w->dungeonY); if(w->version>=56u) L_BOOL(w->isCrimson);
    L_BOOL(w->downedEye); L_BOOL(w->downedEaterBrain); L_BOOL(w->downedSkeletron);
    if(w->version>=66u) L_BOOL(w->downedQueenBee);
    if(w->version>=44u){L_BOOL(w->downedDestroyer);L_BOOL(w->downedTwins);L_BOOL(w->downedSkeletronPrime);L_BOOL(w->downedAnyMech);}
    if(w->version>=64u){L_BOOL(w->downedPlantera);L_BOOL(w->downedGolem);}
    if(w->version>=29u){L_BOOL(w->savedGoblin);L_BOOL(w->savedWizard);if(w->version>=34u){L_BOOL(w->savedMech);if(w->version>=80u)L_BOOL(w->savedStylist);}L_BOOL(w->downedGoblins);}
    if(w->version>=32u) L_BOOL(w->downedClown);
    if(w->version>=37u) L_BOOL(w->downedFrost);
    if(w->version>=56u) L_BOOL(w->downedPirates);
    L_BOOL(w->shadowOrbSmashed); L_BOOL(w->spawnMeteor); L_U8(w->shadowOrbCount);
    if(w->version>=23u){L_I32(w->altarCount);L_BOOL(w->hardMode);}
    L_I32(w->invasionDelay);L_I32(w->invasionSize);L_I32(w->invasionType);L_F64(w->invasionX);
    if(w->version>=113u)L_U8(w->sundialCooldown);
    if(w->version>=53u){L_BOOL(w->isRaining);L_I32(w->rainTime);L_F32(w->maxRain);}
    if(w->version>=54u){L_I32(w->oreTierCobalt);L_I32(w->oreTierMythril);L_I32(w->oreTierAdamantite);}
    if(w->version>=55u){L_U8(w->bgTree);L_U8(w->bgCorruption);L_U8(w->bgJungle);}
    if(w->version>=60u){L_U8(w->bgSnow);L_U8(w->bgHallow);L_U8(w->bgCrimson);L_U8(w->bgDesert);L_U8(w->bgOcean);L_I32(w->cloudBgActive);}
    if(w->version>=62u){uint16_t clouds; if(!terra_reader_has(off,2u,len))goto truncated; clouds=rd_u16le(p,len,&off);w->numClouds=clouds;L_F32(w->windSpeedSet);}
    w->legacy_tile_start=off;
    if(w->maxTilesX<=0||w->maxTilesY<=0||w->maxTilesX>100000||w->maxTilesY>100000){tx_set_error("TERRAX_BAD_HEADER","invalid world dimensions");return 0;}
    /* Locate the end of the RLE tile stream.  This both prevents later tile
     * consumers from treating legacy chest bytes as tiles and gives malformed
     * old files the same bounded-read rejection as modern worlds. */
    for (int32_t x=0; x<w->maxTilesX; x++) {
        int32_t y=0;
        while (y<w->maxTilesY) {
            TxTile tile;
            uint32_t before=off;
            if (!read_tile_at(w,&off,len,&tile) || off<=before) goto truncated_tiles;
            if (tile.same > (uint16_t)(w->maxTilesY-y-1)) goto truncated_tiles;
            y += (int32_t)tile.same + 1;
        }
    }
    w->legacy_tile_end=off; w->starts[1]=w->legacy_tile_start; w->ends[1]=off;
    if (!validate_legacy_tail(w, off)) { tx_set_error("TERRAX_TRUNCATED_LEGACY_TAIL","legacy chest, sign, NPC, or footer data is truncated"); return 0; }
    return 1;
truncated:
    tx_set_error("TERRAX_TRUNCATED_HEADER","legacy header fields exceed file bounds"); return 0;
truncated_tiles:
    tx_set_error("TERRAX_TRUNCATED_TILES","legacy tile stream exceeds file bounds"); return 0;
#undef L_U8
#undef L_I32
#undef L_U32
#undef L_F32
#undef L_F64
#undef L_BOOL
}
int parse_header(TxWorld *w){
    TxWorld candidate;

    if (!w) {
        tx_set_error("TERRAX_INVALID_ARGUMENT","null world");
        return 0;
    }

    if (w->legacy_wld) return parse_legacy_header(w);
    candidate=*w;
    if (parse_header_layout(&candidate,w->version>=289u)) {
        *w=candidate;
        return 1;
    }

    candidate=*w;
    if (parse_header_layout(&candidate,w->version<289u)) {
        *w=candidate;
        return 1;
    }
    return 0;
}
/* ==================================================================== * read_tile_at -- Stream one tile from the binary * ==================================================================== */int read_tile_at(TxWorld *w,uint32_t *off,uint32_t end,TxTile *t){
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    if (*off>=end||*off>=len) return 0;
    memset(t,0,sizeof(TxTile));
    if (w->legacy_wld) {
        uint8_t v;
#define OLD_BOOL(out) do { if (*off>=end||*off>=len) return 0; (out)=p[(*off)++]?1u:0u; } while(0)
        OLD_BOOL(t->active);
        if (t->active) {
            if (w->version<=77u) { if (*off>=end) return 0; t->type=p[(*off)++]; }
            else { if (!terra_reader_has(*off,2u,end)) return 0; t->type=rd_u16le(p,end,off); }
            if (t->type==127u || t->type==504u) t->active=0;
            if (w->version<72u && (t->type==35u||t->type==36u||t->type==170u||t->type==171u||t->type==172u)) {
                if(!terra_reader_has(*off,4u,end)) return 0;
                t->frame_x=(int16_t)rd_u16le(p,end,off); t->frame_y=(int16_t)rd_u16le(p,end,off);
            } else if (tile_important(w,t->type)
                       && !(w->version<28u && t->type==4u)
                       && !(w->version<40u && t->type==19u)
                       && !(w->version<195u && t->type==49u)) {
                if(!terra_reader_has(*off,4u,end)) return 0;
                t->frame_x=(int16_t)rd_u16le(p,end,off); t->frame_y=(int16_t)rd_u16le(p,end,off);
                if (t->type==144u) t->frame_y=0;
            } else { t->frame_x=-1; t->frame_y=-1; }
            if (w->version>=48u) { OLD_BOOL(v); if(v){if(*off>=end)return 0;t->tile_color=p[(*off)++];} }
        }
        if (w->version<=25u) OLD_BOOL(v);
        OLD_BOOL(v); if(v){if(*off>=end)return 0;t->wall=p[(*off)++];if(w->version>=48u){OLD_BOOL(v);if(v){if(*off>=end)return 0;t->wall_color=p[(*off)++];}}}
        OLD_BOOL(v); if(v){if(*off>=end)return 0;t->liquid_amount=p[(*off)++];OLD_BOOL(v);t->liquid_type=v?2u:1u;if(w->version>=51u){OLD_BOOL(v);if(v)t->liquid_type=3u;}}
        if(w->version>=33u) OLD_BOOL(t->wire_red);
        if(w->version>=43u){OLD_BOOL(t->wire_blue);OLD_BOOL(t->wire_green);}
        if(w->version>=41u){OLD_BOOL(v);t->brick_style=v?1u:0u;if(w->version>=49u){if(*off>=end)return 0;v=p[(*off)++];if(v)t->brick_style=(uint8_t)((v&7u)+1u);}}
        if(w->version>=42u){OLD_BOOL(t->actuator);OLD_BOOL(t->inactive);}
        if(w->version>=25u){if(!terra_reader_has(*off,2u,end))return 0;t->same=(uint16_t)(int16_t)rd_u16le(p,end,off);}
#undef OLD_BOOL
        return *off<=end;
    }
    if (end>len) end=len;
#define TILE_U8(value) do { if (!terra_reader_has(*off,1u,end)) return 0; (value)=rd_u8(p,end,off); } while (0)
#define TILE_U16(value) do { if (!terra_reader_has(*off,2u,end)) return 0; (value)=rd_u16le(p,end,off); } while (0)
    uint8_t f1;
    TILE_U8(f1);
    uint8_t f2=0,f3=0,f4=0;
    if (f1&1u)TILE_U8(f2);
    if (f2&1u)TILE_U8(f3);
    if (f3&1u)TILE_U8(f4);
    t->active=(f1>>1)&1u;
    if (t->active){
        if (f1&32u)TILE_U16(t->type); else TILE_U8(t->type);
        if (tile_important(w,t->type)){
            TILE_U16(t->frame_x);
            TILE_U16(t->frame_y);
            if (t->type==144u)t->frame_y=0;
            }
        else {t->frame_x=-1;t->frame_y=-1;}
        if (f3&8u)TILE_U8(t->tile_color);
        }
    if (f1&4u){
        TILE_U8(t->wall);
        if (f3&16u)TILE_U8(t->wall_color);
        }
    {
        uint8_t liq=(f1>>3)&3u;
        if (liq){
            TILE_U8(t->liquid_amount);
            t->liquid_type=(f3&128u)?4u:liq;
            }
        }
    /* WorldFile stores the wall high byte after paint and liquid payloads. */
    if (f3&64u){uint8_t high;TILE_U8(high);t->wall|=(uint16_t)((uint16_t)high<<8);}
    {
        uint8_t rle=(f1>>6)&3u;
        if (rle==1u)TILE_U8(t->same);
        else if (rle){TILE_U16(t->same);if (t->same>32767u)return 0;}
        }
    t->wire_red=(f2>>1)&1u;
    t->wire_blue=(f2>>2)&1u;
    t->wire_green=(f2>>3)&1u;
    t->brick_style=(f2>>4)&7u;
    t->actuator=(f3>>1)&1u;
    t->inactive=(f3>>2)&1u;
    t->wire_yellow=(f3>>5)&1u;
    t->invisible_block=(f4>>1)&1u;
    t->invisible_wall=(f4>>2)&1u;
    t->fullbright_block=(f4>>3)&1u;
    t->fullbright_wall=(f4>>4)&1u;
#undef TILE_U8
#undef TILE_U16
    return *off<=end;
    }
/* ==================================================================== * write_tile -- Serialize a tile back to binary * ==================================================================== */void write_tile(TxWorld *w,TxBuf *b,const TxTile *t,uint32_t same){
    uint8_t f1=0,f2=0,f3=0,f4=0;
    if (t->active)f1|=2u;
    if (t->wall)f1|=4u;
    if (t->liquid_amount&&t->liquid_type){
        uint8_t l=t->liquid_type;
        if (l==4u){
            l=1u;
            f3|=128u;
            }
        f1|=(uint8_t)((l&3u)<<3);
        }
    if (t->active&&t->type>255u)f1|=32u;
    if (same)f1|=(same<=255u)?64u:128u;
    if (t->wire_red)f2|=2u;
    if (t->wire_blue)f2|=4u;
    if (t->wire_green)f2|=8u;
    if (t->brick_style)f2|=(uint8_t)((t->brick_style&7u)<<4);
    if (t->actuator)f3|=2u;
    if (t->inactive)f3|=4u;
    if (t->active&&t->tile_color)f3|=8u;
    if (t->wall&&t->wall_color)f3|=16u;
    if (t->wire_yellow)f3|=32u;
    if (t->wall>255u)f3|=64u;
    if (t->invisible_block)f4|=2u;
    if (t->invisible_wall)f4|=4u;
    if (t->fullbright_block)f4|=8u;
    if (t->fullbright_wall)f4|=16u;
    if (f4)f3|=1u;
    if (f3)f2|=1u;
    if (f2)f1|=1u;
    buf_u8(b,f1);
    if (f1&1u)buf_u8(b,f2);
    if (f2&1u)buf_u8(b,f3);
    if (f3&1u)buf_u8(b,f4);
    if (t->active){
        if (f1&32u)buf_u16le(b,t->type); else buf_u8(b,(uint8_t)t->type);
        if (tile_important(w,t->type)){
            buf_u16le(b,(uint16_t)t->frame_x);
            buf_u16le(b,(uint16_t)t->frame_y);
            }
        }
    if (f3&8u)buf_u8(b,t->tile_color);
    if (f1&4u)buf_u8(b,(uint8_t)t->wall);
    if (f3&16u)buf_u8(b,t->wall_color);
    if (((f1>>3)&3u)!=0u) buf_u8(b,t->liquid_amount);
    if (f3&64u)buf_u8(b,(uint8_t)(t->wall>>8));
    if (same){ if (same<=255u)buf_u8(b,(uint8_t)same); else buf_u16le(b,same); }
    }
/* ==================================================================== * Section name/index mapping * ==================================================================== */static const char *section_name_by_index(uint32_t index){
    static const char *names[11]={
        "header","tiles","chests","signs","npcs","tile_entities","weighted_pressure_plates","town_manager","bestiary","creative_powers","footer"}
    ;
    return index<11u?names[index]:"section";
    }
/* Map the public modern section number to the versioned pointer-table slot. */
static int tx_actual_section_index(const TxWorld *w, int logical) {
    int idx = logical;
    if (logical == 10) {
        int footer=w->version>=220u?10:w->version>=210u?9:w->version>=189u?8:w->version>=170u?7:w->version>=116u?6:5;
        return (uint32_t)footer<w->pointer_count?footer:-1;
    }
    if (logical == 5 && w->version < 116u) return -1;
    if (logical == 6 && w->version < 170u) return -1;
    if (logical == 7 && w->version < 189u) return -1;
    if (logical == 8 && w->version < 210u) return -1;
    if (logical == 9 && w->version < 220u) return -1;
    return (idx >= 0 && (uint32_t)idx < w->pointer_count) ? idx : -1;
}

int section_index_by_name(const char *name,uint32_t len){
    if (tx_streq_n(name,len,"header"))return 0;
    if (tx_streq_n(name,len,"tiles"))return 1;
    if (tx_streq_n(name,len,"chests"))return 2;
    if (tx_streq_n(name,len,"signs"))return 3;
    if (tx_streq_n(name,len,"npcs"))return 4;
    if (tx_streq_n(name,len,"tile_entities")||tx_streq_n(name,len,"tileEntities"))return 5;
    if (tx_streq_n(name,len,"weighted_pressure_plates")||tx_streq_n(name,len,"weightedPressurePlates"))return 6;
    if (tx_streq_n(name,len,"town_manager")||tx_streq_n(name,len,"townManager"))return 7;
    if (tx_streq_n(name,len,"bestiary"))return 8;
    if (tx_streq_n(name,len,"creative_powers")||tx_streq_n(name,len,"creativePowers"))return 9;
    if (tx_streq_n(name,len,"footer"))return 10;
    if (tx_streq_n(name,len,"format"))return-2; return -1;
    }
/* Directly set section override data without copying. Caller transfers ownership of the data buffer. */int set_section_override_data(TxWorld *w,int idx,uint8_t *data,uint32_t len){
    if (idx<0||(uint32_t)idx>=TX_MAX_SECTION_OVERRIDES){
        tx_set_error("TERRAX_SECTION_SET_NOT_SUPPORTED","section index out of range");
        return 0;
        }
    if (w->section_overrides[idx].active&&w->section_overrides[idx].data&&
        w->section_overrides[idx].data!=data)
        tx_internal_free(w->section_overrides[idx].data);
    if (!w->override_heap_mark)w->override_heap_mark=tx_mark();
    w->section_overrides[idx].data=data;
    w->section_overrides[idx].len=len;
    w->section_overrides[idx].active=1;
    w->heap_mark=tx_mark(); return 1;
    }
/* ==================================================================== * JSON serializers for all 11 sections * * These produce JSON matching the TerraX V2 API exactly, as defined in * world_api_v318_format.cpp, world_api_v318_header.cpp, and * world_api_v318_sections.cpp. * ==================================================================== *//* --- format section --- */void serialize_format_json(TxWorld *w,TxBuf *b){
    buf_cstr(b," { \"version\":");
    json_u32(b,w->version);
    buf_cstr(b,",\"magic\":");
    json_string(b,w->magic[0]?w->magic:"relogic");
    buf_cstr(b,",\"type\":");
    json_u32(b,w->file_type);
    buf_cstr(b,",\"revision\":");
    json_u32(b,w->revision);
    buf_cstr(b,",\"favoriteFlags\":");
    json_u64(b,w->favorite);
    buf_cstr(b,",\"pointerCount\":");
    json_u32(b,w->pointer_count);
    buf_cstr(b,",\"positions\":[");
    for (uint32_t i=0;
    i<w->pointer_count;
    i++){
        if (i)buf_u8(b,',');
        json_u32(b,w->positions[i]);
        }
    buf_cstr(b,"],\"tileTypeCount\":");
    json_u32(b,w->tile_type_count);
    buf_cstr(b,",\"tileFrameImportantBitmap\":[");
    for (uint32_t i=0;
    i<w->tile_type_count;
    i++){
        if (i)buf_u8(b,',');
        uint32_t idx=i>>3;
        uint32_t bit=(idx<w->important_len)?((w->important[idx]>>(i&7u))&1u):0;
        json_bool(b,bit);
        }
    buf_cstr(b,"]\n}\n");
    }
/* --- Convert .NET DateTime.ToBinary() u64 to "YYYY-MM-DD HH:MM:SS" string --- */
static void json_dotnet_binary_date(TxBuf *b, uint64_t raw) {
    /* Mask top 2 bits (Kind), keep lower 62 bits (ticks) */
    uint64_t ticks = (raw << 2) >> 2;
    uint64_t dotnet_epoch = 621355968000000000ULL;
    if (ticks < dotnet_epoch) {
        buf_u8(b, '"'); buf_u8(b, 'N'); buf_u8(b, '/'); buf_u8(b, 'A'); buf_u8(b, '"');
        return;
    }
    uint64_t unix_sec = (ticks - dotnet_epoch) / 10000000ULL;
    uint32_t sod = (uint32_t)(unix_sec % 86400ULL);
    uint32_t hour = sod / 3600;
    uint32_t min = (sod % 3600) / 60;
    uint32_t sec = sod % 60;
    uint32_t days = (uint32_t)(unix_sec / 86400ULL);
    int year = 1970;
    while (days >= 365) {
        int lp = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t diy = lp ? 366u : 365u;
        if (days < diy) break;
        days -= diy;
        year++;
    }
    static const uint8_t mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int lp = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int month = 0;
    int i;
    for (i = 0; i < 12; i++) {
        uint8_t dim = mdays[i];
        if (i == 1 && lp) dim = 29;
        if (days < dim) { month = i; break; }
        days -= dim;
    }
    int day = (int)days + 1;
    month += 1;
    /* Output "YYYY-MM-DD HH:MM:SS" directly via buf_u8, no intermediate buffer */
    buf_u8(b, '"');
    buf_u8(b, (uint8_t)('0' + (year / 1000) % 10));
    buf_u8(b, (uint8_t)('0' + (year / 100) % 10));
    buf_u8(b, (uint8_t)('0' + (year / 10) % 10));
    buf_u8(b, (uint8_t)('0' + year % 10));
    buf_u8(b, '-');
    buf_u8(b, (uint8_t)('0' + month / 10));
    buf_u8(b, (uint8_t)('0' + month % 10));
    buf_u8(b, '-');
    buf_u8(b, (uint8_t)('0' + day / 10));
    buf_u8(b, (uint8_t)('0' + day % 10));
    buf_u8(b, ' ');
    buf_u8(b, (uint8_t)('0' + hour / 10));
    buf_u8(b, (uint8_t)('0' + hour % 10));
    buf_u8(b, ':');
    buf_u8(b, (uint8_t)('0' + min / 10));
    buf_u8(b, (uint8_t)('0' + min % 10));
    buf_u8(b, ':');
    buf_u8(b, (uint8_t)('0' + sec / 10));
    buf_u8(b, (uint8_t)('0' + sec % 10));
    buf_u8(b, '"');
}
/* --- header section (all fields, matching buildHeaderJson order) --- */void serialize_header_json(TxWorld *w,TxBuf *b){
    uint8_t *p=w->file;
    uint32_t flen=w->file_len;
    uint32_t header_base=0u;
    if (w->section_overrides[0].active){
        p=w->section_overrides[0].data;
        flen=w->section_overrides[0].len;
        header_base=w->starts[0];
        }
    buf_cstr(b," { \"worldName\":");
    json_string(b,w->worldName);
    buf_cstr(b,",\"seed\":");
    json_string(b,w->seed);
    buf_cstr(b,",\"worldGeneratorVersion\":");
    json_u64(b,w->worldGeneratorVersion);
    buf_cstr(b,",\"uniqueId\":");
    json_string(b,w->uuid);
    buf_cstr(b,",\"worldId\":");
    json_i32(b,w->worldId);
    buf_cstr(b,",\"leftWorld\":");
    json_i32(b,w->leftWorld);
    buf_cstr(b,",\"rightWorld\":");
    json_i32(b,w->rightWorld);
    buf_cstr(b,",\"topWorld\":");
    json_i32(b,w->topWorld);
    buf_cstr(b,",\"bottomWorld\":");
    json_i32(b,w->bottomWorld);
    buf_cstr(b,",\"maxTilesY\":");
    json_i32(b,w->maxTilesY);
    buf_cstr(b,",\"maxTilesX\":");
    json_i32(b,w->maxTilesX);
    buf_cstr(b,",\"gameMode\":");
    json_i32(b,w->gameMode);
    /* Seed flags */buf_cstr(b,",\"drunkWorld\":");
    json_bool(b,w->drunkWorld);
    buf_cstr(b,",\"getGoodWorld\":");
    json_bool(b,w->ftwWorld);
    buf_cstr(b,",\"tenthAnniversaryWorld\":");
    json_bool(b,w->tenthAnniversaryWorld);
    buf_cstr(b,",\"dontStarveWorld\":");
    json_bool(b,w->dontStarveWorld);
    buf_cstr(b,",\"notTheBeesWorld\":");
    json_bool(b,w->notTheBeesWorld);
    buf_cstr(b,",\"remixWorld\":");
    json_bool(b,w->remixWorld);
    buf_cstr(b,",\"noTrapsWorld\":");
    json_bool(b,w->noTrapsWorld);
    buf_cstr(b,",\"zenithWorld\":");
    json_bool(b,w->zenithWorld);
    buf_cstr(b,",\"skyblockWorld\":");
    json_bool(b,w->skyblockWorld);
    /* Timestamps */buf_cstr(b,",\"creationTime\":");
    json_u64(b,w->creationTime);
    buf_cstr(b,",\"lastPlayed\":");
    json_u64(b,w->lastPlayed);
    /* Readable date strings derived from DateTime.ToBinary() ticks */buf_cstr(b,",\"creationTimeDate\":");
    json_dotnet_binary_date(b,w->creationTime);
    buf_cstr(b,",\"lastPlayedDate\":");
    json_dotnet_binary_date(b,w->lastPlayed);
    /* Terrain / environment */buf_cstr(b,",\"moonType\":");
    json_u32(b,w->moonType);
    buf_cstr(b,",\"treeX\":[");
    json_u32(b,w->treeX[0]);
    buf_u8(b,',');
    json_u32(b,w->treeX[1]);
    buf_u8(b,',');
    json_u32(b,w->treeX[2]);
    buf_cstr(b,"],\"treeStyle\":[");
    json_u32(b,w->treeStyle[0]);
    buf_u8(b,',');
    json_u32(b,w->treeStyle[1]);
    buf_u8(b,',');
    json_u32(b,w->treeStyle[2]);
    buf_u8(b,',');
    json_u32(b,w->treeStyle[3]);
    buf_cstr(b,"],\"caveBackX\":[");
    json_u32(b,w->caveBackX[0]);
    buf_u8(b,',');
    json_u32(b,w->caveBackX[1]);
    buf_u8(b,',');
    json_u32(b,w->caveBackX[2]);
    buf_cstr(b,"],\"caveBackStyle\":[");
    json_u32(b,w->caveBackStyle[0]);
    buf_u8(b,',');
    json_u32(b,w->caveBackStyle[1]);
    buf_u8(b,',');
    json_u32(b,w->caveBackStyle[2]);
    buf_u8(b,',');
    json_u32(b,w->caveBackStyle[3]);
    buf_cstr(b,"],\"iceBackStyle\":");
    json_u32(b,w->iceBackStyle);
    buf_cstr(b,",\"jungleBackStyle\":");
    json_u32(b,w->jungleBackStyle);
    buf_cstr(b,",\"hellBackStyle\":");
    json_u32(b,w->hellBackStyle);
    /* Spawn / surface / rock */buf_cstr(b,",\"spawnTileX\":");
    json_i32(b,w->spawnTileX);
    buf_cstr(b,",\"spawnTileY\":");
    json_i32(b,w->spawnTileY);
    buf_cstr(b,",\"worldSurface\":");
    json_float(b,w->worldSurface);
    buf_cstr(b,",\"rockLayer\":");
    json_float(b,w->rockLayer);
    /* Time / state */buf_cstr(b,",\"time\":");
    json_float(b,w->gameTime);
    buf_cstr(b,",\"dayTime\":");
    json_bool(b,w->isDayTime);
    buf_cstr(b,",\"moonPhase\":");
    json_u32(b,w->moonPhase);
    buf_cstr(b,",\"bloodMoon\":");
    json_bool(b,w->isBloodMoon);
    buf_cstr(b,",\"eclipse\":");
    json_bool(b,w->isEclipse);
    buf_cstr(b,",\"dungeonX\":");
    json_i32(b,w->dungeonX);
    buf_cstr(b,",\"dungeonY\":");
    json_i32(b,w->dungeonY);
    buf_cstr(b,",\"crimson\":");
    json_bool(b,w->isCrimson);
    /* Boss / event progress */buf_cstr(b,",\"downedEyeOfCthulhu\":");
    json_bool(b,w->downedEye);
    buf_cstr(b,",\"downedEaterOfWorldsOrBrainOfCthulhu\":");
    json_bool(b,w->downedEaterBrain);
    buf_cstr(b,",\"downedSkeletron\":");
    json_bool(b,w->downedSkeletron);
    buf_cstr(b,",\"downedQueenBee\":");
    json_bool(b,w->downedQueenBee);
    buf_cstr(b,",\"downedDestroyer\":");
    json_bool(b,w->downedDestroyer);
    buf_cstr(b,",\"downedTwins\":");
    json_bool(b,w->downedTwins);
    buf_cstr(b,",\"downedSkeletronPrime\":");
    json_bool(b,w->downedSkeletronPrime);
    buf_cstr(b,",\"downedAnyMechBoss\":");
    json_bool(b,w->downedAnyMech);
    buf_cstr(b,",\"downedPlantera\":");
    json_bool(b,w->downedPlantera);
    buf_cstr(b,",\"downedGolem\":");
    json_bool(b,w->downedGolem);
    buf_cstr(b,",\"downedKingSlime\":");
    json_bool(b,w->downedKingSlime);
    buf_cstr(b,",\"savedGoblin\":");
    json_bool(b,w->savedGoblin);
    buf_cstr(b,",\"savedWizard\":");
    json_bool(b,w->savedWizard);
    buf_cstr(b,",\"savedMech\":");
    json_bool(b,w->savedMech);
    buf_cstr(b,",\"downedGoblins\":");
    json_bool(b,w->downedGoblins);
    buf_cstr(b,",\"downedClown\":");
    json_bool(b,w->downedClown);
    buf_cstr(b,",\"downedFrost\":");
    json_bool(b,w->downedFrost);
    buf_cstr(b,",\"downedPirates\":");
    json_bool(b,w->downedPirates);
    /* World state */buf_cstr(b,",\"shadowOrbSmashed\":");
    json_bool(b,w->shadowOrbSmashed);
    buf_cstr(b,",\"spawnMeteor\":");
    json_bool(b,w->spawnMeteor);
    buf_cstr(b,",\"shadowOrbCount\":");
    json_u32(b,w->shadowOrbCount);
    buf_cstr(b,",\"altarCount\":");
    json_u32(b,w->altarCount);
    buf_cstr(b,",\"hardMode\":");
    json_bool(b,w->hardMode);
    buf_cstr(b,",\"afterPartyOfDoom\":");
    json_bool(b,w->afterPartyOfDoom);
    /* Invasion */buf_cstr(b,",\"invasionDelay\":");
    json_u32(b,w->invasionDelay);
    buf_cstr(b,",\"invasionSize\":");
    json_u32(b,w->invasionSize);
    buf_cstr(b,",\"invasionType\":");
    json_u32(b,w->invasionType);
    buf_cstr(b,",\"invasionX\":");
    json_float(b,w->invasionX);
    buf_cstr(b,",\"slimeRainTime\":");
    json_float(b,w->slimeRainTime);
    buf_cstr(b,",\"sundialCooldown\":");
    json_u32(b,w->sundialCooldown);
    /* Weather */buf_cstr(b,",\"raining\":");
    json_bool(b,w->isRaining);
    buf_cstr(b,",\"rainTime\":");
    json_u32(b,w->rainTime);
    buf_cstr(b,",\"maxRain\":");
    json_float(b,(double)w->maxRain);
    /* Ore tiers */buf_cstr(b,",\"oreTierCobalt\":");
    json_i32(b,w->oreTierCobalt);
    buf_cstr(b,",\"oreTierMythril\":");
    json_i32(b,w->oreTierMythril);
    buf_cstr(b,",\"oreTierAdamantite\":");
    json_i32(b,w->oreTierAdamantite);
    /* Backgrounds */buf_cstr(b,",\"treeBG1\":");
    json_u32(b,w->bgTree);
    buf_cstr(b,",\"corruptBG\":");
    json_u32(b,w->bgCorruption);
    buf_cstr(b,",\"jungleBG\":");
    json_u32(b,w->bgJungle);
    buf_cstr(b,",\"snowBG\":");
    json_u32(b,w->bgSnow);
    buf_cstr(b,",\"hallowBG\":");
    json_u32(b,w->bgHallow);
    buf_cstr(b,",\"crimsonBG\":");
    json_u32(b,w->bgCrimson);
    buf_cstr(b,",\"desertBG\":");
    json_u32(b,w->bgDesert);
    buf_cstr(b,",\"oceanBG\":");
    json_u32(b,w->bgOcean);
    buf_cstr(b,",\"cloudBGActive\":");
    json_i32(b,w->cloudBgActive);
    buf_cstr(b,",\"numClouds\":");
    json_u32(b,w->numClouds);
    buf_cstr(b,",\"windSpeedTarget\":");
    json_float(b,(double)w->windSpeedSet);
    /* Angler */buf_cstr(b,",\"anglerWhoFinishedTodayCount\":");
    json_u32(b,w->anglerFinishedSize);
    buf_cstr(b,",\"anglerWhoFinishedToday\":[");
    {
        uint32_t aoff=w->anglersOff-header_base;
        for (uint32_t i=0;
        i<w->anglerFinishedSize;
        i++){
            if (i)buf_u8(b,',');
            char aname[256];
            rd_string_copy(p,flen,&aoff,aname,256);
            json_string(b,aname);
            }
        }
    buf_cstr(b,"],\"savedAngler\":");
    json_bool(b,w->savedAngler);
    buf_cstr(b,",\"anglerQuest\":");
    json_u32(b,w->anglerQuest);
    buf_cstr(b,",\"savedStylist\":");
    json_bool(b,w->savedStylist);
    buf_cstr(b,",\"savedTaxCollector\":");
    json_bool(b,w->savedTaxCollector);
    buf_cstr(b,",\"savedGolfer\":");
    json_bool(b,w->savedGolfer);
    buf_cstr(b,",\"invasionSizeStart\":");
    json_u32(b,w->invasionSizeStart);
    buf_cstr(b,",\"cultistDelay\":");
    json_u32(b,w->cultistDelay);
    /* Kill counts */buf_cstr(b,",\"killCountLength\":");
    json_u32(b,w->numMobs);
    buf_cstr(b,",\"killCount\":[");
    {
        uint32_t moff=w->mobsOff-header_base;
        for (uint32_t i=0;
        i<w->numMobs;
        i++){
            if (i)buf_u8(b,',');
            json_u32(b,rd_u32le(p,flen,&moff));
            }
        }
    buf_cstr(b,"],\"claimableBannersLength\":");
    json_u32(b,w->numClaimableBanners);
    buf_cstr(b,",\"claimableBanners\":[");
    {
        uint32_t boff=w->claimableBannersOff-header_base;
        for (uint32_t i=0;
        i<w->numClaimableBanners;
        i++){
            if (i)buf_u8(b,',');
            json_u32(b,rd_u16le(p,flen,&boff));
            }
        }
    /* Late events */buf_cstr(b,"],\"fastForwardTimeToDawn\":");
    json_bool(b,w->fastForwardTime);
    buf_cstr(b,",\"downedFishron\":");
    json_bool(b,w->downedFishron);
    buf_cstr(b,",\"downedMartians\":");
    json_bool(b,w->downedMartians);
    buf_cstr(b,",\"downedAncientCultist\":");
    json_bool(b,w->downedLunaticCultist);
    buf_cstr(b,",\"downedMoonlord\":");
    json_bool(b,w->downedMoonlord);
    buf_cstr(b,",\"downedHalloweenKing\":");
    json_bool(b,w->downedHalloweenKing);
    buf_cstr(b,",\"downedHalloweenTree\":");
    json_bool(b,w->downedHalloweenTree);
    buf_cstr(b,",\"downedChristmasIceQueen\":");
    json_bool(b,w->downedChristmasIceQueen);
    buf_cstr(b,",\"downedChristmasSantank\":");
    json_bool(b,w->downedSanta);
    buf_cstr(b,",\"downedChristmasTree\":");
    json_bool(b,w->downedChristmasTree);
    /* Celestial towers */buf_cstr(b,",\"downedTowerSolar\":");
    json_bool(b,w->downedCelestialSolar);
    buf_cstr(b,",\"downedTowerVortex\":");
    json_bool(b,w->downedCelestialVortex);
    buf_cstr(b,",\"downedTowerNebula\":");
    json_bool(b,w->downedCelestialNebula);
    buf_cstr(b,",\"downedTowerStardust\":");
    json_bool(b,w->downedCelestialStardust);
    buf_cstr(b,",\"towerActiveSolar\":");
    json_bool(b,w->downedTowerSolar);
    buf_cstr(b,",\"towerActiveVortex\":");
    json_bool(b,w->downedTowerVortex);
    buf_cstr(b,",\"towerActiveNebula\":");
    json_bool(b,w->downedTowerNebula);
    buf_cstr(b,",\"towerActiveStardust\":");
    json_bool(b,w->downedTowerStardust);
    buf_cstr(b,",\"lunarApocalypseIsUp\":");
    json_bool(b,w->downedTowerAncient);
    /* Party */buf_cstr(b,",\"partyManual\":");
    json_bool(b,w->partyManual);
    buf_cstr(b,",\"partyGenuine\":");
    json_bool(b,w->partyGenuine);
    buf_cstr(b,",\"partyCooldown\":");
    json_u32(b,w->partyCooldown);
    buf_cstr(b,",\"partyCelebratingNpcCount\":");
    json_u32(b,w->partyCelebratingNPCSize);
    buf_cstr(b,",\"partyCelebratingNpcNetIds\":[");
    {
        uint32_t poff=w->partyCelebratingNPCsOff-header_base;
        for (uint32_t i=0;
        i<w->partyCelebratingNPCSize;
        i++){
            if (i)buf_u8(b,',');
            json_i32(b,rd_i32le(p,flen,&poff));
            }
        }
    /* Sandstorm */buf_cstr(b,"],\"sandstormHappening\":");
    json_bool(b,w->sandstormHappening);
    buf_cstr(b,",\"sandstormTimeLeft\":");
    json_u32(b,w->sandStormTime);
    buf_cstr(b,",\"sandstormSeverity\":");
    json_float(b,(double)w->sandStormSeverity);
    buf_cstr(b,",\"sandstormIntendedSeverity\":");
    json_float(b,(double)w->sandstormIntendedSeverity);
    /* DD2 */buf_cstr(b,",\"savedBartender\":");
    json_bool(b,w->savedBartender);
    buf_cstr(b,",\"dd2DownedT1\":");
    json_bool(b,w->downedInvasionT1);
    buf_cstr(b,",\"dd2DownedT2\":");
    json_bool(b,w->downedInvasionT2);
    buf_cstr(b,",\"dd2DownedT3\":");
    json_bool(b,w->downedInvasionT3);
    /* More backgrounds */buf_cstr(b,",\"mushroomBG\":");
    json_u32(b,w->mushroomBg);
    buf_cstr(b,",\"underworldBG\":");
    json_u32(b,w->undergroundDesertBg);
    buf_cstr(b,",\"treeBG2\":");
    json_u32(b,w->bgTree2);
    buf_cstr(b,",\"treeBG3\":");
    json_u32(b,w->bgTree3);
    buf_cstr(b,",\"treeBG4\":");
    json_u32(b,w->bgTree4);
    /* 1.4+ */buf_cstr(b,",\"combatBookWasUsed\":");
    json_bool(b,w->combatBookUsed);
    buf_cstr(b,",\"lanternNightCooldown\":");
    json_u32(b,w->lanternNightCooldown);
    buf_cstr(b,",\"lanternNightGenuine\":");
    json_bool(b,w->lanternNightGenuine);
    buf_cstr(b,",\"lanternNightManual\":");
    json_bool(b,w->lanternNightManual);
    buf_cstr(b,",\"lanternNightNextNightIsGenuine\":");
    json_bool(b,w->lanternNightNextNightIsGenuine);
    /* treeTopVariations */buf_cstr(b,",\"treeTopVariationCount\":");
    json_u32(b,w->treetopSize);
    buf_cstr(b,",\"treeTopVariations\":[");
    {
        uint32_t toff=w->treeTopVariationsOff-header_base;
        for (uint32_t i=0;
        i<w->treetopSize;
        i++){
            if (i)buf_u8(b,',');
            json_i32(b,rd_i32le(p,flen,&toff));
            }
        }
    buf_cstr(b,"],\"forceHalloweenForToday\":");
    json_bool(b,w->forceHalloweenForToday);
    buf_cstr(b,",\"forceXMasForToday\":");
    json_bool(b,w->forceXMasForToday);
    buf_cstr(b,",\"oreTierCopper\":");
    json_u32(b,w->savedOreTiersCopper);
    buf_cstr(b,",\"oreTierIron\":");
    json_u32(b,w->savedOreTiersIron);
    buf_cstr(b,",\"oreTierSilver\":");
    json_u32(b,w->savedOreTiersSilver);
    buf_cstr(b,",\"oreTierGold\":");
    json_u32(b,w->savedOreTiersGold);
    buf_cstr(b,",\"boughtCat\":");
    json_bool(b,w->boughtCat);
    buf_cstr(b,",\"boughtDog\":");
    json_bool(b,w->boughtDog);
    buf_cstr(b,",\"boughtBunny\":");
    json_bool(b,w->boughtBunny);
    buf_cstr(b,",\"downedEmpressOfLight\":");
    json_bool(b,w->downedEmpressOfLight);
    buf_cstr(b,",\"downedQueenSlime\":");
    json_bool(b,w->downedQueenSlime);
    buf_cstr(b,",\"downedDeerclops\":");
    json_bool(b,w->downedDeerclops);
    /* Unlocked spawns */buf_cstr(b,",\"unlockedSlimeBlueSpawn\":");
    json_bool(b,w->unlockedSlimeBlueSpawn);
    buf_cstr(b,",\"unlockedMerchantSpawn\":");
    json_bool(b,w->unlockedMerchantSpawn);
    buf_cstr(b,",\"unlockedDemolitionistSpawn\":");
    json_bool(b,w->unlockedDemolitionistSpawn);
    buf_cstr(b,",\"unlockedPartyGirlSpawn\":");
    json_bool(b,w->unlockedPartyGirlSpawn);
    buf_cstr(b,",\"unlockedDyeTraderSpawn\":");
    json_bool(b,w->unlockedDyeTraderSpawn);
    buf_cstr(b,",\"unlockedTruffleSpawn\":");
    json_bool(b,w->unlockedTruffleSpawn);
    buf_cstr(b,",\"unlockedArmsDealerSpawn\":");
    json_bool(b,w->unlockedArmsDealerSpawn);
    buf_cstr(b,",\"unlockedNurseSpawn\":");
    json_bool(b,w->unlockedNurseSpawn);
    buf_cstr(b,",\"unlockedPrincessSpawn\":");
    json_bool(b,w->unlockedPrincessSpawn);
    buf_cstr(b,",\"combatBookVolumeTwoWasUsed\":");
    json_bool(b,w->combatBookVolumeTwoWasUsed);
    buf_cstr(b,",\"peddlersSatchelWasUsed\":");
    json_bool(b,w->peddlersSatchelWasUsed);
    buf_cstr(b,",\"unlockedSlimeGreenSpawn\":");
    json_bool(b,w->unlockedSlimeGreenSpawn);
    buf_cstr(b,",\"unlockedSlimeOldSpawn\":");
    json_bool(b,w->unlockedSlimeOldSpawn);
    buf_cstr(b,",\"unlockedSlimePurpleSpawn\":");
    json_bool(b,w->unlockedSlimePurpleSpawn);
    buf_cstr(b,",\"unlockedSlimeRainbowSpawn\":");
    json_bool(b,w->unlockedSlimeRainbowSpawn);
    buf_cstr(b,",\"unlockedSlimeRedSpawn\":");
    json_bool(b,w->unlockedSlimeRedSpawn);
    buf_cstr(b,",\"unlockedSlimeYellowSpawn\":");
    json_bool(b,w->unlockedSlimeYellowSpawn);
    buf_cstr(b,",\"unlockedSlimeCopperSpawn\":");
    json_bool(b,w->unlockedSlimeCopperSpawn);
    buf_cstr(b,",\"fastForwardTimeToDusk\":");
    json_bool(b,w->fastForwardTimeToDusk);
    buf_cstr(b,",\"moondialCooldown\":");
    json_u32(b,w->moondialCooldown);
    buf_cstr(b,",\"forceHalloweenForever\":");
    json_bool(b,w->forceHalloweenForever);
    buf_cstr(b,",\"forceXMasForever\":");
    json_bool(b,w->forcexmasForever);
    buf_cstr(b,",\"vampireSeed\":");
    json_bool(b,w->vampireSeed);
    buf_cstr(b,",\"meteorShowerCount\":");
    json_u32(b,w->tempmeteorShowerCount);
    buf_cstr(b,",\"coinRain\":");
    json_u32(b,w->tempcoinRain);
    buf_cstr(b,",\"infectedSeed\":");
    json_bool(b,w->infectedSeed);
    /* Spawn points */buf_cstr(b,",\"teamBasedSpawnsSeed\":");
    json_bool(b,w->teambasedSpawnsSeed);
    buf_cstr(b,",\"spawnPointCount\":");
    json_u32(b,w->numExtradSpawnPointManager);
    buf_cstr(b,",\"spawnPoints\":[");
    {
        uint32_t soff=w->extradSpawnPointManagerOff-header_base;
        for (uint8_t i=0;
        i<w->numExtradSpawnPointManager;
        i++){
            if (i)buf_u8(b,',');
            uint32_t raw=rd_i32le(p,flen,&soff);
            buf_cstr(b," { \"x\":");
            json_i32(b,(int16_t)((uint32_t)raw&0xFFFFu));
            buf_cstr(b,",\"y\":");
            json_i32(b,(int16_t)(((uint32_t)raw>>16)&0xFFFFu));
            buf_cstr(b,"\n}\n");
            }
        }
    buf_cstr(b,"],\"dualDungeonsSeed\":");
    json_bool(b,w->dualdungeonsSeed);
    buf_cstr(b,",\"legacySkip\":");
    json_u32(b,w->legacySkip);
    /* manifestJson */buf_cstr(b,",\"manifestJson\":");
    {
        uint32_t moff=w->maniFestOff-header_base;
        char manifest_buf[4096];
        uint32_t cap=w->maniFestLen<4095u?w->maniFestLen+1u:4096u;
        rd_string_copy(p,flen,&moff,manifest_buf,cap);
        json_string(b,manifest_buf);
        }
    buf_cstr(b,"\n}\n");
    }
/* --- chests section --- *//* * Binary format per chest (version >= 294): * i32 x, i32 y, 7bit-string name, i32 maxItems * For each item: u16 stack;  stack != 0: i32 itemType, u8 prefix */void serialize_chests_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,2);
    uint32_t off=w->section_overrides[2].active?0u:(section<0?0u:w->starts[section]);
    uint32_t end=w->section_overrides[2].active?w->section_overrides[2].len:(section<0?0u:w->ends[section]);
    uint8_t *p=w->section_overrides[2].active?w->section_overrides[2].data:w->file;
    uint32_t len=w->section_overrides[2].active?w->section_overrides[2].len:w->file_len;
    buf_u8(b,'[');
    uint32_t chest_count=(int16_t)rd_u16le(p,len,&off);
    uint32_t slots_per_chest=0;
    if (w->version<294u)slots_per_chest=(int16_t)rd_u16le(p,len,&off);
    if (chest_count<0)chest_count=0;
    for (int32_t c=0;
    c<chest_count&&off<end;
    c++){
        if (c)buf_u8(b,',');
        uint32_t cx=rd_i32le(p,len,&off);
        uint32_t cy=rd_i32le(p,len,&off);
        char chest_name[256];
        rd_string_copy(p,len,&off,chest_name,256);
        uint32_t max_items=w->version>=294u?rd_i32le(p,len,&off):slots_per_chest;
        if (max_items<0)max_items=0;
        if (max_items>504)max_items=504;
        buf_cstr(b," { \"x\":");
        json_i32(b,cx);
        buf_cstr(b,",\"y\":");
        json_i32(b,cy);
        buf_cstr(b,",\"name\":");
        json_string(b,chest_name);
        buf_cstr(b,",\"maxItems\":");
        json_i32(b,max_items);
        buf_cstr(b,",\"items\":[");
        for (int32_t j=0;
        j<max_items&&off<end;
        j++){
            int16_t stack=(int16_t)rd_u16le(p,len,&off);
            if (j)buf_u8(b,',');
            if (stack!=0){
                int32_t item_type=rd_i32le(p,len,&off);
                uint8_t prefix=rd_u8(p,len,&off);
                buf_cstr(b," { \"stack\":");
                json_i32(b,stack<0?1:stack);
                buf_cstr(b,",\"itemType\":");
                json_i32(b,item_type);
                buf_cstr(b,",\"prefix\":");
                json_u32(b,prefix);
                buf_cstr(b,"\n}\n");
                }
            else buf_cstr(b,"null");
            }
        buf_cstr(b,"]\n}\n");
        }
    buf_u8(b,']');
    }
/* --- signs section --- *//* Each record is: 7-bit UTF-8 text, int32 X, int32 Y. */void serialize_signs_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,3); uint32_t off=section<0?0u:w->starts[section];
    uint32_t end=section<0?0u:w->ends[section];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    uint32_t sign_count=(int16_t)rd_u16le(p,len,&off);
    if (sign_count<0)sign_count=0;
    for (int32_t i=0;
    i<sign_count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        char text[1024];
        rd_string_copy(p,len,&off,text,1024);
        uint32_t sx=rd_i32le(p,len,&off);
        uint32_t sy=rd_i32le(p,len,&off);
        buf_cstr(b," { \"x\":");
        json_i32(b,sx);
        buf_cstr(b,",\"y\":");
        json_i32(b,sy);
        buf_cstr(b,",\"text\":");
        json_string(b,text);
        buf_cstr(b,"\n}\n");
        }
    buf_u8(b,']');
    }
/* --- npcs section --- *//* * Binary format: * Loop: i32 npcType;  >= 0: f32 x, f32 y, 7bit-string name, ... * Terminator: npcType < 0 * Then: u32 shimmeredCount, [i32 shimmeredNetId...] */void serialize_npcs_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,4); uint32_t off=section<0?0u:w->starts[section];
    uint32_t end=section<0?0u:w->ends[section];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    /* * NPC section binary format (v318): * [v268+] shimmered_count (u32) + shimmered_count * netId (u32) * Town NPCs loop: hasNPCs (u8) then for each: * [v190+] SpriteId (i32) * DisplayName (vString: varint-len + bytes) * X (f32), Y (f32) * IsHomeless (u8) * HomeX (u32), HomeY (u32) * [v213+] hasVariation (u8) + optional VariationIndex (i32) * [v315+] HomelessDespawn (u8) * Persistent NPCs loop: hasNPCs (u8) then for each: * SpriteId (i32) * X (f32), Y (f32) * Terminator: hasNPCs byte = 0 */uint32_t scan_off=off;
    /* --- Read shimmered section (v268+) --- */uint32_t shimmered_count=0;
    uint32_t shimmered_start=off;
    if (w->version>=268u&&terra_reader_has(scan_off,4u,end)){
        shimmered_count=rd_u32le(p,len,&scan_off);
        shimmered_start=scan_off;
        /* Skip past the shimmered net IDs */for (uint32_t i=0;
        i<shimmered_count&&terra_reader_has(scan_off,4u,end);
        i++){
            rd_u32le(p,len,&scan_off);
            }
        }
    /* --- Count and locate town NPCs --- */uint32_t town_npc_start=scan_off;
    uint32_t town_npc_count=0;
    while (scan_off<end){
        uint8_t has_npc=rd_u8(p,len,&scan_off);
        if (!has_npc) break;
        /* NPC type: legacy versions store a legacy NPC name string. */
        if (w->version>=190u) {
            if (!terra_reader_has(scan_off,4u,end)) break;
            rd_skip(p,len,&scan_off,4);
        } else {
            rd_skip_string_value(p,len,&scan_off);
        }
        /* DisplayName (vString: varint length + bytes) */rd_skip_string_value(p,len,&scan_off);
        /* X, Y (f32 each) */rd_skip(p,len,&scan_off,8);
        /* IsHomeless (u8) */rd_skip(p,len,&scan_off,1);
        /* HomeX, HomeY (u32 each) */rd_skip(p,len,&scan_off,8);
        /* [v213+] hasVariation + optional */if (w->version>=213u){
            uint8_t has_var=rd_u8(p,len,&scan_off);
            if (has_var&1u)rd_skip(p,len,&scan_off,4);
            }
        /* [v315+] homelessDespawn */if (w->version>=315u){
            rd_skip(p,len,&scan_off,1);
            }
        town_npc_count++;
        }
    /* Skip the town NPC terminator byte (already read as has_npc=0) *//* scan_off is now at the start of persistent NPCs *//* --- Count and locate persistent NPCs --- */uint32_t persistent_start=scan_off;
    uint32_t persistent_count=0;
    while (w->version>=140u&&scan_off<end){
        uint8_t has_npc=rd_u8(p,len,&scan_off);
        if (!has_npc) break;
        /* Persistent NPC type follows the same version gate as town NPCs. */
        if (w->version>=190u) {
            if (!terra_reader_has(scan_off,12u,end)) break;
            rd_skip(p,len,&scan_off,12);
        } else {
            rd_skip_string_value(p,len,&scan_off);
            if (!terra_reader_has(scan_off,8u,end)) break;
            rd_skip(p,len,&scan_off,8);
        }
        persistent_count++;
        }
    /* === Serialize === *//* Shimmered */buf_cstr(b," { \"shimmeredTownNpcNetIds\":[");
    uint32_t shimmer_off=shimmered_start;
    for (uint32_t i=0;
    i<shimmered_count&&terra_reader_has(shimmer_off,4u,end);
    i++){
        if (i)buf_u8(b,',');
        json_i32(b,rd_i32le(p,len,&shimmer_off));
        }
    buf_cstr(b,"],\"shimmeredTownNpcCount\":");
    json_u32(b,shimmered_count);
    /* Town NPCs */buf_cstr(b,",\"townNpcs\":[");
    uint32_t npc_off=town_npc_start;
    for (uint32_t i=0;
    i<town_npc_count&&npc_off<end;
    i++){
        /* Read hasNPCs byte */uint8_t has_npc=rd_u8(p,len,&npc_off);
        if (!has_npc) break;
        if (i)buf_u8(b,',');
        /* NPC type */int32_t npc_type=0;
        char npc_type_name[256]; npc_type_name[0]=0;
        if (w->version>=190u) npc_type=rd_i32le(p,len,&npc_off);
        else rd_string_copy(p,len,&npc_off,npc_type_name,256);
        /* DisplayName */char npc_name[256];
        rd_string_copy(p,len,&npc_off,npc_name,256);
        /* X, Y as float s */union{
            uint32_t u;
            float f;
            }
        ux,uy;
        ux.u=rd_u32le(p,len,&npc_off);
        uy.u=rd_u32le(p,len,&npc_off);
        /* IsHomeless */uint8_t homeless=rd_u8(p,len,&npc_off);
        /* HomeX, HomeY */uint32_t home_x=rd_u32le(p,len,&npc_off);
        uint32_t home_y=rd_u32le(p,len,&npc_off);
        /* [v213+] variation */int32_t variation=0;
        uint8_t has_variation=0;
        if (w->version>=213u){
            has_variation=rd_u8(p,len,&npc_off);
            if (has_variation&1u)variation=rd_i32le(p,len,&npc_off);
            }
        /* [v315+] homelessDespawn */uint8_t homeless_despawn=0;
        if (w->version>=315u){
            homeless_despawn=rd_u8(p,len,&npc_off);
            }
        buf_cstr(b," { \"npcNetId\":");
        json_i32(b,w->version>=190u?npc_type:tx_legacy_npc_id(npc_type_name));
        if (w->version<190u) {
            buf_cstr(b,",\"legacyTypeName\":");
            json_string(b,npc_type_name);
        }
        buf_cstr(b,",\"givenName\":");
        json_string(b,npc_name);
        buf_cstr(b,",\"positionX\":");
        json_float(b,(double)ux.f);
        buf_cstr(b,",\"positionY\":");
        json_float(b,(double)uy.f);
        buf_cstr(b,",\"homeless\":");
        json_bool(b,homeless);
        buf_cstr(b,",\"homeTileX\":");
        json_u32(b,home_x);
        buf_cstr(b,",\"homeTileY\":");
        json_u32(b,home_y);
        if (has_variation&1u){
            buf_cstr(b,",\"townNpcVariationIndex\":");
            json_i32(b,variation);
            }
        buf_cstr(b,",\"homelessDespawn\":");
        json_bool(b,homeless_despawn);
        buf_cstr(b,"\n}\n");
        }
    /* Persistent NPCs */buf_cstr(b,"],\"persistentNpcs\":[");
    uint32_t mob_off=persistent_start;
    for (uint32_t i=0;
    i<persistent_count&&mob_off<end;
    i++){
        uint8_t has_npc=rd_u8(p,len,&mob_off);
        if (!has_npc) break;
        if (i)buf_u8(b,',');
        int32_t npc_type=0;
        char npc_type_name[256]; npc_type_name[0]=0;
        if (w->version>=190u) npc_type=rd_i32le(p,len,&mob_off);
        else rd_string_copy(p,len,&mob_off,npc_type_name,256);
        union{
            uint32_t u;
            float f;
            }
        mx,my;
        mx.u=rd_u32le(p,len,&mob_off);
        my.u=rd_u32le(p,len,&mob_off);
        buf_cstr(b," { \"npcNetId\":");
        json_i32(b,w->version>=190u?npc_type:tx_legacy_npc_id(npc_type_name));
        if (w->version<190u) {
            buf_cstr(b,",\"legacyTypeName\":");
            json_string(b,npc_type_name);
        }
        buf_cstr(b,",\"positionX\":");
        json_float(b,(double)mx.f);
        buf_cstr(b,",\"positionY\":");
        json_float(b,(double)my.f);
        buf_cstr(b,"\n}\n");
        }
    buf_cstr(b,"]\n}\n");
    }
/* --- tile_entities section --- *//* * Binary format: * u16 count * For each entity: * u8 type, i32 id, u16 x, u16 y * Then type-specific data */static void serialize_item_stack_json(TxBuf *b,int16_t id,uint8_t prefix,uint16_t stack){
    buf_cstr(b," { \"itemType\":");
    json_i32(b,id);
    buf_cstr(b,",\"prefix\":");
    json_u32(b,prefix);
    buf_cstr(b,",\"stack\":");
    json_u32(b,stack);
    buf_cstr(b,"\n}\n");
    }
void serialize_tile_entities_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,5); uint32_t off=section<0?0u:w->starts[section];
    uint32_t end=section<0?0u:w->ends[section];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    /* v116-v121 contain the legacy dummy section, not TileEntity records. */
    if (w->version<122u) {
        uint32_t count=terra_reader_has(off,4u,end)?rd_u32le(p,end,&off):0u;
        for (uint32_t i=0;i<count && terra_reader_has(off,4u,end);i++) {
            if (i) buf_u8(b,',');
            int16_t x=(int16_t)rd_u16le(p,end,&off),y=(int16_t)rd_u16le(p,end,&off);
            buf_cstr(b,"{\"legacyDummy\":true,\"positionX\":"); json_i32(b,x);
            buf_cstr(b,",\"positionY\":"); json_i32(b,y); buf_u8(b,'}');
        }
        buf_u8(b,']'); return;
    }
    uint32_t count=terra_reader_has(off,4u,end)?rd_u32le(p,len,&off):0u;
    for (int32_t i=0;
    i<count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        uint8_t entity_type=rd_u8(p,len,&off);
        uint32_t entity_id=rd_i32le(p,len,&off);
        uint16_t pos_x=rd_u16le(p,len,&off);
        uint16_t pos_y=rd_u16le(p,len,&off);
        buf_cstr(b," { \"entityType\":");
        json_u32(b,entity_type);
        buf_cstr(b,",\"entityId\":");
        json_i32(b,entity_id);
        buf_cstr(b,",\"positionX\":");
        json_u32(b,pos_x);
        buf_cstr(b,",\"positionY\":");
        json_u32(b,pos_y);
        switch (entity_type){
            case 0:{
                /* TrainingDummy */int16_t npc=(int16_t)rd_u16le(p,len,&off);
                buf_cstr(b,",\"npc\":");
                json_i32(b,npc);
                break;
                }
            case 1:/* ItemFrame */case 4:/* WeaponRack */case 6:/* FoodPlatter */case 8:/* DeadCellsDisplayJar */{
                int16_t item_id=(int16_t)rd_u16le(p,len,&off);
                uint8_t prefix=rd_u8(p,len,&off);
                uint16_t stack=rd_u16le(p,len,&off);
                buf_cstr(b,",\"itemType\":");
                json_i32(b,item_id);
                buf_cstr(b,",\"prefix\":");
                json_u32(b,prefix);
                buf_cstr(b,",\"stack\":");
                json_u32(b,stack);
                break;
                }
            case 2:{
                /* LogicSensor */uint8_t logic_check=rd_u8(p,len,&off);
                uint8_t on=rd_u8(p,len,&off);
                buf_cstr(b,",\"logicCheck\":");
                json_u32(b,logic_check);
                buf_cstr(b,",\"on\":");
                json_bool(b,on);
                break;
                }
            case 3:{
                /* DisplayDoll */uint8_t equip_mask=rd_u8(p,len,&off);
                uint8_t dye_mask=rd_u8(p,len,&off);
                uint8_t pose=0;
                uint8_t extra_mask=0;
                buf_cstr(b,",\"equipMaskLow\":");
                json_u32(b,equip_mask);
                buf_cstr(b,",\"dyeMaskLow\":");
                json_u32(b,dye_mask);
                if (w->version>=307u){
                    pose=rd_u8(p,len,&off);
                    }
                if (w->version>=308u){
                    extra_mask=rd_u8(p,len,&off);
                    }
                buf_cstr(b,",\"pose\":");
                json_u32(b,pose);
                buf_cstr(b,",\"extraMask\":");
                json_u32(b,extra_mask);
                /* Read equip items */buf_cstr(b,",\"equip\":[");
                {
                    int first=1;
                    for (uint32_t slot=0;
                    slot<8u;
                    slot++){
                        if ((equip_mask>>slot)&1u){
                            int16_t id=(int16_t)rd_u16le(p,len,&off);
                            uint8_t pf=rd_u8(p,len,&off);
                            uint16_t st=rd_u16le(p,len,&off);
                            if (!first) buf_u8(b,',');
                            first=0;
                            serialize_item_stack_json(b,id,pf,st);
                            }
                        }
                    if ((extra_mask>>1)&1u && w->version!=311u){
                        int16_t id=(int16_t)rd_u16le(p,len,&off);
                        uint8_t pf=rd_u8(p,len,&off);
                        uint16_t st=rd_u16le(p,len,&off);
                        if (!first) buf_u8(b,',');
                        first=0;
                        serialize_item_stack_json(b,id,pf,st);
                        }
                    }
                buf_cstr(b,"],\"dyes\":[");
                {
                    int first=1;
                    for (uint32_t slot=0;
                    slot<8u;
                    slot++){
                        if ((dye_mask>>slot)&1u){
                            int16_t id=(int16_t)rd_u16le(p,len,&off);
                            uint8_t pf=rd_u8(p,len,&off);
                            uint16_t st=rd_u16le(p,len,&off);
                            if (!first) buf_u8(b,',');
                            first=0;
                            serialize_item_stack_json(b,id,pf,st);
                            }
                        }
                    if ((extra_mask>>2)&1u){
                        int16_t id=(int16_t)rd_u16le(p,len,&off);
                        uint8_t pf=rd_u8(p,len,&off);
                        uint16_t st=rd_u16le(p,len,&off);
                        if (!first) buf_u8(b,',');
                        first=0;
                        serialize_item_stack_json(b,id,pf,st);
                        }
                    }
                buf_cstr(b,"],\"misc\":[");
                {
                    if (extra_mask&1u){
                        int16_t id=(int16_t)rd_u16le(p,len,&off);
                        uint8_t pf=rd_u8(p,len,&off);
                        uint16_t st=rd_u16le(p,len,&off);
                        serialize_item_stack_json(b,id,pf,st);
                        }
                    }
                buf_cstr(b,"]");
                if (w->version==311u && (extra_mask&2u)) {
                    int16_t id=(int16_t)rd_u16le(p,len,&off);
                    uint8_t pf=rd_u8(p,len,&off);
                    uint16_t st=rd_u16le(p,len,&off);
                    buf_cstr(b,",\"equip8\":");
                    serialize_item_stack_json(b,id,pf,st);
                }
                break;
                }
            case 5:{
                /* HatRack */uint8_t item_mask=rd_u8(p,len,&off);
                buf_cstr(b,",\"itemMask\":");
                json_u32(b,item_mask);
                buf_cstr(b,",\"items\":[");
                {
                    int first=1;
                    for (uint32_t slot=0;
                    slot<2u;
                    slot++){
                        if ((item_mask>>slot)&1u){
                            int16_t id=(int16_t)rd_u16le(p,len,&off);
                            uint8_t pf=rd_u8(p,len,&off);
                            uint16_t st=rd_u16le(p,len,&off);
                            if (!first) buf_u8(b,',');
                            first=0;
                            serialize_item_stack_json(b,id,pf,st);
                            }
                        }
                    }
                buf_cstr(b,"],\"dyes\":[");
                {
                    int first=1;
                    for (uint32_t slot=0;
                    slot<2u;
                    slot++){
                        if ((item_mask>>(slot+2u))&1u){
                            int16_t id=(int16_t)rd_u16le(p,len,&off);
                            uint8_t pf=rd_u8(p,len,&off);
                            uint16_t st=rd_u16le(p,len,&off);
                            if (!first) buf_u8(b,',');
                            first=0;
                            serialize_item_stack_json(b,id,pf,st);
                            }
                        }
                    }
                buf_cstr(b,"]");
                break;
                }
            case 7:/* TeleportationPylon *//* No extra data */break;
            case 9:/* KiteAnchor */case 10:/* CritterAnchor */{
                int16_t item_type=(int16_t)rd_u16le(p,len,&off);
                buf_cstr(b,",\"itemType\":");
                json_i32(b,item_type);
                break;
                }
            }
        buf_cstr(b,"\n}\n");
        }
    buf_u8(b,']');
    }
/* --- weighted_pressure_plates section --- */void serialize_weighted_pressure_plates_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,6); uint32_t off=section<0?0u:w->starts[section];
    uint32_t end=section<0?0u:w->ends[section];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    uint32_t count=terra_reader_has(off,4u,end)?rd_u32le(p,len,&off):0u;
    for (int32_t i=0;
    i<count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        uint32_t x=rd_i32le(p,len,&off);
        uint32_t y=rd_i32le(p,len,&off);
        buf_cstr(b," { \"x\":");
        json_i32(b,x);
        buf_cstr(b,",\"y\":");
        json_i32(b,y);
        buf_cstr(b,"\n}\n");
        }
    buf_u8(b,']');
    }
/* --- town_manager section --- */void serialize_town_manager_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,7); uint32_t off=section<0?0u:w->starts[section];
    uint32_t end=section<0?0u:w->ends[section];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    uint32_t count=terra_reader_has(off,4u,end)?rd_u32le(p,len,&off):0u;
    for (int32_t i=0;
    i<count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        uint32_t npc_id=rd_i32le(p,len,&off);
        uint32_t x=rd_i32le(p,len,&off);
        uint32_t y=rd_i32le(p,len,&off);
        buf_cstr(b," { \"npcType\":");
        json_i32(b,npc_id);
        buf_cstr(b,",\"x\":");
        json_i32(b,x);
        buf_cstr(b,",\"y\":");
        json_i32(b,y);
        buf_cstr(b,"\n}\n");
        }
    buf_u8(b,']');
    }
/* --- bestiary section --- *//* * Binary format: * u32 killCount, [7bit-string name, u32 count...] * u32 sightingCount, [7bit-string name...] * u32 chatCount, [7bit-string name...] */void serialize_bestiary_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,8);
    uint32_t off=w->section_overrides[8].active?0u:(section<0?0u:w->starts[section]);
    uint32_t end=w->section_overrides[8].active?w->section_overrides[8].len:(section<0?0u:w->ends[section]);
    uint8_t *p=w->section_overrides[8].active?w->section_overrides[8].data:w->file;
    uint32_t len=w->section_overrides[8].active?w->section_overrides[8].len:w->file_len;
    buf_cstr(b," { \"kills\":[");
    uint32_t kill_count=0;
    if (terra_reader_has(off,4u,end))kill_count=rd_u32le(p,len,&off);
    for (uint32_t i=0;
    i<kill_count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        char name[256];
        rd_string_copy(p,len,&off,name,256);
        uint32_t count=rd_u32le(p,len,&off);
        buf_cstr(b," { \"persistentNpcId\":");
        json_string(b,name);
        buf_cstr(b,",\"killCount\":");
        json_u32(b,count);
        buf_cstr(b,"\n}\n");
        }
    buf_cstr(b,"],\"sightings\":[");
    uint32_t sighting_count=0;
    if (terra_reader_has(off,4u,end))sighting_count=rd_u32le(p,len,&off);
    for (uint32_t i=0;
    i<sighting_count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        char name[256];
        rd_string_copy(p,len,&off,name,256);
        buf_cstr(b," { \"persistentNpcId\":");
        json_string(b,name);
        buf_cstr(b,"\n}\n");
        }
    buf_cstr(b,"],\"chats\":[");
    uint32_t chat_count=0;
    if (terra_reader_has(off,4u,end))chat_count=rd_u32le(p,len,&off);
    for (uint32_t i=0;
    i<chat_count&&off<end;
    i++){
        if (i)buf_u8(b,',');
        char name[256];
        rd_string_copy(p,len,&off,name,256);
        buf_cstr(b," { \"persistentNpcId\":");
        json_string(b,name);
        buf_cstr(b,"\n}\n");
        }
    buf_cstr(b,"]\n}\n");
    }
/* --- creative_powers section (Journey mode) --- *//* * Binary format: * For each power: u16 powerId, then type-specific data * Read until section end. * * Known power IDs: * 0 = TimeSetFrozen (bool/u8) * 8 = TimeSetSpeed (f32) * 9 = RainSetFrozen (bool/u8) * 10 = WindSetFrozen (bool/u8) * 12 = SetDifficulty (f32) * 13 = BiomeSpreadSetFrozen (bool/u8) */int serialize_creative_powers_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,9);
    uint32_t off=section<0?0u:w->starts[section];
    uint32_t end=section<0?0u:w->ends[section];
    const uint8_t *p=w->file;
    int first=1;
    buf_u8(b,'[');
    if (end>w->file_len) goto truncated;
    if (off==end) { buf_u8(b,']'); return 1; }
    while (terra_reader_has(off,1u,end)) {
        if (!rd_u8(p,end,&off)) { buf_u8(b,']'); return 1; }
        if (!terra_reader_has(off,2u,end)) goto truncated;
        uint16_t power_id=rd_u16le(p,end,&off);
        if (!first) buf_u8(b,',');
        first=0;
        buf_cstr(b," { \"powerId\":"); json_u32(b,power_id);
        switch (power_id) {
            case 0: case 9: case 10: case 13:
                if (!terra_reader_has(off,1u,end)) goto truncated;
                buf_cstr(b,",\"enabled\":"); json_bool(b,rd_u8(p,end,&off));
                break;
            case 8: case 12:
                if (!terra_reader_has(off,4u,end)) goto truncated;
                buf_cstr(b,",\"sliderValue\":"); json_float(b,rd_f32le(p,end,&off));
                break;
            default:
                tx_set_error("TERRAX_UNSUPPORTED_CREATIVE_POWER","unknown creative power payload");
                return 0;
        }
        buf_cstr(b,"\n}\n");
    }
truncated:
    tx_set_error("TERRAX_TRUNCATED_CREATIVE_POWERS","creative power payload or terminator is truncated");
    return 0;
    }
/* --- footer section --- */void serialize_footer_json(TxWorld *w,TxBuf *b){
    int section=tx_actual_section_index(w,10);
    uint8_t overridden=section>=0 && w->section_overrides[section].active;
    uint32_t off=overridden?0u:(section<0?0u:w->starts[section]);
    uint32_t end=overridden?w->section_overrides[section].len:(section<0?0u:w->ends[section]);
    uint8_t *p=overridden?w->section_overrides[section].data:w->file;
    uint32_t len=overridden?end:w->file_len;
    uint8_t valid=0; char name[TX_MAX_NAME]={0}; int32_t id=0;
    if (p && end<=len && terra_reader_has(off,1u,end) && rd_u8(p,end,&off)) {
        int ok=0; uint32_t name_start=off;
        uint32_t size=rd_7bit(p,end,&off,&ok);
        if (ok && terra_reader_has(off,size,end)) {
            off+=size;
            if (terra_reader_has(off,4u,end)) {
                id=rd_i32le(p,end,&off);
                rd_string_copy(p,end,&name_start,name,TX_MAX_NAME);
                valid=1;
            }
        }
    }
    buf_cstr(b," { \"valid\":"); json_bool(b,valid);
    buf_cstr(b,",\"worldName\":"); json_string(b,valid?name:"");
    buf_cstr(b,",\"worldId\":"); json_i32(b,valid?id:0);
    buf_cstr(b,"\n}\n");
    }
/* ==================================================================== * Unified section serializer -- dispatch by section index * ==================================================================== */int serialize_section_json(TxWorld *w,int idx,TxBuf *b){
    if (w->legacy_wld && idx>=2 && idx<=10) return serialize_legacy_section_json(w,idx,b);
    /* API section names use the latest layout; old worlds omit sections. */
    if (idx>=2 && idx<=10) {
        int actual=tx_actual_section_index(w,idx);
        if (actual<0) {
            /* Public section reads remain shape-stable on old worlds. */
            if (idx==8) { buf_cstr(b,"{\"kills\":[],\"sightings\":[],\"chats\":[]}"); return 1; }
            buf_u8(b,'['); buf_u8(b,']'); return 1;
        }
    }
    switch (idx){
        case -2:serialize_format_json(w,b);
        return 1;
        case 0:serialize_header_json(w,b);
        return 1;
        case 2:serialize_chests_json(w,b);
        return 1;
        case 3:serialize_signs_json(w,b);
        return 1;
        case 4:serialize_npcs_json(w,b);
        return 1;
        case 5:serialize_tile_entities_json(w,b);
        return 1;
        case 6:serialize_weighted_pressure_plates_json(w,b);
        return 1;
        case 7:serialize_town_manager_json(w,b);
        return 1;
        case 8:serialize_bestiary_json(w,b);
        return 1;
        case 9:return serialize_creative_powers_json(w,b);
        case 10:serialize_footer_json(w,b);
        return 1;
        default:/* Tiles section (idx=1) or unknown: return section range info */if (idx>=0&&(uint32_t)idx<w->pointer_count){
            buf_cstr(b," { \"name\":");
            json_string(b,section_name_by_index((uint32_t)idx));
            buf_cstr(b,",\"start\":");
            json_u32(b,w->starts[idx]);
            buf_cstr(b,",\"end\":");
            json_u32(b,w->ends[idx]);
            buf_cstr(b,",\"byteLength\":");
            json_u32(b,w->ends[idx]-w->starts[idx]);
            buf_cstr(b,"\n}\n");
            }
        else{
            buf_cstr(b,"null");
            }
        ;
        }
    return b->ok ? 1 : 0;
    }
/* ==================================================================== * Same-tile comparison helper * ==================================================================== */int same_tile(const TxTile *a,const TxTile *b){
    return a->active==b->active&&a->type==b->type&&a->frame_x==b->frame_x&&a->frame_y==b->frame_y&&a->wall==b->wall&&a->liquid_amount==b->liquid_amount&&a->liquid_type==b->liquid_type&&a->brick_style==b->brick_style&&a->tile_color==b->tile_color&&a->wall_color==b->wall_color&&a->wire_red==b->wire_red&&a->wire_blue==b->wire_blue&&a->wire_green==b->wire_green&&a->wire_yellow==b->wire_yellow&&a->actuator==b->actuator&&a->inactive==b->inactive&&a->invisible_block==b->invisible_block&&a->invisible_wall==b->invisible_wall&&a->fullbright_block==b->fullbright_block&&a->fullbright_wall==b->fullbright_wall;
    }
