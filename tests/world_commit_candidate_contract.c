#include "terra_world.h"
#include "terra_types.h"
#include "terra_output.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern TxWorld* tx_get_world(uint32_t);
extern void txw_test_fail_after(uint32_t);
extern uint32_t tx_native_heap_used(void);
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);
static void run(const char* file) {
    unsigned rejected=0,committed=0;
    for(unsigned prepared=0;prepared<2;prepared++)for(unsigned fault=0;fault<24;fault++) {
        uint32_t h=0,next=99,size=0;uint64_t needed=0;
        assert(terra_world_open(file,&h)==0);
        if(!tx_get_world(h)->legacy_wld)assert(terra_op_execute_json(h,"header_patch","{\"patch\":{\"worldName\":\"retained until candidate ready\"}}",NULL,0,&needed)==0);
        TxWorld* w=tx_get_world(h);
        if(prepared){char result[256];assert(terra_op_execute_json(h,"begin_output_preparation","{\"preview_width\":64}",result,sizeof(result),&needed)==0);assert(w->prepared_output);}
        assert(terra_world_commit_to_buffer(h,NULL,0,&size,&next)==0&&next==0&&size>0);
        uint8_t* output=malloc(size);assert(output);TxWorld before=*w;uint32_t heap=tx_native_heap_used();
        txw_test_fail_after(fault);
        int status=terra_world_commit_to_buffer(h,output,size,&size,&next);
        txw_test_fail_after(UINT32_MAX);
        if(status){
            rejected++;assert(next==0);
            assert(tx_get_world(h)==w); /* Old implementation closes h before its failing allocation. */
            assert(!memcmp(w,&before,sizeof(before))&&tx_native_heap_used()==heap);
            uint32_t again=0;assert(terra_world_save_to_buffer(h,NULL,0,&again)==0&&again==size);
            assert(terra_world_close(h)==0);
        }else{
            committed++;assert(next&&next!=h&&!tx_get_world(h));TxWorld* fresh=tx_get_world(next);assert(fresh);
            assert(!strcmp(fresh->worldName,before.worldName));
            assert(fresh->prepared_output==before.prepared_output&&fresh->tile_decode_calls==before.tile_decode_calls);
            if(fresh->legacy_wld){uint32_t offset=fresh->starts[1];TxTile tile;assert(read_tile_at(fresh,&offset,fresh->ends[1],&tile)&&tile.type==3);}
            uint32_t again=0;assert(terra_world_save_to_buffer(next,NULL,0,&again)==0&&again==size);
            uint8_t* saved=malloc(again);assert(saved);assert(terra_world_save_to_buffer(next,saved,again,&again)==0);
            assert(!memcmp(saved,output,size));free(saved);
            if(fault==23)for(unsigned repeat=0;repeat<3;repeat++) {
                uint32_t replaced=0;assert(terra_world_commit_to_buffer(next,output,size,&again,&replaced)==0);
                assert(replaced&&replaced!=next&&!tx_get_world(next));next=replaced;
                assert(tx_get_world(next)->prepared_output==before.prepared_output);
            }
            assert(terra_world_close(next)==0);
        }
        free(output);assert(tx_native_heap_used()==0);
    }
    assert(rejected&&committed);printf("candidate commit: %u failures retain old owner, %u replacements succeed\n",rejected,committed);
}
extern uint8_t* tx_alloc(uint32_t);
extern int set_section_override_data(TxWorld*,int,uint8_t*,uint32_t);
static void corrupt_candidates(const char* file) {
    for(unsigned workspace=0;workspace<2;workspace++)for(unsigned corruption=0;corruption<4;corruption++) {
        uint32_t h=0,next=99,size=0,token=0,budget=0;
        assert(terra_world_open(file,&h)==0);TxWorld* w=tx_get_world(h);
        uint32_t original_size=w->file_len;uint8_t* original=malloc(original_size);assert(original);
        memcpy(original,w->file,original_size);
        if(workspace){assert(terra_world_workspace_checkpoint_size(h,&budget)==0);assert(terra_world_workspace_begin(h,budget,&token)==0);}
        unsigned section=corruption==3?w->pointer_count-1:1;
        uint32_t length=w->ends[section]-w->starts[section]+(corruption==2);
        uint8_t* bytes=tx_alloc(length);assert(bytes);memcpy(bytes,w->file+w->starts[section],length-(corruption==2));
        if(corruption==0){bytes[0]=0x22;bytes[1]=0xff;bytes[2]=0xff;}
        if(corruption==1){bytes[0]=0x80;bytes[1]=0xff;bytes[2]=0x7f;}
        if(corruption==2)bytes[length-1]=0;
        if(corruption==3)bytes[0]=0;
        assert(set_section_override_data(w,section,bytes,length));
        int status=workspace?terra_world_workspace_save_to_buffer(h,NULL,0,&size):terra_world_commit_to_buffer(h,NULL,0,&size,&next);
        assert(status==0&&size);uint8_t* output=malloc(size);assert(output);
        status=workspace?terra_world_workspace_save_to_buffer(h,output,size,&size):terra_world_commit_to_buffer(h,output,size,&size,&next);
        assert(status==TERRAX_WORLD_STATUS_PARSE_ERROR&&tx_get_world(h)==w);
        assert(workspace||next==0);assert(!memcmp(w->file,original,original_size));
        if(workspace){assert(terra_world_workspace_rollback(h,token)==0);assert(terra_world_save_to_buffer(h,output,size,&size)==0);assert(size==original_size&&!memcmp(output,original,size));}
        free(output);free(original);assert(terra_world_close(h)==0);assert(tx_native_heap_used()==0);
    }
    puts("candidate validation: invalid tile types, RLE, trailing tile bytes and footer rejected by both save paths");
}
int main(int argc,char**argv){assert(argc==3&&terra_world_commit_abi_version()==1);run(argv[1]);run(argv[2]);corrupt_candidates(argv[1]);return 0;}
