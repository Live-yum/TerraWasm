/* Future reads use the pinned 326 schema, never a substituted release word.
 * Reference: TerrariaDecompiledSource 8255d346, WorldFile.LoadWorld_Version2,
 * FileMetadata.Read, TileEntity.Read and CreativePowerManager.LoadFromWorld.
 * Successful structural validation is not permission to rewrite that schema.
 */
#include "terra_types.h"
#include "terra_reader.h"
#include <string.h>
#include <limits.h>

extern void tx_set_error(const char*, const char*);
extern int read_tile_at(TxWorld*, uint32_t*, uint32_t, TxTile*);

int tx_world_is_future(const TxWorld* w) {
    return w && (w->original_version > TX_CURRENT_KNOWN_VERSION ||
                 w->version > TX_CURRENT_KNOWN_VERSION);
}
int tx_world_require_writable(const TxWorld* w) {
    if (!w) return 0;
    if (!tx_world_is_future(w)) return 1;
    tx_set_error("TERRAX_FUTURE_VERSION_READ_ONLY",
        "future-version world supports reading and original-byte export only");
    return 0;
}

typedef struct { const uint8_t* p; uint32_t off, end; } FutureReader;
static int take(FutureReader* r, uint32_t n) { return terra_reader_take(&r->off,n,r->end); }
static int number(FutureReader* r, uint32_t n, uint32_t* out) {
    uint32_t off=r->off, v=0;
    if (!take(r,n)) return 0;
    for (uint32_t i=0;i<n;i++) v|=(uint32_t)r->p[off+i]<<(i*8u);
    *out=v; return 1;
}
static int count(FutureReader* r, uint32_t n, uint32_t minimum, uint32_t* out) {
    return number(r,n,out) && *out<=(n==2u?INT16_MAX:INT32_MAX) &&
        (!minimum || *out<=(r->end-r->off)/minimum);
}
static int string(FutureReader* r, uint32_t* start, uint32_t* length) {
    uint32_t n=0,b;
    for (uint32_t i=0;i<5;i++) {
        if (!number(r,1,&b) || (i==4 && b>7u)) return 0;
        n|=(b&127u)<<(i*7);
        if (!(b&128u)) {
            if (start) *start=r->off;
            if (length) *length=n;
            return take(r,n);
        }
    }
    return 0;
}
static FutureReader section(TxWorld* w, uint32_t i) {
    return (FutureReader){w->file,w->starts[i],w->ends[i]};
}
static uint32_t bits(uint32_t n) { uint32_t c=0; for(;n;n>>=1)c+=n&1u; return c; }
static int bad(const char* message) { tx_set_error("TERRAX_FUTURE_LAYOUT_ERROR",message); return 0; }

int tx_validate_future_sections(TxWorld* w) {
    if (!tx_world_is_future(w)) return 1;
    if (w->pointer_count!=11u) return bad("attempted WLD layout requires 11 sections");
    /* Metadata is bounded separately from tiles, as in the streaming reader. */
    if ((uint64_t)w->file_len-(w->ends[1]-w->starts[1])>16u*1024u*1024u)
        return bad("world metadata exceeds the bounded reader budget");
    FutureReader r=section(w,2); uint32_t n,v,slots;
    if (!count(&r,2,13,&n)) return bad("invalid chest count");
    for(uint32_t i=0;i<n;i++) {
        if(!take(&r,8)||!string(&r,0,0)||!count(&r,4,2,&slots)||slots>504u)return bad("invalid chest record");
        for(uint32_t j=0;j<slots;j++)if(!number(&r,2,&v)||(v&&!take(&r,5)))return bad("truncated chest item");
    }
    if(r.off!=r.end)return bad("chest section was not consumed exactly");
    r=section(w,3);
    if(!count(&r,2,9,&n))return bad("invalid sign count");
    for(uint32_t i=0;i<n;i++)if(!string(&r,0,0)||!take(&r,8))return bad("truncated sign");
    if(r.off!=r.end)return bad("sign section was not consumed exactly");
    r=section(w,4);
    if(!count(&r,4,4,&n)||!take(&r,n*4u))return bad("invalid shimmered NPC list");
    for(uint32_t list=0;list<2;list++)for(;;) {
        if(!number(&r,1,&v))return bad("missing NPC terminator");
        if(!v)break;
        if(!take(&r,4))return bad("truncated NPC type");
        if(list){if(!take(&r,8))return bad("truncated persistent NPC");}
        else if(!string(&r,0,0)||!take(&r,17)||!number(&r,1,&v)||
                ((v&1u)&&!take(&r,4))||!take(&r,1))return bad("truncated town NPC");
    }
    if(r.off!=r.end)return bad("NPC section was not consumed exactly");
    r=section(w,5);
    if(!count(&r,4,9,&n))return bad("invalid tile entity count");
    for(uint32_t i=0;i<n;i++) {
        uint32_t type,a,b,c=0;
        if(!number(&r,1,&type)||!take(&r,8))return bad("truncated tile entity header");
        switch(type) {
        case 0: case 2: case 9: case 10: if(!take(&r,2))return bad("truncated tile entity"); break;
        case 1: case 4: case 6: case 8: if(!take(&r,5))return bad("truncated tile entity item"); break;
        case 3:
            if(!number(&r,1,&a)||!number(&r,1,&b)||!take(&r,1)||!number(&r,1,&c)||c>7u||
               !take(&r,(bits(a)+bits(b)+bits(c))*5u))return bad("invalid display doll");
            break;
        case 5:
            if(!number(&r,1,&a)||a>15u||!take(&r,bits(a)*5u))return bad("invalid hat rack");
            break;
        case 7: break;
        default: return bad("unknown tile entity payload in attempted layout");
        }
    }
    if(r.off!=r.end)return bad("tile entity section was not consumed exactly");
    for(uint32_t i=6;i<=7;i++) {
        r=section(w,i);uint32_t stride=i==6?8u:12u;
        if(!count(&r,4,stride,&n)||!take(&r,n*stride)||r.off!=r.end)return bad("invalid pressure plate or town section");
    }
    r=section(w,8);
    for(uint32_t list=0;list<3;list++) {
        if(!count(&r,4,list?1u:5u,&n))return bad("invalid bestiary count");
        for(uint32_t i=0;i<n;i++)if(!string(&r,0,0)||(!list&&!take(&r,4)))return bad("truncated bestiary entry");
    }
    if(r.off!=r.end)return bad("bestiary section was not consumed exactly");
    r=section(w,9);
    for(;;) {
        if(!number(&r,1,&v))return bad("missing creative power terminator");
        if(!v)break;
        if(!number(&r,2,&v))return bad("truncated creative power id");
        switch(v) {
        case 0: case 9: case 10: case 13: if(!take(&r,1))return bad("truncated creative power"); break;
        case 8: case 12: if(!take(&r,4))return bad("truncated creative slider"); break;
        default:return bad("unknown creative power payload in attempted layout");
        }
    }
    if(r.off!=r.end)return bad("creative section was not consumed exactly");
    r=section(w,10);FutureReader h=section(w,0);uint32_t hs,hn,fs,fn;
    if(!number(&r,1,&v)||!v||!string(&r,&fs,&fn)||!string(&h,&hs,&hn)||fn!=hn||
       memcmp(w->file+fs,w->file+hs,fn)||!number(&r,4,&v)||v!=(uint32_t)w->worldId||r.off!=r.end)
        return bad("footer does not exactly match the world name and id");
    return 1;
}

int tx_validate_future_tiles(TxWorld* w) {
    if (!tx_world_is_future(w)) return 1;
    uint32_t off=w->starts[1],end=w->ends[1];
    for(uint32_t x=0;x<(uint32_t)w->maxTilesX;x++)for(uint32_t y=0;y<(uint32_t)w->maxTilesY;) {
        TxTile t;
        if(!read_tile_at(w,&off,end,&t)||(uint32_t)t.same+1u>(uint32_t)w->maxTilesY-y)
            return bad("tile record or RLE exceeds the attempted world layout");
        y+=(uint32_t)t.same+1u;
    }
    if(off!=end)return bad("tile section was not consumed exactly");
    return 1;
}
