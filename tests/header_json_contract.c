/* Synthetic fixtures only; byte-for-byte producer contract, including NUL. */
#include "terra_world.h"
#include "terra_types.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern TxWorld* tx_get_world(uint32_t);
extern uint64_t txw_test_allocation_attempts(void);
extern uint32_t tx_native_heap_used(void);
extern void buf_u8(TxBuf*,uint8_t);
extern void buf_bytes(TxBuf*,const void*,uint32_t);
extern int buf_reserve(TxBuf*,uint32_t);
extern char tx_last_error[];
static void test_world(const char* name) {
 char path[2048]; snprintf(path,sizeof path,"%s/%s",TERRAX_HEADER_STRING_FIXTURE_DIR,name);
 FILE* f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 unsigned char* source=malloc(size);assert(source);assert(fread(source,1,size,f)==(size_t)size);fclose(f);
 uint32_t h=0;assert(!terra_world_open_from_buffer(source,size,&h));TxWorld* w=tx_get_world(h);
 uint64_t n=0,oldn=0;assert(!terra_section_get_json(h,"header",NULL,0,&oldn));
 void* cache=w->section_response;uint32_t cachelen=w->section_response_len;
 uint64_t attempts=txw_test_allocation_attempts();uint32_t live=tx_native_heap_used();
 assert(!terra_header_get_json(h,NULL,0,&n));assert(n==oldn&&n>1);
 unsigned char* output=malloc(n+2);assert(output);memset(output,0xa5,n+2);
 assert(terra_header_get_json(h,(char*)output+1,n-1,&n)==TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL);
 for(uint64_t i=0;i<n+2;i++)assert(output[i]==0xa5);
 assert(!terra_header_get_json(h,(char*)output+1,0,&n));assert(output[1]==0xa5);
 assert(!terra_header_get_json(h,(char*)output+1,UINT64_MAX,&n));
 assert(!terra_header_get_json(h,(char*)output+1,UINT64_C(1)<<32,NULL));
 assert(!terra_header_get_json(h,NULL,0,NULL));
 assert(output[0]==0xa5&&output[n+1]==0xa5&&output[n]==0);
 assert(!memcmp(output+1,cache,n));assert(cache==w->section_response&&cachelen==w->section_response_len);
 assert(txw_test_allocation_attempts()==attempts&&tx_native_heap_used()==live);
 /* Mutation between calls must be recounted, not served from the old cache. */
 int32_t id=w->worldId;w->worldId=0;assert(!terra_header_get_json(h,NULL,0,&n));
 w->worldId=INT32_MIN;memset(output,0xa5,oldn+2);uint64_t grown=0;
 assert(terra_header_get_json(h,(char*)output+1,n,&grown)==TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL);assert(grown==n+10);
 for(uint64_t i=0;i<oldn+2;i++)assert(output[i]==0xa5);
 w->worldId=id;assert(w->section_response==cache);
 /* Malformed active header must retain a source error, not become OOM. */
 uint32_t end=w->ends[0];w->ends[0]=w->starts[0];n=123;
 assert(terra_header_get_json(h,(char*)output+1,oldn,&n)==TERRAX_WORLD_STATUS_INTERNAL_ERROR);
 assert(n==0&&strstr(tx_last_error,"TERRAX_STATE_ERROR")&&!strstr(tx_last_error,"OOM")&&!strstr(tx_last_error,"SIZE_OVERFLOW"));
 assert(w->section_response==cache);w->ends[0]=end;
 char* legacy=malloc(oldn);assert(legacy);assert(!terra_section_get_json(h,"header",legacy,oldn,&n));
 assert(!w->section_response);assert(!terra_header_get_json(h,(char*)output+1,oldn,&n));assert(!memcmp(legacy,output+1,n));
 /* Borrow the exact active source span to exercise override routing, then restore ownership. */
 w->section_overrides[0].active=1;w->section_overrides[0].data=w->file+w->starts[0];w->section_overrides[0].len=w->ends[0]-w->starts[0];
 assert(!terra_header_get_json(h,(char*)output+1,oldn,&n));assert(!memcmp(legacy,output+1,n));
 memset(&w->section_overrides[0],0,sizeof w->section_overrides[0]);
 free(legacy);free(output);assert(!terra_world_close(h));free(source);
}
int main(void) {
 assert(terra_header_json_abi_version()==1);uint64_t n=99;char b[4]={1,2,3,4};
 assert(terra_header_get_json(0,b,sizeof b,&n)!=0&&n==0&&b[0]==1);
 TxBuf count;buf_init_count(&count);uint64_t attempts=txw_test_allocation_attempts();
 buf_bytes(&count,NULL,UINT32_MAX-1);assert(count.ok&&count.len==UINT32_MAX-1);
 buf_u8(&count,0);assert(count.ok&&count.len==UINT32_MAX);buf_u8(&count,0);assert(!count.ok);
 TxBuf fixed;buf_init_fixed(&fixed,b,4);buf_bytes(&fixed,"abcd",4);buf_u8(&fixed,1);assert(!fixed.ok&&!memcmp(b,"abcd",4));
 buf_init_fixed(&fixed,b,UINT32_MAX);fixed.len=UINT32_MAX-1;assert(buf_reserve(&fixed,1));assert(!buf_reserve(&fixed,2));assert(!memcmp(b,"abcd",4));
 assert(txw_test_allocation_attempts()==attempts);
 const unsigned versions[]={88,179,293,326};for(unsigned i=0;i<4;i++){char name[64];snprintf(name,sizeof name,"v%u-original.wld",versions[i]);test_world(name);}
 test_world("padded-original.wld");
 const unsigned legacy[]={6,7,37,58,87};for(unsigned i=0;i<5;i++){char name[64];snprintf(name,sizeof name,"legacy-v%u-original.wld",legacy[i]);test_world(name);}
 puts("bounded header JSON: exact bytes, no allocations, cache, mutation, malformed source, capacity PASS");return 0;
}
