/* * terra_wld.c -- WLD binary parser for TerraWasm. * * Pure C implementation that parses raw .wld bytes int o TxWorld metadata, * streams tiles on-demand, and provides JSON serializers/deserializers * for all 11 world sections matching the TerraX V2 API. * * Uses bump allocator from terra_mem.c (tx_malloc, tx_alloc, TxBuf). * Uses json_string, json_u32, etc. from terra_json.c for JSON output. */

#include "terra_types.h"
#include "terra_reader.h"
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
    int ok=0;
    uint32_t slen=rd_7bit(p,len,off,&ok);
    if (!ok||!terra_reader_has(*off,slen,len)){
        if (cap)out[0]=0;
        *off=len;
        ;
        }
    uint32_t n=slen;
    if (n+1u>cap)n=cap?cap-1u:0u;
    for (uint32_t i=0;
    i<n;
    i++)out[i]=(char)p[*off+i];
    if (cap)out[n]=0;
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
    int ok=0;
    uint32_t slen=rd_7bit(p,len,off,&ok);
    if (!ok||!terra_reader_has(*off,slen,len)){
        *off=len;
        ;
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
    if (w->version<88u||w->version>400u){
        tx_set_error("TERRAX_UNSUPPORTED_VERSION","unsupported .wld version");
        return 0;
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
    tx_record_header_bool(w,json_name,off,(uint32_t)((uint8_t*)&w->member-(uint8_t*)w)); \
    w->member=rd_u8(p,len,&off); \
    } while(0)

/* ==================================================================== * parse_header -- Extract header metadata from raw .wld bytes * ==================================================================== */int parse_header(TxWorld *w){
    uint32_t off=w->starts[0];
    uint32_t len=w->file_len;
    uint8_t *p=w->file;
    int ok;
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
        w->anglersOff=off;
        for (uint32_t i=0;
        i<w->anglerFinishedSize;
        i++)rd_skip_string_value(p,len,&off);
        }
    if (w->version>=99u)TX_RD_HEADER_BOOL(savedAngler,"savedAngler");
    if (w->version>=101u)w->anglerQuest=rd_u32le(p,len,&off);
    if (w->version>=104u)TX_RD_HEADER_BOOL(savedStylist,"savedStylist");
    if (w->version>=140u)TX_RD_HEADER_BOOL(savedTaxCollector,"savedTaxCollector");
    if (w->version>=201u)TX_RD_HEADER_BOOL(savedGolfer,"savedGolfer");
    if (w->version>=107u)w->invasionSizeStart=rd_u32le(p,len,&off);
    if (w->version>=108u)w->cultistDelay=rd_u32le(p,len,&off);
    /* Kill counts (>=109) */if (w->version>=109u){
        w->numMobs=rd_u16le(p,len,&off);
        w->mobsOff=off;
        if (!terra_reader_take_count(&off,w->numMobs,4u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","mob data exceeds section bounds");
            return 0;
        }
        }
    /* Banners */if (w->version>=109u){
        w->numClaimableBanners=rd_u16le(p,len,&off);
        w->claimableBannersOff=off;
        if (!terra_reader_take_count(&off,w->numClaimableBanners,2u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","banner data exceeds section bounds");
            return 0;
        }
        }
    /* fastForwardTime (>=140, but gate starts at >=128) */if (w->version>=128u){
        if (w->version>=140u)TX_RD_HEADER_BOOL(fastForwardTime,"fastForwardTimeToDawn");
        if (w->version>=131u){
            TX_RD_HEADER_BOOL(downedFishron,"downedFishron");
            if (w->version>=140u){
                TX_RD_HEADER_BOOL(downedMartians,"downedMartians");
                TX_RD_HEADER_BOOL(downedLunaticCultist,"downedAncientCultist");
                TX_RD_HEADER_BOOL(downedMoonlord,"downedMoonlord");
                }
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
        w->partyCelebratingNPCsOff=off;
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
    if (w->version>=195u){
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
        w->treeTopVariationsOff=off;
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
        w->extradSpawnPointManagerOff=off;
        if (!terra_reader_take_count(&off,(uint32_t)w->numExtradSpawnPointManager,4u,len)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER","spawn point data exceeds section bounds");
            return 0;
        }
        }
    /* dualDungeonsSeed (>=304) */if (w->version>=304u)TX_RD_HEADER_BOOL(dualdungeonsSeed,"dualDungeonsSeed");
    /* legacySkip (>=299 && <313) */if (w->version>=299u&&w->version<313u)w->legacySkip=rd_u32le(p,len,&off);
    /* manifestJson (>=299) */if (w->version>=299u){
        w->maniFestOff=off;
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
/* ==================================================================== * read_tile_at -- Stream one tile from the binary * ==================================================================== */int read_tile_at(TxWorld *w,uint32_t *off,uint32_t end,TxTile *t){
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    if (*off>=end||*off>=len) return 0;
    memset(t,0,sizeof(TxTile));
    uint8_t f1=rd_u8(p,len,off);
    uint8_t f2=0,f3=0,f4=0;
    if (f1&1u)f2=rd_u8(p,len,off);
    if (f2&1u)f3=rd_u8(p,len,off);
    if (f3&1u)f4=rd_u8(p,len,off);
    t->active=(f1>>1)&1u;
    if (t->active){
        if (f1&32u)t->type=rd_u16le(p,len,off); else t->type=rd_u8(p,len,off);
        if (tile_important(w,t->type)){
            t->frame_x=(int16_t)rd_u16le(p,len,off);
            t->frame_y=(int16_t)rd_u16le(p,len,off);
            }
        }
    if (f3&8u)t->tile_color=rd_u8(p,len,off);
    if (f1&4u){ if (f3&64u)t->wall=rd_u16le(p,len,off); else t->wall=rd_u8(p,len,off); }
    if (f3&16u)t->wall_color=rd_u8(p,len,off);
    {
        uint8_t liq=(f1>>3)&3u;
        if (liq){
            t->liquid_amount=rd_u8(p,len,off);
            t->liquid_type=(f3&128u)?4u:liq;
            }
        }
    {
        uint8_t rle=(f1>>6)&3u;
        if (rle==1u)t->same=rd_u8(p,len,off);
        else if (rle)t->same=rd_u16le(p,len,off);
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
    if (t->tile_color)f3|=8u;
    if (t->wall_color)f3|=16u;
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
    if (f1&4u){ if (f3&64u)buf_u16le(b,t->wall); else buf_u8(b,(uint8_t)t->wall); }
    if (f3&16u)buf_u8(b,t->wall_color);
    if (((f1>>3)&3u)!=0u) buf_u8(b,t->liquid_amount);
    if (same){ if (same<=255u)buf_u8(b,(uint8_t)same); else buf_u16le(b,same); }
    }
/* ==================================================================== * Section name/index mapping * ==================================================================== */static const char *section_name_by_index(uint32_t index){
    static const char *names[11]={
        "header","tiles","chests","signs","npcs","tile_entities","weighted_pressure_plates","town_manager","bestiary","creative_powers","footer"}
    ;
    return index<11u?names[index]:"section";
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
        uint32_t aoff=w->anglersOff;
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
        uint32_t moff=w->mobsOff;
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
        uint32_t boff=w->claimableBannersOff;
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
        uint32_t poff=w->partyCelebratingNPCsOff;
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
        uint32_t toff=w->treeTopVariationsOff;
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
        uint32_t soff=w->extradSpawnPointManagerOff;
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
        uint32_t moff=w->maniFestOff;
        char manifest_buf[4096];
        uint32_t cap=w->maniFestLen<4095u?w->maniFestLen+1u:4096u;
        rd_string_copy(p,flen,&moff,manifest_buf,cap);
        json_string(b,manifest_buf);
        }
    buf_cstr(b,"\n}\n");
    }
/* --- chests section --- *//* * Binary format per chest (version >= 294): * i32 x, i32 y, 7bit-string name, i32 maxItems * For each item: u16 stack;  stack != 0: i32 itemType, u8 prefix */void serialize_chests_json(TxWorld *w,TxBuf *b){
    uint32_t off=w->section_overrides[2].active?0u:w->starts[2];
    uint32_t end=w->section_overrides[2].active?w->section_overrides[2].len:w->ends[2];
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
/* --- signs section --- */void serialize_signs_json(TxWorld *w,TxBuf *b){
    uint32_t off=w->starts[3];
    uint32_t end=w->ends[3];
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
        uint32_t sx=rd_i32le(p,len,&off);
        uint32_t sy=rd_i32le(p,len,&off);
        rd_string_copy(p,len,&off,text,1024);
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
    uint32_t off=w->starts[4];
    uint32_t end=w->ends[4];
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
        /* SpriteId */if (!terra_reader_has(scan_off,4u,end)) break;
        rd_skip(p,len,&scan_off,4);
        /* DisplayName (vString: varint length + bytes) */rd_skip_string_value(p,len,&scan_off);
        /* X, Y (f32 each) */rd_skip(p,len,&scan_off,8);
        /* IsHomeless (u8) */rd_skip(p,len,&scan_off,1);
        /* HomeX, HomeY (u32 each) */rd_skip(p,len,&scan_off,8);
        /* [v213+] hasVariation + optional */if (w->version>=213u){
            uint8_t has_var=rd_u8(p,len,&scan_off);
            if (has_var)rd_skip(p,len,&scan_off,4);
            }
        /* [v315+] homelessDespawn */if (w->version>=315u){
            rd_skip(p,len,&scan_off,1);
            }
        town_npc_count++;
        }
    /* Skip the town NPC terminator byte (already read as has_npc=0) *//* scan_off is now at the start of persistent NPCs *//* --- Count and locate persistent NPCs --- */uint32_t persistent_start=scan_off;
    uint32_t persistent_count=0;
    while (scan_off<end){
        uint8_t has_npc=rd_u8(p,len,&scan_off);
        if (!has_npc) break;
        /* SpriteId (i32) + X (f32) + Y (f32) = 12 bytes */if (!terra_reader_has(scan_off,12u,end)) break;
        rd_skip(p,len,&scan_off,12);
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
        /* SpriteId */int32_t npc_type=rd_i32le(p,len,&npc_off);
        /* DisplayName (vString) */char npc_name[256];
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
            if (has_variation)variation=rd_i32le(p,len,&npc_off);
            }
        /* [v315+] homelessDespawn */uint8_t homeless_despawn=0;
        if (w->version>=315u){
            homeless_despawn=rd_u8(p,len,&npc_off);
            }
        buf_cstr(b," { \"npcNetId\":");
        json_i32(b,npc_type);
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
        if (has_variation){
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
        uint32_t npc_type=rd_i32le(p,len,&mob_off);
        union{
            uint32_t u;
            float f;
            }
        mx,my;
        mx.u=rd_u32le(p,len,&mob_off);
        my.u=rd_u32le(p,len,&mob_off);
        buf_cstr(b," { \"npcNetId\":");
        json_i32(b,npc_type);
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
    uint32_t off=w->starts[5];
    uint32_t end=w->ends[5];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    uint32_t count=(int16_t)rd_u16le(p,len,&off);
    if (count<0)count=0;
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
                ;
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
                ;
                }
            case 2:{
                /* LogicSensor */uint8_t logic_check=rd_u8(p,len,&off);
                uint8_t on=rd_u8(p,len,&off);
                buf_cstr(b,",\"logicCheck\":");
                json_u32(b,logic_check);
                buf_cstr(b,",\"on\":");
                json_bool(b,on);
                ;
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
                if (w->version>=260u){
                    pose=rd_u8(p,len,&off);
                    }
                if (w->version>=262u){
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
                    if ((extra_mask>>1)&1u){
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
                ;
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
                ;
                }
            case 7:/* TeleportationPylon *//* No extra data */break;
            case 9:/* KiteAnchor */case 10:/* CritterAnchor */{
                int16_t item_type=(int16_t)rd_u16le(p,len,&off);
                buf_cstr(b,",\"itemType\":");
                json_i32(b,item_type);
                ;
                }
            }
        buf_cstr(b,"\n}\n");
        }
    buf_u8(b,']');
    }
/* --- weighted_pressure_plates section --- */void serialize_weighted_pressure_plates_json(TxWorld *w,TxBuf *b){
    uint32_t off=w->starts[6];
    uint32_t end=w->ends[6];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    uint32_t count=(int16_t)rd_u16le(p,len,&off);
    if (count<0)count=0;
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
    uint32_t off=w->starts[7];
    uint32_t end=w->ends[7];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    uint32_t count=(int16_t)rd_u16le(p,len,&off);
    if (count<0)count=0;
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
    uint32_t off=w->section_overrides[8].active?0u:w->starts[8];
    uint32_t end=w->section_overrides[8].active?w->section_overrides[8].len:w->ends[8];
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
/* --- creative_powers section (Journey mode) --- *//* * Binary format: * For each power: u16 powerId, then type-specific data * Read until section end. * * Known power IDs: * 0 = TimeSetFrozen (bool/u8) * 8 = TimeSetSpeed (f32) * 9 = RainSetFrozen (bool/u8) * 10 = WindSetFrozen (bool/u8) * 12 = SetDifficulty (f32) * 13 = BiomeSpreadSetFrozen (bool/u8) */void serialize_creative_powers_json(TxWorld *w,TxBuf *b){
    uint32_t off=w->starts[9];
    uint32_t end=w->ends[9];
    uint8_t *p=w->file;
    uint32_t len=w->file_len;
    buf_u8(b,'[');
    int first=1;
    if (terra_reader_has(off,2u,end)){
        uint16_t power_id=rd_u16le(p,len,&off);
        if (!first) buf_u8(b,',');
        first=0;
        buf_cstr(b," { \"powerId\":");
        json_u32(b,power_id);
        switch (power_id){
            case 0:/* TimeSetFrozen */case 9:/* RainSetFrozen */case 10:/* WindSetFrozen */case 13:/* BiomeSpreadSetFrozen */{
                uint8_t val=rd_u8(p,len,&off);
                buf_cstr(b,",\"enabled\":");
                json_bool(b,val);
                ;
                }
            case 8:/* TimeSetSpeed */case 12:/* SetDifficulty */{
                union{
                    uint32_t u;
                    float f;
                    }
                v;
                v.u=rd_u32le(p,len,&off);
                buf_cstr(b,",\"sliderValue\":");
                json_float(b,(double)v.f);
                ;
                }
            default:/* Unknown power; skip remaining bytes */off=end;
            ;
            }
        buf_cstr(b,"\n}\n");
        }
    buf_u8(b,']');
    }
/* --- footer section --- */void serialize_footer_json(TxWorld *w,TxBuf *b){
    buf_cstr(b," { \"valid\":true,\"worldName\":");
    json_string(b,w->worldName);
    buf_cstr(b,",\"worldId\":");
    json_i32(b,w->worldId);
    buf_cstr(b,"\n}\n");
    }
/* ==================================================================== * Unified section serializer -- dispatch by section index * ==================================================================== */int serialize_section_json(TxWorld *w,int idx,TxBuf *b){
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
        case 9:serialize_creative_powers_json(w,b);
        return 1;
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
