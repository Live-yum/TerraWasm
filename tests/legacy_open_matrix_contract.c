/* Independent pre-88 source-layout fixture. No TerraWasm encoder is used.
 * This exercises Release libc paths as well as every historical open/save gate. */
#include "terra_world.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t bytes[16384];static uint32_t length;
static void byte(uint32_t n){assert(length<sizeof bytes);bytes[length++]=(uint8_t)n;}
static void zero(uint32_t n){while(n--)byte(0);}
static void i16(uint32_t n){byte(n);byte(n>>8);}
static void i32(uint32_t n){for(unsigned i=0;i<4;i++)byte(n>>(i*8));}
static void text(const char*s){uint32_t n=(uint32_t)strlen(s);assert(n<128);byte(n);while(*s)byte((uint8_t)*s++);}
static void fixture(uint32_t v){
    length=0;i32(v);text("Legacy World");i32(87);
    i32(0);i32(1);i32(0);i32(1);i32(1);i32(1);
    if(v>=63)byte(2);if(v>=44)zero(28);if(v>=60){zero(32);if(v>=61)zero(8);}
    zero(8+24);byte(1);zero(4+1);if(v>=70)byte(0);
    zero(8);if(v>=56)byte(0);zero(3);if(v>=66)byte(0);if(v>=44)zero(4);if(v>=64)zero(2);
    if(v>=29){zero(2);if(v>=34){byte(0);if(v>=80)byte(0);}byte(0);}
    if(v>=32)byte(0);if(v>=37)byte(0);if(v>=56)byte(0);zero(3);if(v>=23)zero(5);
    zero(12+8);if(v>=53)zero(9);if(v>=54){i32(107);i32(108);i32(111);}
    if(v>=55)zero(3);if(v>=60)zero(9);if(v>=62)zero(6);
    byte(1);if(v<=77)byte(3);else i16(3);i16(12);i16(34);
    if(v>=48)byte(0);if(v<=25)byte(0);zero(2);if(v>=33)byte(0);if(v>=43)zero(2);
    if(v>=41){byte(0);if(v>=49)byte(0);}if(v>=42)zero(2);if(v>=25)i16(0);
    byte(1);zero(8);if(v>=85)text("Chest");
    uint32_t slots=v<58?20:40;
    for(uint32_t i=0;i<slots;i++){if(v<59)byte(i?0:3);else i16(i?0:3);if(!i){if(v>=38)i32(1);else text("Dirt");if(v>=36)byte(0);}}
    zero(999);byte(1);text("Legacy sign");zero(8);zero(999);
    byte(1);text("Guide");if(v>=83)text("Guide");i32(0x3f800000);i32(0x40000000);zero(10);
    if(v>=31&&v<=83){uint32_t n=9+(v>=35)+(v>=65?8:0)+(v>=79);for(uint32_t i=0;i<n;i++)text(i==4?"Old Guide":"");}
    if(v>=7){byte(1);text("Legacy World");i32(87);}
}
int main(void){
    /* Volatile calls prevent the test compiler from eliding bulk operations. */
    void*(*volatile set)(void*,int,size_t)=memset;
    void*(*volatile copy)(void*,const void*,size_t)=memcpy;
    uint8_t a[8192],b[8192];set(a,0x5a,sizeof a);copy(b,a,sizeof a);assert(!memcmp(a,b,sizeof a));
    for(uint32_t v=1;v<=87;v++){
        fixture(v);uint32_t h=0,n=0;int status=terra_world_open_from_buffer(bytes,length,&h);
        if(status){char error[512];uint64_t z;terra_info_get_last_error_json(error,sizeof error,&z);fprintf(stderr,"v%u: %s\n",v,error);}assert(status==0&&h);
        assert(terra_world_save_to_buffer(h,NULL,0,&n)==0&&n==length);uint8_t*out=malloc(n);assert(out);
        assert(terra_world_save_to_buffer(h,out,n,&n)==0&&n==length&&!memcmp(out,bytes,n));free(out);assert(terra_world_close(h)==0);
    }
    puts("Release legacy open/save: all 87 historical layouts byte-identical");return 0;
}
