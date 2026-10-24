#include "terra_world.h"
#include "terra_types.h"
#include "terra_checkpoint.h"
#include "terra_native_span.h"
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern TxWorld* tx_get_world(uint32_t);
extern void txw_test_fail_after(uint32_t);
extern uint32_t tx_native_heap_used(void);
_Static_assert(sizeof(TerraOutcomeV2)==40,"fixed native outcome size");
_Static_assert(offsetof(TerraOutcomeV2,revision_before)==24,"native outcome revision alignment");
static const char* patch="{\"patch\":{\"worldName\":\"fixed receipt\"}}";
static uint8_t* original;static uint32_t original_size;
static void receipt(const TerraOutcomeV2* o,int status,uint32_t effect,uint32_t state,uint64_t before,uint64_t after) {
    assert(o->struct_size==40&&o->version==2&&o->status==status&&o->reserved==0);
    assert(o->effect==effect&&o->transaction_state==state&&o->revision_before==before&&o->revision_after==after);
}
static void unchanged_file(uint32_t h) {
    uint32_t n=0;assert(terra_world_save_to_buffer(h,NULL,0,&n)==0&&n==original_size);
    uint8_t* bytes=malloc(n);assert(bytes);assert(terra_world_save_to_buffer(h,bytes,n,&n)==0);
    assert(n==original_size&&!memcmp(bytes,original,n));free(bytes);
}
static void sweep(const char* file) {
    unsigned rejected=0,rolled_back=0,applied=0;
    for(unsigned joined=0;joined<2;joined++)for(unsigned prepared=0;prepared<2;prepared++)for(uint32_t fault=0;fault<80;fault++) {
        uint32_t h=0,token=0;uint64_t needed=99;TerraOutcomeV2 out;
        assert(terra_world_open(file,&h)==0);TxWorld*w=tx_get_world(h);
        char name[sizeof(w->worldName)];memcpy(name,w->worldName,sizeof(name));uint32_t file_revision=w->revision;
        assert(w->workspace_revision==0);
        if(joined){
            assert(terra_world_workspace_begin(h,UINT32_MAX,&token)==0);
            assert(terra_op_execute_json(h,"header_patch","{\"patch\":{\"worldName\":\"earlier provisional edit\"}}",NULL,0,&needed)==0);
            assert(!strcmp(w->worldName,"earlier provisional edit"));
        }
        if(prepared){char response[256];assert(terra_op_execute_json(h,"begin_output_preparation","{\"preview_width\":64}",response,sizeof(response),&needed)==0);}
        memset(&out,0xa5,sizeof(out));txw_test_fail_after(fault);
        int status=terra_op_execute_json_outcome(h,"header_patch",patch,NULL,0,&needed,joined?TERRA_OUTCOME_JOIN_CALLER:0,&out);
        txw_test_fail_after(UINT32_MAX);
        assert(w->revision==file_revision);
        if(status!=0){
            assert(!tx_checkpoint_active()&&needed==0&&w->workspace_revision==0&&!memcmp(w->worldName,name,sizeof(name)));
            if(out.effect==TERRA_EFFECT_NONE){assert(!joined);rejected++;receipt(&out,status,TERRA_EFFECT_NONE,TERRA_TX_IDLE,0,0);}
            else{rolled_back++;receipt(&out,status,TERRA_EFFECT_ROLLED_BACK,TERRA_TX_ROLLED_BACK,0,0);}
            unchanged_file(h);
        }else{
            applied++;assert(!strcmp(w->worldName,"fixed receipt"));
            receipt(&out,0,TERRA_EFFECT_APPLIED,joined?TERRA_TX_OPEN:TERRA_TX_COMMITTED,0,joined?0:1);
            assert(tx_checkpoint_active()==(int)joined&&needed>0&&needed<2048);
            char response[2048];memset(&out,0xa5,sizeof(out));txw_test_fail_after(0);
            assert(terra_op_execute_json_outcome(h,"header_patch",patch,response,sizeof(response),&needed,0,&out)==0);
            receipt(&out,0,TERRA_EFFECT_NONE,joined?TERRA_TX_OPEN:TERRA_TX_IDLE,joined?0:1,joined?0:1);
            if(joined){assert(terra_world_workspace_commit_outcome(h,token,&out)==0);receipt(&out,0,TERRA_EFFECT_APPLIED,TERRA_TX_COMMITTED,0,1);}
            txw_test_fail_after(UINT32_MAX);assert(!tx_checkpoint_active()&&w->workspace_revision==1);
        }
        assert(terra_world_close(h)==0&&tx_native_heap_used()==0);
    }
    assert(rejected&&rolled_back&&applied);
    printf("fixed outcomes: %u pre-admission rejections, %u confirmed whole-checkpoint rollbacks, %u applied/cache-fill cases\n",rejected,rolled_back,applied);
}
static void controls(const char* file) {
    uint32_t h=0,token=0;uint64_t n=99;TerraOutcomeV2 o;
    assert(terra_world_open(file,&h)==0);TxWorld*w=tx_get_world(h);
    assert(terra_op_execute_json_outcome(h,"header_patch",patch,NULL,0,&n,0,NULL)==TERRAX_WORLD_STATUS_INVALID_ARGUMENT);
    assert(terra_op_execute_json_outcome(h,"header_patch",patch,NULL,0,&n,2,&o)==TERRAX_WORLD_STATUS_INVALID_ARGUMENT);
    receipt(&o,1,TERRA_EFFECT_NONE,TERRA_TX_IDLE,0,0);
    assert(terra_world_workspace_begin(h,UINT32_MAX,&token)==0);
    assert(terra_op_execute_json(h,"header_patch","{\"patch\":{\"worldName\":\"pending\"}}",NULL,0,&n)==0);
    assert(terra_op_execute_json_outcome(h,"header_patch",patch,NULL,0,&n,0,&o)==TERRAX_WORLD_STATUS_STATE_ERROR);
    receipt(&o,8,TERRA_EFFECT_NONE,TERRA_TX_OPEN,0,0);assert(!strcmp(w->worldName,"pending")&&tx_checkpoint_active());
    assert(terra_op_execute_json_outcome(h,"unknown_query","{}",NULL,0,&n,0,&o)==TERRAX_WORLD_STATUS_NOT_FOUND);
    receipt(&o,3,TERRA_EFFECT_NONE,TERRA_TX_OPEN,0,0);assert(!strcmp(w->worldName,"pending")&&tx_checkpoint_active());
    assert(terra_op_execute_json_outcome(h,"header_patch",NULL,NULL,0,&n,TERRA_OUTCOME_JOIN_CALLER,&o)==TERRAX_WORLD_STATUS_INVALID_ARGUMENT);
    receipt(&o,1,TERRA_EFFECT_NONE,TERRA_TX_OPEN,0,0);assert(!strcmp(w->worldName,"pending"));
    assert(terra_world_workspace_rollback_outcome(h,token+1,&o)==8);receipt(&o,8,TERRA_EFFECT_NONE,TERRA_TX_OPEN,0,0);
    txw_test_fail_after(0);assert(terra_world_workspace_rollback_outcome(h,token,&o)==0);receipt(&o,0,TERRA_EFFECT_ROLLED_BACK,TERRA_TX_ROLLED_BACK,0,0);txw_test_fail_after(UINT32_MAX);
    unchanged_file(h);
    char short_buffer[1];assert(terra_op_execute_json_outcome(h,"header_patch",patch,short_buffer,1,&n,0,&o)==2);
    receipt(&o,2,TERRA_EFFECT_APPLIED,TERRA_TX_COMMITTED,0,1);assert(n>1&&!strcmp(w->worldName,"fixed receipt"));
    assert(terra_world_workspace_begin(h,UINT32_MAX,&token)==0);
    assert(terra_world_workspace_commit_outcome(h,token+1,&o)==8);receipt(&o,8,TERRA_EFFECT_NONE,TERRA_TX_OPEN,1,1);
    txw_test_fail_after(0);assert(terra_world_workspace_commit_outcome(h,token,&o)==0);receipt(&o,0,TERRA_EFFECT_APPLIED,TERRA_TX_COMMITTED,1,2);txw_test_fail_after(UINT32_MAX);
    w->workspace_revision=UINT64_MAX;
    assert(terra_op_execute_json_outcome(h,"header_patch","{\"patch\":{\"worldName\":\"must not apply\"}}",NULL,0,&n,0,&o)==8);
    receipt(&o,8,TERRA_EFFECT_NONE,TERRA_TX_IDLE,UINT64_MAX,UINT64_MAX);assert(!strcmp(w->worldName,"fixed receipt"));
    assert(terra_world_close(h)==0&&tx_native_heap_used()==0);
    assert(terra_world_workspace_commit_outcome(h,token,&o)==8);receipt(&o,8,TERRA_EFFECT_NONE,TERRA_TX_INVALID,0,0);
}
static uint64_t wire_value(const uint8_t* p,unsigned n) {
    uint64_t value=0;for(unsigned i=0;i<n;i++)value|=(uint64_t)p[i]<<(8*i);return value;
}
static void wire_controls(const char* file) {
    uint32_t h=0,token=0;assert(terra_world_open(file,&h)==0);TxWorld* w=tx_get_world(h);
    uint8_t* out=tx_bridge_alloc_native(41);uint64_t* required=tx_bridge_alloc_native(8);
    char* op=tx_bridge_alloc_native(13);char* req=tx_bridge_alloc_native(strlen(patch)+1);
    char* response=tx_bridge_alloc_native(2048);assert(out&&required&&op&&req&&response);
    memcpy(op,"header_patch",13);strcpy(req,patch);memset(out,0xa5,41);
    assert(terra_op_execute_json_receipt(h,op,req,NULL,0,required,0,out,39)==1);
    assert(w->workspace_revision==0&&out[0]==0xa5);
    assert(terra_op_execute_json_receipt(h,op,req,NULL,0,(uint64_t*)(out+1),0,out,40)==1);
    assert(wire_value(out+12,4)==TERRA_EFFECT_NONE&&w->workspace_revision==0&&out[40]==0xa5);
    memset(op,'x',13);
    assert(terra_op_execute_json_receipt(h,op,req,NULL,0,required,0,out,40)==1);
    assert(wire_value(out+12,4)==TERRA_EFFECT_NONE&&w->workspace_revision==0);
    memcpy(op,"header_patch",13);
    txw_test_fail_after(0);
    int status=terra_op_execute_json_receipt(h,op,req,NULL,0,required,0,out,40);
    txw_test_fail_after(UINT32_MAX);assert(status!=0&&wire_value(out+12,4)==TERRA_EFFECT_NONE&&w->workspace_revision==0);
    assert(terra_op_execute_json_receipt(h,op,req,NULL,0,required,0,out+1,40)==0);
    assert(wire_value(out+1,4)==40&&wire_value(out+5,4)==2&&wire_value(out+9,4)==0);
    assert(wire_value(out+13,4)==TERRA_EFFECT_APPLIED&&wire_value(out+17,4)==TERRA_TX_COMMITTED);
    assert(wire_value(out+25,8)==0&&wire_value(out+33,8)==1&&w->workspace_revision==1);
    txw_test_fail_after(0);
    assert(terra_op_execute_json_receipt(h,op,req,response,2048,required,0,out,40)==0);
    assert(wire_value(out+12,4)==TERRA_EFFECT_NONE&&wire_value(out+32,8)==1);
    txw_test_fail_after(UINT32_MAX);
    assert(terra_world_workspace_begin(h,UINT32_MAX,&token)==0);
    assert(terra_world_workspace_commit_receipt(h,token,(uint8_t*)1,40)==1&&tx_checkpoint_active());
    txw_test_fail_after(0);
    assert(terra_world_workspace_rollback_receipt(h,token,out,40)==0);
    assert(wire_value(out+12,4)==TERRA_EFFECT_ROLLED_BACK&&wire_value(out+16,4)==TERRA_TX_ROLLED_BACK);
    txw_test_fail_after(UINT32_MAX);
    /* Explicit little-endian encoding retains the entire native 64-bit value. */
    w->workspace_revision=UINT64_C(0x0102030405060708);
    assert(terra_world_workspace_begin(h,UINT32_MAX,&token)==0);
    assert(terra_world_workspace_commit_receipt(h,token,out,40)==0);
    assert(wire_value(out+24,8)==UINT64_C(0x0102030405060708));
    assert(wire_value(out+32,8)==UINT64_C(0x0102030405060709)&&out[24]==8&&out[31]==1);
    tx_bridge_free_native(out);tx_bridge_free_native(required);tx_bridge_free_native(op);
    tx_bridge_free_native(req);tx_bridge_free_native(response);
    assert(terra_world_close(h)==0&&tx_native_heap_used()==0);
}
static void violated_invariant_is_unknown(void) {
    uint32_t h=0,token=0;TerraOutcomeV2 o;uint64_t n=0;
    assert(terra_world_create(&h)==0&&terra_world_workspace_begin(h,UINT32_MAX,&token)==0);
    /* Deliberately bypass the public owner protocol. The public owner must
     * quarantine rather than claim that its now-missing checkpoint rolled back. */
    assert(tx_checkpoint_finish(0));
    assert(terra_world_workspace_rollback_outcome(h,token,&o)==8);receipt(&o,8,TERRA_EFFECT_UNKNOWN,TERRA_TX_INVALID,0,0);
    assert(terra_op_execute_json_outcome(h,"header_patch",patch,NULL,0,&n,TERRA_OUTCOME_JOIN_CALLER,&o)==8);
    receipt(&o,8,TERRA_EFFECT_UNKNOWN,TERRA_TX_INVALID,0,0);
    assert(terra_world_close(h)==8&&tx_native_heap_used()==0); /* empty owner retained until this test process exits */
}
int main(int argc,char**argv) {
    assert(argc==2&&terra_world_outcome_abi_version()==2);
    FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);assert(size>0&&size<UINT32_MAX);rewind(f);
    original_size=(uint32_t)size;original=malloc(original_size);assert(original&&fread(original,1,original_size,f)==original_size);fclose(f);
    sweep(argv[1]);controls(argv[1]);wire_controls(argv[1]);free(original);violated_invariant_is_unknown();return 0;
}
