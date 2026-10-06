/* Emits deterministic semantic JSON and encrypted outputs for comparison with
 * the pre-workspace codec. Intentionally uses only the established public ABI. */
#define main existing_plr_contract_main
#include "plr_contract.c"
#undef main
int main(void) {
  char *fixture=read_json_fixture();uint32_t h=0;
  CHECK(fixture&&terra_plr_open_json(fixture,&h)==0,"differential open");free(fixture);
  uint32_t seed=0x243f6a88u;
  for(uint32_t i=0;i<256;i++){
    seed=1664525u*seed+1013904223u;
    uint32_t slot=seed%58u,stack=(seed>>8)%9999u+1u,prefix=(seed>>24)%85u;
    char patch[700];
    snprintf(patch,sizeof patch,"[{\"path\":\"/name\",\"value\":\"workspace-%u\"},{\"path\":\"/inventory/%u\",\"value\":{\"itemType\":%u,\"stack\":%u,\"prefix\":%u,\"favorited\":true}},{\"path\":\"/inventory/%u/stack\",\"value\":%u}]",i,slot,seed%5000u+1u,stack,prefix,slot,stack+1u);
    CHECK(terra_plr_set_many(h,patch)==0,"differential patch");
    CHECK(terra_plr_set_many(h,"[{\"path\":\"/name\",\"value\":\"rollback\"},{\"path\":\"/not-found\",\"value\":0}]")!=0,"differential failed patch");
    char *json=NULL;uint32_t length=0;CHECK(get_document_json(h,&json,&length),"differential JSON");
    CHECK(fwrite(json,1,length,stdout)==length,"write JSON");free(json);
    if(i%16u==0){uint8_t*data=NULL;uint32_t n=0;CHECK(save_document(h,&data,&n),"differential encrypted output");CHECK(fwrite(data,1,n,stdout)==n,"write encrypted bytes");free(data);}
  }
  CHECK(terra_plr_close(h)==0,"differential close");return 0;
}
