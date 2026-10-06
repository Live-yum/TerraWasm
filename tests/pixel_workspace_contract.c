#include "terra_pixel_workspace.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t tx_native_heap_used(void);
extern void txw_test_allocation_limit(uint32_t maximum);
extern void tx_reset_heap(void);
#define OK(call) assert((call) == TERRA_PIXEL_OK)
#define W 67u
#define H 69u
static uint32_t random_state = 0x82b4097du;
static uint32_t random_u32(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5; return random_state; }
static TerraPixelWorkspaceStats stats(uint32_t handle) { TerraPixelWorkspaceStats result; OK(terra_pixel_workspace_stats(handle, &result)); return result; }
static uint32_t create(uint32_t width, uint32_t height, uint32_t history, uint32_t palette) {
    uint32_t handle = 0; OK(terra_pixel_workspace_create(width, height, 32u * 1024u * 1024u, history, palette, &handle)); assert(handle); return handle;
}
static uint32_t color(uint32_t handle, uint32_t rgb) { uint32_t result; OK(terra_pixel_workspace_palette_add(handle, rgb, &result)); return result; }
static void cell(uint32_t handle, int32_t x, int32_t y, uint32_t index) { TerraPixelCell entry = {x,y,index}; OK(terra_pixel_workspace_cells(handle, &entry, 1)); }
static uint16_t get(uint32_t handle, int32_t x, int32_t y) { uint16_t value; OK(terra_pixel_workspace_read_rect(handle,x,y,1,1,&value,1)); return value; }
static void compare(uint32_t handle, const uint16_t* expected, uint32_t width, uint32_t height) {
    uint16_t row[TERRA_PIXEL_MAX_DIMENSION]; uint32_t used = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t first = 0; first < width; first += TERRA_PIXEL_QUERY_CELLS) {
            uint32_t count = width - first; if (count > TERRA_PIXEL_QUERY_CELLS) count = TERRA_PIXEL_QUERY_CELLS;
            OK(terra_pixel_workspace_read_rect(handle,(int32_t)first,(int32_t)y,count,1,row,count));
            for (uint32_t x = 0; x < count; ++x) { assert(row[x] == expected[y * width + first + x]); used += row[x] != 0; }
        }
    }
    assert(stats(handle).used_cells == used);
}
static uint32_t distance(uint32_t a, uint32_t b) {
    int r=(int)(a>>16)-(int)(b>>16),g=(int)((a>>8)&255)-(int)((b>>8)&255),blue=(int)(a&255)-(int)(b&255);
    return (uint32_t)(r*r+g*g+blue*blue);
}
static void basic_contract(void) {
    uint32_t baseline = tx_native_heap_used(), handle = create(65, 66, 1024u*1024u, 65536);
    TerraPixelWorkspaceStats original = stats(handle); assert(original.palette_count == 1 && !original.dirty && !original.active_blocks);
    uint32_t black=color(handle,0), manual=color(handle,0x010305), near=color(handle,0x010307);
    assert(black==1 && manual==2 && near==3 && color(handle,0x010305)==manual);
    uint32_t query=0x010306; uint16_t nearest=0; OK(terra_pixel_workspace_nearest(handle,&query,1,&nearest)); assert(nearest==manual);
    uint32_t values[4]; OK(terra_pixel_workspace_palette_read(handle,0,4,values)); assert(values[0]==0 && values[2]==0x010305);
    assert(terra_pixel_workspace_cells(handle,NULL,0)==TERRA_PIXEL_TRANSACTION);
    OK(terra_pixel_workspace_tx_begin(handle)); cell(handle,64,65,manual); cell(handle,0,0,black); cell(handle,1,0,near);
    assert(stats(handle).dirty); assert(stats(handle).transaction_open);
    uint16_t out[4096]; assert(terra_pixel_workspace_read_rect(handle,64,65,2,1,out,2)==TERRA_PIXEL_BOUNDS);
    assert(stats(handle).transaction_open); /* A read error cannot cancel a stroke. */
    OK(terra_pixel_workspace_tx_commit(handle)); assert(stats(handle).undo_count==1 && stats(handle).used_cells==3);
    TerraPixelBlockInfo info[4]; OK(terra_pixel_workspace_block_versions(handle,0,4,info)); assert(info[0].used==2 && info[3].used==1 && info[1].used==0);
    OK(terra_pixel_workspace_read_block(handle,1,1,out,4096)); assert(out[64]==manual && out[65]==0 && out[0]==0);
    uint8_t rgba[16384]; OK(terra_pixel_workspace_raster_block(handle,1,1,0,rgba,sizeof rgba));
    assert(rgba[64*4]==1 && rgba[64*4+1]==3 && rgba[64*4+2]==5 && rgba[64*4+3]==255 && rgba[65*4+3]==0);
    for (uint32_t level=0;level<=6;++level) {
        uint32_t side=64u>>level; OK(terra_pixel_workspace_raster_block(handle,0,0,level,rgba,sizeof rgba));
        for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x){uint32_t i=(y*side+x)*4u;uint16_t v=get(handle,(int32_t)(x<<level),(int32_t)(y<<level));assert(rgba[i+3]==(v?255:0));}
    }
    OK(terra_pixel_workspace_checkpoint(handle)); uint32_t saved=stats(handle).state_id, version=stats(handle).revision;
    OK(terra_pixel_workspace_tx_begin(handle)); cell(handle,64,65,0); OK(terra_pixel_workspace_tx_commit(handle));
    assert(stats(handle).dirty && stats(handle).active_blocks==1);
    OK(terra_pixel_workspace_undo(handle)); assert(stats(handle).state_id==saved && !stats(handle).dirty && get(handle,64,65)==manual);
    assert(stats(handle).revision>version);
    OK(terra_pixel_workspace_redo(handle)); assert(stats(handle).dirty && get(handle,64,65)==0);
    OK(terra_pixel_workspace_undo(handle)); OK(terra_pixel_workspace_undo(handle)); assert(!stats(handle).used_cells);
    OK(terra_pixel_workspace_tx_begin(handle)); cell(handle,4,4,near); OK(terra_pixel_workspace_tx_commit(handle));
    assert(!stats(handle).redo_count && terra_pixel_workspace_redo(handle)==TERRA_PIXEL_HISTORY);
    OK(terra_pixel_workspace_tx_begin(handle)); uint32_t before_count=stats(handle).palette_count; color(handle,0xffffff); cell(handle,2,2,manual);
    TerraPixelCell invalid[2]={{3,3,manual},{-1,0,manual}};
    assert(terra_pixel_workspace_cells(handle,invalid,2)==TERRA_PIXEL_BOUNDS);
    assert(!stats(handle).transaction_open && stats(handle).palette_count==before_count && !get(handle,2,2) && !get(handle,3,3));
    OK(terra_pixel_workspace_tx_begin(handle)); cell(handle,4,4,black); cell(handle,4,4,near); uint32_t state=stats(handle).state_id;
    OK(terra_pixel_workspace_tx_commit(handle)); assert(stats(handle).state_id==state && stats(handle).undo_count==1);
    OK(terra_pixel_workspace_clear_history(handle)); assert(!stats(handle).history_bytes && !stats(handle).undo_count);
    tx_reset_heap(); assert(get(handle,4,4)==near); /* Persistent ownership survives unrelated native rewinds. */
    OK(terra_pixel_workspace_close(handle)); assert(terra_pixel_workspace_close(handle)==TERRA_PIXEL_HANDLE);
    assert(tx_native_heap_used()==baseline);
}
static void ref_brush(uint16_t* board,int x,int y,uint16_t value,uint32_t size) {
    int left=x-(int)(size/2),top=y-(int)(size/2);
    for(uint32_t dy=0;dy<size;++dy)for(uint32_t dx=0;dx<size;++dx){int xx=left+(int)dx,yy=top+(int)dy;if(xx>=0&&yy>=0&&xx<(int)W&&yy<(int)H)board[(uint32_t)yy*W+(uint32_t)xx]=value;}
}
static void ref_stroke(uint16_t* board,const TerraPixelPoint* points,uint32_t count,uint16_t value,uint32_t size) {
    for(uint32_t i=0;i<count;++i){int x=points[i?i-1:0].x,y=points[i?i-1:0].y,ex=points[i].x,ey=points[i].y;
        int dx=abs(ex-x),dy=abs(ey-y),sx=x<ex?1:-1,sy=y<ey?1:-1,error=dx-dy;
        for(;;){ref_brush(board,x,y,value,size);if(x==ex&&y==ey)break;int twice=2*error;if(twice>-dy){error-=dy;x+=sx;}if(twice<dx){error+=dx;y+=sy;}}
    }
}
static void ref_fill(uint16_t* board,uint32_t x,uint32_t y,uint16_t value){
    uint16_t target=board[y*W+x];if(target==value)return;
    uint32_t queue[W*H],first=0,last=0;queue[last++]=y*W+x;board[y*W+x]=value;
    while(first<last){uint32_t cell=queue[first++],xx=cell%W,yy=cell/W,candidates[4],count=0;
        if(xx)candidates[count++]=cell-1;
        if(xx+1<W)candidates[count++]=cell+1;
        if(yy)candidates[count++]=cell-W;
        if(yy+1<H)candidates[count++]=cell+W;
        for(uint32_t i=0;i<count;++i)if(board[candidates[i]]==target){board[candidates[i]]=value;queue[last++]=candidates[i];}
    }
}
static void differential_edits(void) {
    uint32_t baseline=tx_native_heap_used(),handle=create(W,H,8u*1024u*1024u,1024);
    uint16_t board[W*H]={0},before[W*H],copy[W*H];
    for(uint32_t i=0;i<12;++i)assert(color(handle,random_u32()&0xffffffu)==i+1);
    for(uint32_t round=0;round<240;++round){
        memcpy(before,board,sizeof board); OK(terra_pixel_workspace_tx_begin(handle));
        uint32_t operation=random_u32()%5u,index=random_u32()%13u;
        if(operation==0){TerraPixelCell cells[73];for(uint32_t i=0;i<73;++i){cells[i]=(TerraPixelCell){(int32_t)(random_u32()%W),(int32_t)(random_u32()%H),random_u32()%13u};board[(uint32_t)cells[i].y*W+(uint32_t)cells[i].x]=(uint16_t)cells[i].index;}OK(terra_pixel_workspace_cells(handle,cells,73));}
        else if(operation==1){TerraPixelPoint points[5];uint32_t size=random_u32()%7u+1u;for(uint32_t i=0;i<5;++i)points[i]=(TerraPixelPoint){(int32_t)(random_u32()%W),(int32_t)(random_u32()%H)};OK(terra_pixel_workspace_stroke(handle,points,5,index,size));ref_stroke(board,points,5,(uint16_t)index,size);}
        else if(operation==2){uint32_t x=random_u32()%W,y=random_u32()%H;OK(terra_pixel_workspace_fill(handle,(int32_t)x,(int32_t)y,index));ref_fill(board,x,y,(uint16_t)index);}
        else if(operation==3){uint32_t from=random_u32()%13u;OK(terra_pixel_workspace_replace(handle,from,index));for(uint32_t i=0;i<W*H;++i)if(board[i]==from)board[i]=(uint16_t)index;}
        else {uint32_t size=random_u32()%28u+1u,x=random_u32()%(W-size+1u),y=random_u32()%(H-size+1u),kind=random_u32()%5u+1u;
            memcpy(copy,board,sizeof board);OK(terra_pixel_workspace_transform(handle,(int32_t)x,(int32_t)y,size,size,kind));
            for(uint32_t yy=0;yy<size;++yy)for(uint32_t xx=0;xx<size;++xx){uint32_t sx=xx,sy=yy;
                if(kind==1)sx=size-1u-xx;else if(kind==2)sy=size-1u-yy;else if(kind==3){sx=size-1u-xx;sy=size-1u-yy;}else if(kind==4){sx=yy;sy=size-1u-xx;}else {sx=size-1u-yy;sy=xx;}
                board[(y+yy)*W+x+xx]=copy[(y+sy)*W+x+sx];}
        }
        if(round%11u==0){OK(terra_pixel_workspace_tx_rollback(handle));memcpy(board,before,sizeof board);}
        else {uint32_t old_state=stats(handle).state_id;OK(terra_pixel_workspace_tx_commit(handle));
            if(round%7u==0 && stats(handle).state_id!=old_state){OK(terra_pixel_workspace_undo(handle));compare(handle,before,W,H);OK(terra_pixel_workspace_redo(handle));}}
        compare(handle,board,W,H);
        if(round%31u==0){OK(terra_pixel_workspace_checkpoint(handle));assert(!stats(handle).dirty);}
    }
    /* Legacy Bresenham visits outside endpoints but clips every brush cell. */
    TerraPixelPoint outside[3]={{-12,-5},{80,73},{-8,62}};
    OK(terra_pixel_workspace_tx_begin(handle));
    OK(terra_pixel_workspace_stroke(handle,outside,3,7,3));
    ref_stroke(board,outside,3,7,3);
    OK(terra_pixel_workspace_tx_commit(handle));compare(handle,board,W,H);
    OK(terra_pixel_workspace_close(handle));assert(tx_native_heap_used()==baseline);
}
static uint32_t quantize(uint8_t channel){uint32_t level=(uint32_t)((double)channel*31.0/255.0+0.5);return level*255u/31u;}
static uint16_t ref_color(uint32_t* palette,uint32_t* count,uint32_t limit,uint32_t rgb){
    for(uint32_t i=1;i<*count;++i)if(palette[i]==rgb)return(uint16_t)i;
    if(*count<limit){uint32_t index=(*count)++;palette[index]=rgb;return(uint16_t)index;}
    uint32_t best=0,dist=UINT32_MAX;for(uint32_t i=1;i<*count;++i){uint32_t d=distance(rgb,palette[i]);if(d<dist){best=i;dist=d;}}return(uint16_t)best;
}
static void differential_import(void){
    uint32_t baseline=tx_native_heap_used();
    /* Rollback must invalidate import cache indices that were truncated from the
     * palette. A later manual color can occupy the same former numeric slot. */
    uint32_t retry=create(1,1,1024,16),first=color(retry,0x102030);
    uint8_t red[4]={255,0,0,255},transparent[4]={255,255,255,15};
    OK(terra_pixel_workspace_tx_begin(retry));cell(retry,0,0,first);OK(terra_pixel_workspace_tx_commit(retry));
    OK(terra_pixel_workspace_tx_begin(retry));OK(terra_pixel_workspace_import_rgba(retry,0,0,1,1,red,4,4));OK(terra_pixel_workspace_tx_rollback(retry));
    assert(color(retry,0x00ff00)==2);
    OK(terra_pixel_workspace_tx_begin(retry));OK(terra_pixel_workspace_import_rgba(retry,0,0,1,1,red,4,4));OK(terra_pixel_workspace_tx_commit(retry));
    assert(get(retry,0,0)==3);uint32_t retry_state=stats(retry).state_id;
    OK(terra_pixel_workspace_tx_begin(retry));OK(terra_pixel_workspace_import_rgba(retry,0,0,1,1,transparent,4,4));OK(terra_pixel_workspace_tx_commit(retry));
    assert(get(retry,0,0)==3&&stats(retry).state_id==retry_state);OK(terra_pixel_workspace_close(retry));assert(tx_native_heap_used()==baseline);
    for(uint32_t limit_case=0;limit_case<3;++limit_case){uint32_t limit=limit_case==0?65536u:limit_case==1?17u:1u;
        uint32_t handle=create(W,H,1024u*1024u,limit),palette[65536]={0},count=1;uint16_t expected[W*H]={0};uint8_t rgba[W*H*4+H*12];
        uint32_t stride=W*4u+12u;
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){uint8_t* p=rgba+y*stride+x*4u;p[0]=(uint8_t)random_u32();p[1]=(uint8_t)random_u32();p[2]=(uint8_t)random_u32();p[3]=(uint8_t)random_u32();if((x+y)%7u==0)p[3]=15;if((x+y)%13u==0)p[3]=16;}
        for(uint32_t first=0;first<H;first+=9u){uint32_t rows=H-first;if(rows>9u)rows=9u;OK(terra_pixel_workspace_tx_begin(handle));
            OK(terra_pixel_workspace_import_rgba(handle,0,(int32_t)first,W,rows,rgba+first*stride,rows*stride,stride));OK(terra_pixel_workspace_tx_commit(handle));
            for(uint32_t y=first;y<first+rows;++y)for(uint32_t x=0;x<W;++x){uint8_t* p=rgba+y*stride+x*4u;if(p[3]<16)continue;uint32_t rgb=(quantize(p[0])<<16)|(quantize(p[1])<<8)|quantize(p[2]);uint16_t index=ref_color(palette,&count,limit,rgb);if(index)expected[y*W+x]=index;}}
        compare(handle,expected,W,H);assert(stats(handle).palette_count==count);
        uint32_t actual[4096];for(uint32_t first=0;first<count;first+=4096u){uint32_t n=count-first;if(n>4096)n=4096;OK(terra_pixel_workspace_palette_read(handle,first,n,actual));assert(!memcmp(actual,palette+first,n*4u));}
        OK(terra_pixel_workspace_tx_begin(handle));uint32_t saved=count;uint8_t pixel[4]={18,92,183,255};OK(terra_pixel_workspace_import_rgba(handle,0,0,1,1,pixel,4,4));OK(terra_pixel_workspace_tx_rollback(handle));assert(stats(handle).palette_count==saved);compare(handle,expected,W,H);
        OK(terra_pixel_workspace_close(handle));assert(tx_native_heap_used()==baseline);
    }
    /* Every 5-bit bucket is imported exactly once. This is the historical JS
     * cache domain and exercises palette hash collisions without quadratic scans. */
    uint32_t handle=create(256,128,0,65536);uint8_t* rgba=malloc(32768u*4u);assert(rgba);
    for(uint32_t i=0;i<32768u;++i){rgba[i*4]=(uint8_t)((i>>10)*255u/31u);rgba[i*4+1]=(uint8_t)(((i>>5)&31u)*255u/31u);rgba[i*4+2]=(uint8_t)((i&31u)*255u/31u);rgba[i*4+3]=255;}
    OK(terra_pixel_workspace_tx_begin(handle));OK(terra_pixel_workspace_import_rgba(handle,0,0,256,128,rgba,32768u*4u,1024));OK(terra_pixel_workspace_tx_commit(handle));
    assert(stats(handle).palette_count==32769u && stats(handle).used_cells==32768u && !stats(handle).history_bytes);free(rgba);OK(terra_pixel_workspace_close(handle));assert(tx_native_heap_used()==baseline);
}
static uint32_t brute_candidate(const TerraPixelCandidate* candidates,uint32_t size,uint32_t rgb,uint32_t flags){
    uint32_t best=UINT32_MAX,dist=UINT32_MAX,rank=UINT32_MAX;
    for(uint32_t i=0;i<size;++i){uint32_t p=candidates[i].flags;
        if(((flags&1)&&(p&1))||((flags&2)&&!(p&2))||((flags&8)&&(p&2))||((flags&16)&&!(p&1)))continue;
        uint32_t d=distance(rgb,candidates[i].rgb),r=(p&1)*2u+(((flags&4)&&!(p&2))?1u:0u);
        if(d<dist||(d==dist&&r<rank)){best=i;dist=d;rank=r;}}
    return best;
}
static void differential_matching(void){
    uint32_t baseline=tx_native_heap_used();TerraPixelCandidate candidates[1300];uint32_t queries[257],results[257];
    for(uint32_t i=0;i<1300;++i)candidates[i]=(TerraPixelCandidate){random_u32()&0xffffffu,random_u32()%4u};
    for(uint32_t i=500;i<1000;++i)candidates[i].rgb=candidates[i-500].rgb;
    candidates[0]=(TerraPixelCandidate){0x010100,1};candidates[1]=(TerraPixelCandidate){0x010102,2};candidates[2]=(TerraPixelCandidate){0x010100,0};candidates[3]=(TerraPixelCandidate){0x010102,0};
    for(uint32_t i=0;i<257;++i)queries[i]=random_u32()&0xffffffu;
    queries[0]=0x010101;queries[1]=0x010100;
    for(uint32_t flags=0;flags<32;++flags){if(((flags&2)&&(flags&8))||((flags&1)&&(flags&16)))continue;
        OK(terra_pixel_workspace_match_colors(candidates,1300,queries,257,flags,results));
        for(uint32_t i=0;i<257;++i)assert(results[i]==brute_candidate(candidates,1300,queries[i],flags));
        assert(tx_native_heap_used()==baseline);}
    txw_test_allocation_limit(0);assert(terra_pixel_workspace_match_colors(candidates,1300,queries,257,0,results)==TERRA_PIXEL_OOM);txw_test_allocation_limit(UINT32_MAX);assert(tx_native_heap_used()==baseline);
    TerraPixelCandidate* duplicates=malloc(65536u*sizeof(*duplicates));uint32_t* many=malloc(65536u*4u),*out=malloc(65536u*4u);assert(duplicates&&many&&out);
    for(uint32_t i=0;i<65536u;++i)duplicates[i]=(TerraPixelCandidate){0x123456,i%4u};
    for(uint32_t i=0;i<65536u;++i)many[i]=i;
    OK(terra_pixel_workspace_match_colors(duplicates,65536,many,65536,4,out));for(uint32_t i=0;i<65536u;++i)assert(out[i]==2);
    free(duplicates);free(many);free(out);assert(tx_native_heap_used()==baseline);
}
static void failure_and_budget_contract(void){
    uint32_t baseline=tx_native_heap_used(),handle=create(128,128,1024u*1024u,64),index=color(handle,0x123456);
    OK(terra_pixel_workspace_tx_begin(handle));cell(handle,0,0,index);OK(terra_pixel_workspace_tx_commit(handle));
    TerraPixelWorkspaceStats before=stats(handle);
    OK(terra_pixel_workspace_tx_begin(handle));cell(handle,1,0,index);color(handle,0x987654);
    txw_test_allocation_limit(64);TerraPixelCell entry={127,127,index};assert(terra_pixel_workspace_cells(handle,&entry,1)==TERRA_PIXEL_OOM);txw_test_allocation_limit(UINT32_MAX);
    assert(!get(handle,1,0)&&!get(handle,127,127)&&get(handle,0,0)==index);assert(stats(handle).state_id==before.state_id&&stats(handle).palette_count==before.palette_count&&!stats(handle).transaction_open);
    OK(terra_pixel_workspace_tx_begin(handle));cell(handle,1,0,index);txw_test_allocation_limit(64);assert(terra_pixel_workspace_tx_commit(handle)==TERRA_PIXEL_OOM);txw_test_allocation_limit(UINT32_MAX);assert(!get(handle,1,0));
    OK(terra_pixel_workspace_tx_begin(handle));cell(handle,0,0,0);OK(terra_pixel_workspace_tx_commit(handle));before=stats(handle);assert(!before.active_blocks);
    txw_test_allocation_limit(64);assert(terra_pixel_workspace_undo(handle)==TERRA_PIXEL_OOM);txw_test_allocation_limit(UINT32_MAX);
    assert(stats(handle).state_id==before.state_id&&!stats(handle).active_blocks&&stats(handle).undo_count==before.undo_count);OK(terra_pixel_workspace_undo(handle));assert(get(handle,0,0)==index);
    OK(terra_pixel_workspace_close(handle));assert(tx_native_heap_used()==baseline);
    handle=create(64,64,128,16);index=color(handle,0x123456);OK(terra_pixel_workspace_tx_begin(handle));cell(handle,0,0,index);assert(terra_pixel_workspace_tx_commit(handle)==TERRA_PIXEL_LIMIT);assert(!get(handle,0,0)&&!stats(handle).undo_count);OK(terra_pixel_workspace_close(handle));
    handle=create(128,128,0,16);uint32_t minimum=stats(handle).active_bytes;OK(terra_pixel_workspace_close(handle));
    OK(terra_pixel_workspace_create(128,128,minimum+20000u,0,16,&handle));index=color(handle,0);OK(terra_pixel_workspace_tx_begin(handle));assert(terra_pixel_workspace_fill(handle,0,0,index)==TERRA_PIXEL_LIMIT);assert(!stats(handle).used_cells&&!stats(handle).active_blocks&&!stats(handle).transaction_open);OK(terra_pixel_workspace_close(handle));
    /* Byte-budget eviction and saved identity remain independent. */
    handle=create(8,8,1024,16);index=color(handle,0);for(uint32_t i=0;i<40;++i){OK(terra_pixel_workspace_tx_begin(handle));cell(handle,(int32_t)(i%8),(int32_t)(i/8),index);OK(terra_pixel_workspace_tx_commit(handle));assert(stats(handle).history_bytes<=1024);}
    uint32_t undos=stats(handle).undo_count;assert(undos>0&&undos<40);for(uint32_t i=0;i<undos;++i)OK(terra_pixel_workspace_undo(handle));assert(terra_pixel_workspace_undo(handle)==TERRA_PIXEL_HISTORY);for(uint32_t i=0;i<undos;++i)OK(terra_pixel_workspace_redo(handle));assert(stats(handle).used_cells==40);OK(terra_pixel_workspace_close(handle));assert(tx_native_heap_used()==baseline);
    uint32_t handles[16];for(uint32_t i=0;i<16;++i)handles[i]=create(1,1,0,1);uint32_t missing=99;assert(terra_pixel_workspace_create(1,1,100000,0,1,&missing)==TERRA_PIXEL_LIMIT&&!missing);for(uint32_t i=0;i<16;++i)OK(terra_pixel_workspace_close(handles[i]));assert(tx_native_heap_used()==baseline);
}
int main(void){assert(terra_pixel_workspace_abi_version()==1u);basic_contract();differential_edits();differential_import();differential_matching();failure_and_budget_contract();for(uint32_t i=0;i<20;++i)basic_contract();puts("pixel workspace: differential edits/import/matching, rollback, budgets, raster, history and 20 lifecycles passed");return 0;}
