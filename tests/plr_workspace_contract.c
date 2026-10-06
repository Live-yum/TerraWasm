/* Source-native workspace regression; no claim of Emscripten/device timing. */
#define main existing_plr_contract_main
#include "plr_contract.c"
#undef main
#include <string.h>
extern void terrax_test_plr_fail_alloc_after(int32_t remaining);

int main(void) {
    CHECK(existing_plr_contract_main() == 0, "existing PLR codec contract");
    const uint32_t baseline = tx_heap_used();
    char *fixture = read_json_fixture();
    uint32_t h = 0, required = 0;
    CHECK(fixture && terra_plr_open_json(fixture, &h) == 0, "workspace open");
    free(fixture);
    CHECK(terra_plr_workspace_abi_version() == 1, "workspace ABI");
    CHECK(terra_plr_get_keys(h, "", NULL, 0, &required) == 0 && required < 4096, "bounded field discovery");
    char *keys = malloc(required);
    CHECK(keys && terra_plr_get_keys(h, "", keys, required, &required) == 0, "read keys");
    CHECK(strstr(keys, "\"inventory\"") && strstr(keys, "\"name\""), "keys contain known panels");
    free(keys);
    terrax_test_plr_fail_alloc_after(0);
    CHECK(terra_plr_get_keys(h,"/metadata",NULL,0,&required)==TERRAX_WORLD_STATUS_INTERNAL_ERROR,"key query preserves allocation failure");
    terrax_test_plr_fail_alloc_after(-1);
    CHECK(terra_plr_get_keys(h,NULL,NULL,0,&required)==TERRAX_WORLD_STATUS_INVALID_ARGUMENT,"key query rejects null pointer");
    CHECK(terra_plr_set_many(h, "[]") == 0, "empty transaction");
    char *before = NULL, *after = NULL; uint32_t before_len = 0, after_len = 0;
    CHECK(get_document_json(h, &before, &before_len), "baseline JSON");
    CHECK(terra_plr_release_caches(h) == 0, "release serialization cache");
    uint32_t retained = tx_heap_used();
    const char *failures[] = {
      "[{\"path\":\"/inventory/0/stack\",\"value\":2},{\"path\":\"/inventory/0\",\"value\":null}]",
      "[{\"path\":\"/inventory/0\",\"value\":{\"itemType\":1,\"stack\":1,\"prefix\":0,\"favorited\":false}},{\"path\":\"/inventory/0/stack\",\"value\":\"invalid\"}]",
      "[{\"path\":\"/name\",\"value\":\"first\"},{\"path\":\"/name\",\"value\":\"second\"},{\"path\":\"/missing\",\"value\":0}]",
      "[{\"path\":\"\",\"value\":{\"name\":\"bad-root\"}},{\"path\":\"/name\",\"value\":\"still-bad\"}]",
      "[{\"path\":\"/metadata/magicAndType\",\"value\":244154697780061570},{\"path\":\"/version\",\"value\":0}]",
    };
    for (uint32_t round = 0; round < 20; round++) for (uint32_t i = 0; i < sizeof(failures)/sizeof(failures[0]); i++) {
      CHECK(terra_plr_set_many(h, failures[i]) != 0, "reject invalid patch");
      CHECK(tx_heap_used() == retained, "rollback journal leak");
      CHECK(get_document_json(h, &after, &after_len), "failed transaction JSON");
      CHECK(before_len == after_len && bytes_equal((uint8_t*)before,(uint8_t*)after,before_len), "complete rollback equality");
      free(after); after = NULL;
      CHECK(terra_plr_release_caches(h) == 0, "release failed query cache");
    }
    unsigned oom_attempts = 0;
    for (int32_t budget = 0; budget < 1000; budget++) {
      terrax_test_plr_fail_alloc_after(budget);
      int status = terra_plr_set_many(h,"[{\"path\":\"/inventory/0/stack\",\"value\":3},{\"path\":\"/name\",\"value\":\"oom-candidate\"}]");
      terrax_test_plr_fail_alloc_after(-1);
      if (status == 0) {
        CHECK(terra_plr_replace_json(h,before)==0,"reset successful fault trial");
        break;
      }
      oom_attempts++;
      CHECK(tx_heap_used()==retained,"OOM rollback allocation baseline");
      CHECK(get_document_json(h,&after,&after_len),"OOM rollback JSON");
      CHECK(before_len==after_len&&bytes_equal((uint8_t*)before,(uint8_t*)after,before_len),"OOM rollback equality");
      free(after);after=NULL;CHECK(terra_plr_release_caches(h)==0,"release OOM query cache");
    }
    CHECK(oom_attempts>20 && oom_attempts<1000,"allocation failure sweep completes");
    printf("PLR workspace: %u allocation failure positions rolled back\n",oom_attempts);
    free(before);
    CHECK(terra_plr_set_many(h,"[{\"path\":\"/inventory/0/stack\",\"value\":2},{\"path\":\"/inventory/0\",\"value\":{\"itemType\":1,\"stack\":3,\"prefix\":0,\"favorited\":false}},{\"path\":\"/inventory/0/stack\",\"value\":4}]") == 0, "ordered overlapping transaction");
    char *value = NULL;
    CHECK(get_field_text(h,"/inventory/0/stack",&value) && text_equal(value,"4"), "overlap order"); free(value);
    uint32_t seed = 0x6a09e667;
    for (uint32_t i = 0; i < 300; i++) {
      seed = seed * 1664525u + 1013904223u;
      uint32_t slot = seed % 58u, stack = (seed >> 8) % 9999u + 1u;
      char patch[512], path[128], expected[64];
      snprintf(patch,sizeof patch,"[{\"path\":\"/inventory/%u/itemType\",\"value\":1},{\"path\":\"/inventory/%u/stack\",\"value\":%u},{\"path\":\"/statLife\",\"value\":%u}]",slot,slot,stack,i+100);
      CHECK(terra_plr_set_many(h,patch)==0,"fuzz valid patch");
      snprintf(path,sizeof path,"/inventory/%u/stack",slot); snprintf(expected,sizeof expected,"%u",stack);
      CHECK(get_field_text(h,path,&value)&&text_equal(value,expected),"fuzz field value");free(value);
      if(i%25u==0) {
        uint8_t *bytes=NULL;uint32_t n=0,reopened=0;
        CHECK(save_document(h,&bytes,&n)&&terra_plr_open_from_buffer(bytes,n,&reopened)==0,"fuzz encrypted roundtrip");
        CHECK(get_field_text(reopened,path,&value)&&text_equal(value,expected),"roundtrip field equality");free(value);free(bytes);
        CHECK(terra_plr_close(reopened)==0&&terra_plr_release_caches(h)==0,"roundtrip owner release");
      }
    }
    CHECK(terra_plr_close(h)==0,"workspace close");
    CHECK(terra_plr_get_keys(h,"",NULL,0,&required)!=0,"stale query rejected");
    CHECK(tx_heap_used()==baseline,"workspace lifecycle returns to baseline");
    puts("PLR workspace: 300 seeded edits, 100 rollback faults, overlap order, binary readback, ownership PASS");
    return 0;
}
