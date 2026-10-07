#ifndef TERRA_CIRCUIT_OBJECTS_H
#define TERRA_CIRCUIT_OBJECTS_H
/* Internal COB1 codec. Raw strings/items never pass through display JSON.
* WorldFile and all eleven TileEntitiesManager registrations are pinned to
* 8255d34616c780af12079425ac92a0a7aed87d71 (WLD 326). */
#include "terra_types.h"
#include <string.h>
#include <stdlib.h>
#define CO_MAGIC 0x31424f43u
#define CO_LIMIT (4u*1024u*1024u)
#define CO_COUNT 32768u
typedef struct CoItem {
    uint32_t section,kind,x,y,tile,length,offset,seen;
    const uint8_t* payload;
} CoItem;
typedef struct CoCursor {
    const uint8_t* bytes;
    uint32_t length,offset,count,index,section;
} CoCursor;
static uint32_t co_u16(const uint8_t* p) {
    return p[0]|((uint32_t)p[1]<<8);
}
static uint32_t co_u32(const uint8_t* p) {
    return co_u16(p)|(co_u16(p+2)<<16);
}
static void co_put(uint8_t* p,uint32_t v) {
    for(uint32_t i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));
}
static int co_take(uint32_t* p,uint32_t n,uint32_t end) {
    if(*p>end||n>end-*p)return 0;
    *p+=n;
    return 1;
}
static int co_string(const uint8_t* p,uint32_t end,uint32_t* off) {
    uint32_t n=0;
    for(uint32_t shift=0;shift<35;shift+=7) {
        if(*off>=end)return 0;
        uint32_t b=p[(*off)++];
        if(shift==28&&(b&240))return 0;
        n|=(b&127)<<shift;
        if(!(b&128))return co_take(off,n,end);
    }
    return 0;
}
static uint32_t co_bits(uint32_t n) {
    uint32_t k=0;
    for(;n;n>>=1)k+=n&1;
    return k;
}
static uint32_t co_entity_tile(uint32_t k) {
    static const uint16_t ids[]= {
        378,395,423,470,471,475,520,597,698,723,724
    };
    return k<11?ids[k]:0;
}
static uint32_t co_section(uint32_t t) {
    switch(t) {
        case 21:case 88:case 467:return 2;
        case 55:case 85:case 425:case 573:return 3;
        default:
            for(uint32_t k=0;k<11;k++)if(co_entity_tile(k)==t)return 5;
            return 0;
    }
}
static uint32_t co_shape(uint32_t t) {
    if(co_section(t)==2)return t==88?3u|(2u<<8):2u|(2u<<8);
    if(co_section(t)==3)return 2u|(2u<<8);
    static const uint8_t widths[]= {
        2,2,1,2,3,3,1,3,1,1,1
    };
    static const uint8_t heights[]= {
        3,2,1,3,3,4,1,4,2,1,1
    };
    for(uint32_t k=0;k<11;k++)if(co_entity_tile(k)==t)return widths[k]|((uint32_t)heights[k]<<8);
    return 0;
}
static int co_payload(uint32_t section,uint32_t kind,const uint8_t* p,uint32_t end,uint32_t* off) {
    if(section==3)return !kind&&co_string(p,end,off);
    if(section==2) {
        if(kind||!co_string(p,end,off)||!co_take(off,4,end))return 0;
        uint32_t slots=co_u32(p+*off-4);
        if(slots>(end-*off)/2)return 0;
        for(uint32_t i=0;i<slots;i++) {
            if(!co_take(off,2,end))return 0;
            uint32_t stack=co_u16(p+*off-2);
            if(stack>INT16_MAX)return 0;
            if(stack&&!co_take(off,5,end))return 0;
        }
        return 1;
    }
    if(section!=5||kind>10)return 0;
    uint32_t n=0;
    switch(kind) {
        case 0:case 2:case 9:case 10:
            n=2;
            break;
        case 1:case 4:case 6:case 8:
            n=5;
            break;
        case 7:
            break; /* A pylon has no extra saved data. */
        case 3:
            if(!co_take(off,4,end)||p[*off-1]&~7u)return 0;
            n=5u*(co_bits(p[*off-4])+co_bits(p[*off-3])+co_bits(p[*off-1]));
            break;
        case 5:
            if(!co_take(off,1,end)||p[*off-1]&~15u)return 0;
            n=5u*co_bits(p[*off-1]);
            break;
    }
    uint32_t start=*off;
    if(!co_take(off,n,end))return 0;
    if(kind==2&&(p[start]>7||p[start+1]>1))return 0;
    return 1;
}
static int co_cursor(TxWorld* w,uint32_t section,CoCursor* c) {
    memset(c,0,sizeof(*c));
    c->section=section;
    if(w->version!=326||section>=w->pointer_count)return 0;
    if(w->section_overrides[section].active) {
        c->bytes=w->section_overrides[section].data;
        c->length=w->section_overrides[section].len;
    } else {
        c->bytes=w->file+w->starts[section];
        c->length=w->ends[section]-w->starts[section];
    }
    c->offset=section==5?4:2;
    if(c->length<c->offset)return 0;
    c->count=section==5?co_u32(c->bytes):co_u16(c->bytes);
    uint32_t minimum_record=section==2?13:9;
    if(c->count>(c->length-c->offset)/minimum_record ||
       (section==2&&c->count>8000) ||
       (section==3&&c->count>32000))return 0;
    return 1;
}
static int co_next(CoCursor* c,CoItem* item) {
    if(c->index==c->count)return c->offset==c->length?2:0;
    memset(item,0,sizeof(*item));
    item->section=c->section;
    const uint8_t* p=c->bytes;
    uint32_t n=c->length,o=c->offset,body;
    if(c->section==3) {
        body=o;
        if(!co_string(p,n,&o))return 0;
        item->payload=p+body;
        item->length=o-body;
        if(!co_take(&o,8,n))return 0;
        item->x=co_u32(p+o-8);
        item->y=co_u32(p+o-4);
    } else {
        if(!co_take(&o,c->section==5?9:8,n))return 0;
        if(c->section==5) {
            item->kind=p[c->offset];
            item->tile=co_entity_tile(item->kind);
            item->x=co_u16(p+c->offset+5);
            item->y=co_u16(p+c->offset+7);
        } else {
            item->x=co_u32(p+c->offset);
            item->y=co_u32(p+c->offset+4);
        }
        body=o;
        if(!co_payload(c->section,item->kind,p,n,&o))return 0;
        item->payload=p+body;
        item->length=o-body;
    }
    item->offset=c->offset;
    c->offset=o;
    c->index++;
    return 1;
}
static int co_item_compare(const void* a,const void* b) {
    const CoItem* x=a,*y=b;
    if(x->x!=y->x)return x->x<y->x?-1:1;
    return x->y<y->y?-1:x->y>y->y;
}
static CoItem* co_find(CoItem* a,uint32_t count,uint32_t x,uint32_t y) {
    uint32_t lo=0,hi=count;
    while(lo<hi) {
        uint32_t m=lo+(hi-lo)/2;
        if(a[m].x<x||(a[m].x==x&&a[m].y<y))lo=m+1;
        else hi=m;
    }
    return lo<count&&a[lo].x==x&&a[lo].y==y?a+lo:NULL;
}
static int co_root_frame(uint32_t tile,int16_t fx,int16_t fy) {
    if(fx<0||fy<0)return 0;
    if(tile==423)return fx%18==0&&fy%18==0&&fy<=108;
    if(tile==724)return fx%18==0&&fy%18==0;
    uint32_t shape=co_shape(tile);
    return shape&&fy==0&&(uint32_t)fx%((shape&255)*18)==0;
}
#endif
