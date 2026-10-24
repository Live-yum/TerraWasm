/* Complete self-contained encoded entries, tested against raw9 pair oracle. */
#define COLD_CONTRACT_MAIN cold_existing_contract_main
#include "circuit_geometry_cold_contract.c"
#undef COLD_CONTRACT_MAIN
static uint16_t pair_value(uint32_t index){return (uint16_t)((index*73u+256u)&511u);}
static void prepare_pairs(Fixture* f,uint32_t column,uint32_t k,uint32_t forced){
    CxOrderGeometry* g=f->owner.geometry;g->slot=0;g->target_column=column;g->cursor=f->columns[column+1u];g->cold_runs=k==1?1:f->height;g->cold_count=k;
    memset(g->cold_scratch->seen,0,sizeof(g->cold_scratch->seen));
    for(uint32_t i=0;i<k;i++){uint16_t v=pair_value(i);g->cold_scratch->map[v]=(uint16_t)i;g->cold_scratch->seen[v>>6]|=UINT64_C(1)<<(v&63u);}
    for(uint32_t y=0;y<f->height;y++){uint16_t v=pair_value(y%k);g->cells[y]=(uint8_t)v;g->compiler_axis[y]=(uint8_t)(v>>8);}
    uint64_t selected=cold_select(&f->owner,g);assert(selected<=CX_GEOMETRY_COLD_BYTES);
    if(forced==3u){g->cold_format=3;g->cold_bits=0;while((1u<<g->cold_bits)<k)++g->cold_bits;selected=k*2u+((uint64_t)f->height*g->cold_bits+7u)/8u;}
    else if(forced==2u){g->cold_format=2;g->cold_bits=9;selected=((uint64_t)f->height*9u+7u)/8u;}
    g->cold_length=(uint32_t)selected;g->cold_offset=g->cold_head;if(g->cold_offset>CX_GEOMETRY_COLD_BYTES-g->cold_length)g->cold_offset=0;
    g->cold_scan=0;g->cold_phase=1;g->active=1;g->valid[0]=0;
}
static void encode_ready(Fixture* f){CxOrderGeometry* g=f->owner.geometry;
    for(uint32_t n=0;;n++){assert(n<100000);uint32_t work=1;int s=cold_step(&f->owner,&work);assert(s>=0);if(!s)break;assert(!entry(g,g->target_column));}
    g->active=0;clear_hot(g);
}
static void check_pairs(Fixture* f,uint32_t column,uint32_t k){for(uint32_t y=0;y<f->height;y++){
    uint8_t word,route;uint16_t expected=pair_value(y%k);assert(cx_geometry_get_connectivity(&f->owner,column,y,&word,&route)==0);
    assert(word==(uint8_t)expected&&route==((expected&256u)?1u:((expected>>4)&3u)));
}}
static void formats_and_bounds(void){const uint32_t sizes[]={1,2,3,4,5,8,16,17,32,33,64,128,129,256,257,512};
    Fixture f;init(&f,4,1051,0);assert(cx_geometry_create(&f.owner)==0);CxOrderGeometry* g=f.owner.geometry;
    for(uint32_t n=0;n<sizeof(sizes)/sizeof(*sizes);n++)for(uint32_t format=2;format<=3;format++){
        uint32_t k=sizes[n];memset(g->cold_entries,0,CX_GEOMETRY_COLD_ENTRIES*sizeof(CxColdColumn));g->cold_head=0;
        prepare_pairs(&f,0,k,format);uint32_t bytes=g->cold_length;
        /* Exact arena ending tests the final byte and no cross-payload reads. */
        g->cold_offset=CX_GEOMETRY_COLD_BYTES-bytes;encode_ready(&f);CxColdColumn* e=entry(g,0);assert(e&&e->offset+e->length==CX_GEOMETRY_COLD_BYTES&&e->format==format);
        check_pairs(&f,0,k);uint32_t saved=e->length;e->length--;uint8_t word;assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);e->length=saved;
        uint32_t bits=format==2?9:0;if(format==3)while((1u<<bits)<k)++bits;uint32_t tail=f.height*bits%8u;
        if(tail){uint8_t* last=g->cold_bytes+e->offset+e->length-1;assert(!(*last>>tail));uint8_t keep=*last;*last|=128u;assert(cx_geometry_get(&f.owner,0,f.height-1,&word)==TCW_FORMAT);*last=keep;}
        if(format==3){uint8_t* table=g->cold_bytes+e->offset;uint16_t keep=cold_u16(table);cold_put16(table,512);assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);cold_put16(table,keep);
            if(k>1&&(k&(k-1u))){uint8_t* at=table+k*2u;uint8_t a=at[0],b=at[1];at[0]=255;at[1]|=1u;assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);at[0]=a;at[1]=b;}
            uint32_t count=e->runs;e->runs=513;assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);e->runs=count;
        }
        /* Destroy/rebuild mutable encoder scratch for a different column.
         * Published lookup must remain wholly self-contained even while pending. */
        prepare_pairs(&f,1,2,3);check_pairs(&f,0,k);cx_geometry_cancel(&f.owner);check_pairs(&f,0,k);
    }
    /* Selection uses actual payload lengths: palette constant and raw9 high K. */
    prepare_pairs(&f,0,1,0);assert(g->cold_format==3&&g->cold_length==2);cx_geometry_cancel(&f.owner);
    prepare_pairs(&f,0,512,0);assert(g->cold_format==2&&g->cold_length==((uint64_t)f.height*9u+7u)/8u);cx_geometry_cancel(&f.owner);finish(&f);
}
static void palette_cancel(void){Fixture f;init(&f,4,1051,0);assert(cx_geometry_create(&f.owner)==0);CxOrderGeometry* g=f.owner.geometry;
    for(uint32_t phase=1;phase<=3;phase++){
        memset(g->cold_entries,0,CX_GEOMETRY_COLD_ENTRIES*sizeof(CxColdColumn));g->cold_head=0;
        prepare_pairs(&f,0,17,3);encode_ready(&f);assert(entry(g,0));g->cold_head=0;prepare_pairs(&f,1,33,3);
        for(uint32_t n=0;;n++){assert(n<100000);uint32_t work=1;assert(cold_step(&f.owner,&work)==CX_GEOMETRY_MORE);
            if(g->cold_phase==phase&&(phase==1?g->cold_scan>8:phase==3?g->cold_palette_scan>8:g->cold_row>8))break;
        }
        assert(!entry(g,0)&&!entry(g,1));cx_geometry_cancel(&f.owner);assert(!g->active&&!g->cold_phase&&!entry(g,1));
        prepare_pairs(&f,1,33,3);encode_ready(&f);check_pairs(&f,1,33);
    }
    finish(&f);
}
int main(void){uint32_t baseline=tx_native_heap_used();formats_and_bounds();palette_cancel();cold_existing_contract_main();assert(tx_native_heap_used()==baseline);puts("palette/raw9 geometry contract passed");return 0;}
