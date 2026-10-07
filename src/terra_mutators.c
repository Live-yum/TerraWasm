/* Verified WLD section encoders used by explicit mutating operations. */
#include "terra_types.h"
#include <float.h>
#include <limits.h>

extern uint32_t tx_strlen(const char *s);
extern int tx_streq_c(const char *a,const char *b);
extern void tx_set_error(const char *code,const char *message);
extern void tx_internal_free(void *ptr);
extern uint32_t tx_mark(void);
extern void buf_init(TxBuf *b,uint32_t cap);
extern void buf_u8(TxBuf *b,uint8_t v);
extern void buf_u16le(TxBuf *b,uint32_t v);
extern void buf_u32le(TxBuf *b,uint32_t v);
extern void buf_u64le(TxBuf *b,uint64_t v);
extern void buf_bytes(TxBuf *b,const void *p,uint32_t n);
extern void buf_cstr(TxBuf *b,const char *s);
extern void json_u32(TxBuf *b,uint32_t v);
extern int set_result_buf(TxBuf *b);
extern int set_section_override_data(TxWorld *w,int idx,uint8_t *data,uint32_t len);
extern void *memcpy(void *dst,const void *src,unsigned long n);
extern void *memset(void *dst,int value,unsigned long n);
extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t rd_7bit(const uint8_t *p,uint32_t len,uint32_t *off,int *ok);
extern uint8_t *tx_alloc(uint32_t size);
extern int parse_header(TxWorld *w);

#define TX_MUTATOR_MAX_JSON_BYTES (1024u * 1024u)
#define TX_MUTATOR_MAX_STRING_BYTES 255u
#define TX_MAX_HEADER_PATCH_FIELDS 256u
#define TX_MUTATOR_MAX_CHESTS 1000u
#define TX_MUTATOR_MAX_CHEST_ITEMS 504u
#define TX_MUTATOR_MAX_BESTIARY_ENTRIES 4096u
/* Terraria NPCKillsTracker.POSITIVE_KILL_COUNT_CAP; fits its signed Int32 field. */
#define TX_MUTATOR_MAX_KILL_COUNT 999999999u
#define TX_MUTATOR_MAX_ITEM_TYPE 1000000u

typedef struct TxJsonParser {
    const char *text;
    uint32_t len;
    uint32_t pos;
} TxJsonParser;

typedef struct TxPatchField {
    char name[96];
    uint32_t name_len;
    uint32_t value_start;
    uint32_t value_end;
    uint8_t used;
} TxPatchField;

typedef struct TxSpawnPoint {
    int16_t x;
    int16_t y;
} TxSpawnPoint;

static int mut_fail(const char *code,const char *message){
    tx_set_error(code,message);
    return 0;
    }

static int mut_error(const char *code,const char *message){
    tx_set_error(code,message);
    return -1;
    }

static void discard_response(TxBuf *response){
    if (response&&response->data)tx_internal_free(response->data);
    if (response){response->data=NULL;response->len=0u;response->cap=0u;response->ok=0;}
    tx_last_ptr=0u;
    tx_last_len=0u;
    }

static void jp_ws(TxJsonParser *p){
    while (p->pos<p->len){
        char c=p->text[p->pos];
        if (c!=' '&&c!='\t'&&c!='\n'&&c!='\r')break;
        p->pos++;
        }
    }

static int jp_take(TxJsonParser *p,char expected){
    jp_ws(p);
    if (p->pos>=p->len||p->text[p->pos]!=expected)
        return mut_fail("TERRAX_PARSE_ERROR","malformed JSON structure");
    p->pos++;
    return 1;
    }

static int jp_hex(char c,uint32_t *out){
    if (c>='0'&&c<='9'){*out=(uint32_t)(c-'0');return 1;}
    if (c>='a'&&c<='f'){*out=(uint32_t)(c-'a'+10);return 1;}
    if (c>='A'&&c<='F'){*out=(uint32_t)(c-'A'+10);return 1;}
    return 0;
    }

static int jp_u16_escape(TxJsonParser *p,uint32_t *out){
    uint32_t value=0u;
    for (uint32_t i=0;i<4u;i++){
        uint32_t nibble=0u;
        if (p->pos>=p->len||!jp_hex(p->text[p->pos++],&nibble))
            return mut_fail("TERRAX_PARSE_ERROR","invalid JSON unicode escape");
        value=(value<<4u)|nibble;
        }
    *out=value;
    return 1;
    }

static int jp_emit_utf8(char *out,uint32_t cap,uint32_t *used,uint32_t cp){
    uint8_t bytes[4];
    uint32_t count=0u;
    if (cp==0u||cp>0x10ffffu||(cp>=0xd800u&&cp<=0xdfffu))
        return mut_fail("TERRAX_VALIDATION_ERROR","strings may not contain invalid or NUL code points");
    if (cp<=0x7fu){bytes[0]=(uint8_t)cp;count=1u;}
    else if (cp<=0x7ffu){
        bytes[0]=(uint8_t)(0xc0u|(cp>>6u));bytes[1]=(uint8_t)(0x80u|(cp&0x3fu));count=2u;
        }
    else if (cp<=0xffffu){
        bytes[0]=(uint8_t)(0xe0u|(cp>>12u));bytes[1]=(uint8_t)(0x80u|((cp>>6u)&0x3fu));
        bytes[2]=(uint8_t)(0x80u|(cp&0x3fu));count=3u;
        }
    else{
        bytes[0]=(uint8_t)(0xf0u|(cp>>18u));bytes[1]=(uint8_t)(0x80u|((cp>>12u)&0x3fu));
        bytes[2]=(uint8_t)(0x80u|((cp>>6u)&0x3fu));bytes[3]=(uint8_t)(0x80u|(cp&0x3fu));count=4u;
        }
    if (*used>cap||count>cap-*used)
        return mut_fail("TERRAX_VALIDATION_ERROR","JSON string exceeds the supported byte limit");
    for (uint32_t i=0;i<count;i++)out[(*used)++]=(char)bytes[i];
    return 1;
    }

static int jp_copy_raw_utf8(TxJsonParser *p,char *out,uint32_t cap,uint32_t *used,uint8_t first){
    uint32_t continuation_count;
    uint8_t min_second=0x80u,max_second=0xbfu;
    if (first>=0xc2u&&first<=0xdfu)continuation_count=1u;
    else if (first>=0xe0u&&first<=0xefu){
        continuation_count=2u;
        if (first==0xe0u)min_second=0xa0u;
        if (first==0xedu)max_second=0x9fu;
        }
    else if (first>=0xf0u&&first<=0xf4u){
        continuation_count=3u;
        if (first==0xf0u)min_second=0x90u;
        if (first==0xf4u)max_second=0x8fu;
        }
    else return mut_fail("TERRAX_PARSE_ERROR","invalid UTF-8 in JSON string");
    if (p->pos+continuation_count>p->len||*used+continuation_count+1u>=cap)
        return mut_fail(p->pos+continuation_count>p->len?"TERRAX_PARSE_ERROR":"TERRAX_VALIDATION_ERROR",
                        p->pos+continuation_count>p->len?"truncated UTF-8 in JSON string":"JSON string exceeds the supported byte limit");
    uint8_t second=(uint8_t)p->text[p->pos];
    if (second<min_second||second>max_second)
        return mut_fail("TERRAX_PARSE_ERROR","non-canonical UTF-8 in JSON string");
    for (uint32_t i=1u;i<continuation_count;i++){
        uint8_t byte=(uint8_t)p->text[p->pos+i];
        if (byte<0x80u||byte>0xbfu)
            return mut_fail("TERRAX_PARSE_ERROR","invalid UTF-8 continuation byte in JSON string");
        }
    out[(*used)++]=(char)first;
    for (uint32_t i=0u;i<continuation_count;i++)out[(*used)++]=p->text[p->pos++];
    return 1;
    }

static int jp_string(TxJsonParser *p,char *out,uint32_t cap,uint32_t *out_len){
    uint32_t used=0u;
    if (!out||cap==0u||!jp_take(p,'\"'))return 0;
    while (p->pos<p->len){
        uint8_t c=(uint8_t)p->text[p->pos++];
        if (c=='\"'){
            if (used>=cap)return mut_fail("TERRAX_VALIDATION_ERROR","JSON string exceeds the supported byte limit");
            out[used]=0;
            if (out_len)*out_len=used;
            return 1;
            }
        if (c<0x20u)return mut_fail("TERRAX_PARSE_ERROR","unescaped control character in JSON string");
        if (c>=0x80u){
            if (!jp_copy_raw_utf8(p,out,cap,&used,c))return 0;
            continue;
            }
        if (c!='\\'){
            if (used+1u>=cap)return mut_fail("TERRAX_VALIDATION_ERROR","JSON string exceeds the supported byte limit");
            out[used++]=(char)c;
            continue;
            }
        if (p->pos>=p->len)return mut_fail("TERRAX_PARSE_ERROR","unterminated JSON escape");
        c=(uint8_t)p->text[p->pos++];
        if (c=='\"'||c=='\\'||c=='/'){
            if (used+1u>=cap)return mut_fail("TERRAX_VALIDATION_ERROR","JSON string exceeds the supported byte limit");
            out[used++]=(char)c;
            }
        else if (c=='b'||c=='f'||c=='n'||c=='r'||c=='t'){
            static const char escaped[]={'\b','\f','\n','\r','\t'};
            uint32_t index=c=='b'?0u:c=='f'?1u:c=='n'?2u:c=='r'?3u:4u;
            if (used+1u>=cap)return mut_fail("TERRAX_VALIDATION_ERROR","JSON string exceeds the supported byte limit");
            out[used++]=escaped[index];
            }
        else if (c=='u'){
            uint32_t cp=0u;
            if (!jp_u16_escape(p,&cp))return 0;
            if (cp>=0xd800u&&cp<=0xdbffu){
                uint32_t low=0u;
                if (p->pos+2u>p->len||p->text[p->pos++]!='\\'||p->text[p->pos++]!='u'||
                    !jp_u16_escape(p,&low)||low<0xdc00u||low>0xdfffu)
                    return mut_fail("TERRAX_PARSE_ERROR","invalid JSON surrogate pair");
                cp=0x10000u+((cp-0xd800u)<<10u)+(low-0xdc00u);
                }
            if (!jp_emit_utf8(out,cap-1u,&used,cp))return 0;
            }
        else return mut_fail("TERRAX_PARSE_ERROR","unsupported JSON escape");
        }
    return mut_fail("TERRAX_PARSE_ERROR","unterminated JSON string");
    }

static int jp_bool(TxJsonParser *p,int *out){
    jp_ws(p);
    const char *word=NULL;
    uint32_t size=0u;
    if (p->pos+4u<=p->len&&p->text[p->pos]=='t'){word="true";size=4u;*out=1;}
    else if (p->pos+5u<=p->len&&p->text[p->pos]=='f'){word="false";size=5u;*out=0;}
    else return mut_fail("TERRAX_VALIDATION_ERROR","expected a JSON boolean");
    for (uint32_t i=0;i<size;i++)if (p->text[p->pos+i]!=word[i])
        return mut_fail("TERRAX_PARSE_ERROR","malformed JSON boolean");
    p->pos+=size;
    return 1;
    }

static int jp_null(TxJsonParser *p){
    jp_ws(p);
    if (p->pos+4u>p->len)return 0;
    const char *word="null";
    for (uint32_t i=0;i<4u;i++)if (p->text[p->pos+i]!=word[i])return 0;
    p->pos+=4u;
    return 1;
    }

static int jp_integer(TxJsonParser *p,int64_t min_value,int64_t max_value,int64_t *out){
    jp_ws(p);
    if (p->pos>=p->len)return mut_fail("TERRAX_PARSE_ERROR","missing JSON integer");
    int negative=0;
    if (p->text[p->pos]=='-'){negative=1;p->pos++;}
    if (p->pos>=p->len||p->text[p->pos]<'0'||p->text[p->pos]>'9')
        return mut_fail("TERRAX_VALIDATION_ERROR","expected an integer");
    if (p->text[p->pos]=='0'&&p->pos+1u<p->len&&p->text[p->pos+1u]>='0'&&p->text[p->pos+1u]<='9')
        return mut_fail("TERRAX_PARSE_ERROR","JSON integers may not have leading zeroes");
    uint64_t magnitude=0u;
    while (p->pos<p->len&&p->text[p->pos]>='0'&&p->text[p->pos]<='9'){
        uint32_t digit=(uint32_t)(p->text[p->pos++]-'0');
        if (magnitude>(UINT64_MAX-digit)/10u)
            return mut_fail("TERRAX_VALIDATION_ERROR","integer overflow");
        magnitude=magnitude*10u+digit;
        }
    int64_t value;
    if (negative){
        if (magnitude>(uint64_t)INT64_MAX+1u)return mut_fail("TERRAX_VALIDATION_ERROR","integer overflow");
        value=magnitude==(uint64_t)INT64_MAX+1u?INT64_MIN:-(int64_t)magnitude;
        }
    else{
        if (magnitude>(uint64_t)INT64_MAX)return mut_fail("TERRAX_VALIDATION_ERROR","integer overflow");
        value=(int64_t)magnitude;
        }
    if (value<min_value||value>max_value)
        return mut_fail("TERRAX_VALIDATION_ERROR","integer is outside the allowed range");
    *out=value;
    return 1;
    }

static int decimal_u64(const char *text,uint32_t length,uint64_t max_value,uint64_t *out){
    if (!text||!length||!out)return mut_fail("TERRAX_VALIDATION_ERROR","expected a non-empty unsigned integer");
    uint64_t value=0u;
    for (uint32_t i=0;i<length;i++){
        if (text[i]<'0'||text[i]>'9')return mut_fail("TERRAX_VALIDATION_ERROR","expected an unsigned integer");
        uint32_t digit=(uint32_t)(text[i]-'0');
        if (value>(max_value-digit)/10u)return mut_fail("TERRAX_VALIDATION_ERROR","unsigned integer overflow");
        value=value*10u+digit;
        }
    *out=value;
    return 1;
    }

static int uuid_text_to_bytes(const char *text,uint32_t length,uint8_t *out){
    static const uint8_t order[16]={3u,2u,1u,0u,5u,4u,7u,6u,8u,9u,10u,11u,12u,13u,14u,15u};
    uint8_t display[16];
    uint32_t digit=0u;
    if (!text||!out||length!=36u)return mut_fail("TERRAX_VALIDATION_ERROR","uniqueId must be a canonical UUID");
    for (uint32_t i=0;i<36u;i++){
        if (i==8u||i==13u||i==18u||i==23u){
            if (text[i]!='-')return mut_fail("TERRAX_VALIDATION_ERROR","uniqueId must be a canonical UUID");
            continue;
            }
        uint32_t nibble=0u;
        if (!jp_hex(text[i],&nibble)||digit>=32u)return mut_fail("TERRAX_VALIDATION_ERROR","uniqueId must be a canonical UUID");
        if ((digit&1u)==0u)display[digit>>1u]=(uint8_t)(nibble<<4u);
        else display[digit>>1u]=(uint8_t)(display[digit>>1u]|nibble);
        digit++;
        }
    if (digit!=32u)return mut_fail("TERRAX_VALIDATION_ERROR","uniqueId must be a canonical UUID");
    for (uint32_t i=0;i<16u;i++)out[order[i]]=display[i];
    return 1;
    }

static int jp_member_next(TxJsonParser *p,int *first,int *done){
    jp_ws(p);
    if (p->pos<p->len&&p->text[p->pos]=='}'){p->pos++;*done=1;return 1;}
    if (!*first){if (!jp_take(p,','))return 0;}
    *first=0;
    *done=0;
    return 1;
    }

static int jp_array_next(TxJsonParser *p,int *first,int *done){
    jp_ws(p);
    if (p->pos<p->len&&p->text[p->pos]==']'){p->pos++;*done=1;return 1;}
    if (!*first){if (!jp_take(p,','))return 0;}
    *first=0;
    *done=0;
    return 1;
    }

static int jp_end(TxJsonParser *p){
    jp_ws(p);
    if (p->pos!=p->len)return mut_fail("TERRAX_PARSE_ERROR","trailing data after JSON value");
    return 1;
    }

static void buf_7bit(TxBuf *b,uint32_t value){
    do{
        uint8_t byte=(uint8_t)(value&0x7fu);
        value>>=7u;
        if (value)byte|=0x80u;
        buf_u8(b,byte);
        }while(value);
    }

static int jp_skip_string_token(TxJsonParser *p){
    if (!jp_take(p,'"'))return 0;
    while (p->pos<p->len){
        uint8_t c=(uint8_t)p->text[p->pos++];
        if (c=='"')return 1;
        if (c<0x20u)return mut_fail("TERRAX_PARSE_ERROR","unescaped control character in JSON string");
        if (c!='\\')continue;
        if (p->pos>=p->len)return mut_fail("TERRAX_PARSE_ERROR","unterminated JSON escape");
        c=(uint8_t)p->text[p->pos++];
        if (c=='"'||c=='\\'||c=='/'||c=='b'||c=='f'||c=='n'||c=='r'||c=='t')continue;
        if (c!='u'||p->pos+4u>p->len)return mut_fail("TERRAX_PARSE_ERROR","invalid JSON escape");
        for (uint32_t i=0;i<4u;i++){
            uint32_t nibble=0u;
            if (!jp_hex(p->text[p->pos++],&nibble))
                return mut_fail("TERRAX_PARSE_ERROR","invalid JSON unicode escape");
            }
        }
    return mut_fail("TERRAX_PARSE_ERROR","unterminated JSON string");
    }

static int jp_skip_number(TxJsonParser *p){
    jp_ws(p);
    uint32_t start=p->pos;
    if (p->pos<p->len&&p->text[p->pos]=='-')p->pos++;
    if (p->pos>=p->len)return mut_fail("TERRAX_PARSE_ERROR","truncated JSON number");
    if (p->text[p->pos]=='0')p->pos++;
    else{
        if (p->text[p->pos]<'1'||p->text[p->pos]>'9')
            return mut_fail("TERRAX_PARSE_ERROR","invalid JSON number");
        while (p->pos<p->len&&p->text[p->pos]>='0'&&p->text[p->pos]<='9')p->pos++;
        }
    if (p->pos<p->len&&p->text[p->pos]=='.'){
        p->pos++;
        if (p->pos>=p->len||p->text[p->pos]<'0'||p->text[p->pos]>'9')
            return mut_fail("TERRAX_PARSE_ERROR","invalid JSON fraction");
        while (p->pos<p->len&&p->text[p->pos]>='0'&&p->text[p->pos]<='9')p->pos++;
        }
    if (p->pos<p->len&&(p->text[p->pos]=='e'||p->text[p->pos]=='E')){
        p->pos++;
        if (p->pos<p->len&&(p->text[p->pos]=='+'||p->text[p->pos]=='-'))p->pos++;
        if (p->pos>=p->len||p->text[p->pos]<'0'||p->text[p->pos]>'9')
            return mut_fail("TERRAX_PARSE_ERROR","invalid JSON exponent");
        while (p->pos<p->len&&p->text[p->pos]>='0'&&p->text[p->pos]<='9')p->pos++;
        }
    return p->pos>start;
    }

static int jp_skip_value(TxJsonParser *p,uint32_t depth){
    if (depth>32u)return mut_fail("TERRAX_VALIDATION_ERROR","JSON nesting is too deep");
    jp_ws(p);
    if (p->pos>=p->len)return mut_fail("TERRAX_PARSE_ERROR","missing JSON value");
    char c=p->text[p->pos];
    if (c=='"')return jp_skip_string_token(p);
    if (c=='{'||c=='['){
        char close=c=='{'?'}':']';
        p->pos++;
        int first=1;
        for (;;){
            jp_ws(p);
            if (p->pos<p->len&&p->text[p->pos]==close){p->pos++;return 1;}
            if (!first&&!jp_take(p,','))return 0;
            first=0;
            if (c=='{'){
                if (!jp_skip_string_token(p)||!jp_take(p,':'))return 0;
                }
            if (!jp_skip_value(p,depth+1u))return 0;
            }
        }
    if (c=='t'||c=='f'){int value=0;return jp_bool(p,&value);}
    if (c=='n'){
        if (!jp_null(p))return mut_fail("TERRAX_PARSE_ERROR","malformed JSON null");
        return 1;
        }
    return jp_skip_number(p);
    }

static int patch_field_duplicate(TxPatchField *fields,uint32_t count,const char *name){
    for (uint32_t i=0;i<count;i++)if (tx_streq_c(fields[i].name,name))return 1;
    return 0;
    }

static int parse_patch_fields(TxJsonParser *p,TxPatchField *fields,uint32_t *field_count){
    int first=1,done=0,seen_patch=0;
    if (!jp_take(p,'{'))return 0;
    while (!done){
        char key[64];uint32_t key_len=0u;
        if (!jp_member_next(p,&first,&done))return 0;
        if (done)break;
        if (!jp_string(p,key,sizeof(key),&key_len)||!jp_take(p,':'))return 0;
        if (!tx_streq_c(key,"patch"))
            return mut_fail("TERRAX_VALIDATION_ERROR","header_patch accepts only the patch field");
        if (seen_patch)return mut_fail("TERRAX_VALIDATION_ERROR","duplicate patch field");
        seen_patch=1;
        if (!jp_take(p,'{'))return 0;
        int patch_first=1,patch_done=0;
        while (!patch_done){
            if (!jp_member_next(p,&patch_first,&patch_done))return 0;
            if (patch_done)break;
            if (*field_count>=TX_MAX_HEADER_PATCH_FIELDS)
                return mut_fail("TERRAX_VALIDATION_ERROR","too many header patch fields");
            TxPatchField *field=&fields[*field_count];
            if (!jp_string(p,field->name,sizeof(field->name),&field->name_len)||!jp_take(p,':'))return 0;
            if (patch_field_duplicate(fields,*field_count,field->name))
                return mut_fail("TERRAX_VALIDATION_ERROR","duplicate header patch field");
            jp_ws(p);
            field->value_start=p->pos;
            if (!jp_skip_value(p,0u))return 0;
            field->value_end=p->pos;
            field->used=0u;
            (*field_count)++;
            }
        }
    if (!seen_patch||*field_count==0u)
        return mut_fail("TERRAX_VALIDATION_ERROR","header patch must not be empty");
    return jp_end(p);
    }

static TxPatchField *patch_find(TxPatchField *fields,uint32_t count,const char *name){
    for (uint32_t i=0;i<count;i++)if (tx_streq_c(fields[i].name,name)){
        fields[i].used=1u;
        return &fields[i];
        }
    return NULL;
    }

static void patch_parser(const TxJsonParser *request,const TxPatchField *field,TxJsonParser *value){
    value->text=request->text+field->value_start;
    value->len=field->value_end-field->value_start;
    value->pos=0u;
    }

static int patch_bool(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                      const char *name,uint8_t current,uint8_t *out){
    TxPatchField *field=patch_find(fields,count,name);
    if (!field){*out=current;return 1;}
    TxJsonParser value;int parsed=0;
    patch_parser(request,field,&value);
    if (!jp_bool(&value,&parsed)||!jp_end(&value))return 0;
    *out=(uint8_t)parsed;
    return 1;
    }

static int patch_i64(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                     const char *name,int64_t current,int64_t min_value,int64_t max_value,int64_t *out){
    TxPatchField *field=patch_find(fields,count,name);
    if (!field){*out=current;return 1;}
    TxJsonParser value;
    patch_parser(request,field,&value);
    return jp_integer(&value,min_value,max_value,out)&&jp_end(&value);
    }

static int patch_u64(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                     const char *name,uint64_t current,uint64_t *out){
    TxPatchField *field=patch_find(fields,count,name);
    if (!field){*out=current;return 1;}
    TxJsonParser value;
    patch_parser(request,field,&value);
    jp_ws(&value);
    if (value.pos<value.len&&value.text[value.pos]=='"'){
        char decimal[32];uint32_t decimal_len=0u;
        if (!jp_string(&value,decimal,sizeof(decimal),&decimal_len)||
            !decimal_u64(decimal,decimal_len,UINT64_MAX,out))return 0;
        }
    else{
        uint32_t start=value.pos;
        if (value.pos<value.len&&value.text[value.pos]=='-')
            return mut_fail("TERRAX_VALIDATION_ERROR","expected an unsigned integer");
        while (value.pos<value.len&&value.text[value.pos]>='0'&&value.text[value.pos]<='9')value.pos++;
        if (value.pos==start||!decimal_u64(value.text+start,value.pos-start,UINT64_MAX,out))return 0;
        }
    return jp_end(&value);
    }

static int patch_double(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                        const char *name,double current,double *out){
    TxPatchField *field=patch_find(fields,count,name);
    if (!field){*out=current;return 1;}
    TxJsonParser value;patch_parser(request,field,&value);jp_ws(&value);
    int negative=0;if(value.pos<value.len&&value.text[value.pos]=='-'){negative=1;value.pos++;}
    double parsed=0.0;uint32_t digits=0u;
    while(value.pos<value.len&&value.text[value.pos]>='0'&&value.text[value.pos]<='9'){
        uint32_t digit=(uint32_t)(value.text[value.pos++]-'0');
        if(parsed>(DBL_MAX-(double)digit)/10.0)return mut_fail("TERRAX_VALIDATION_ERROR","floating point value is outside the supported range");
        parsed=parsed*10.0+(double)digit;digits++;
        }
    if(!digits)return mut_fail("TERRAX_VALIDATION_ERROR","expected a finite JSON number");
    if(value.pos<value.len&&value.text[value.pos]=='.'){
        value.pos++;double place=0.1;uint32_t fraction_digits=0u;
        while(value.pos<value.len&&value.text[value.pos]>='0'&&value.text[value.pos]<='9'){
            parsed+=(double)(value.text[value.pos++]-'0')*place;place*=0.1;fraction_digits++;
            }
        if(!fraction_digits)return mut_fail("TERRAX_VALIDATION_ERROR","expected digits after the decimal point");
        }
    int exponent=0;
    if(value.pos<value.len&&(value.text[value.pos]=='e'||value.text[value.pos]=='E')){
        value.pos++;int exponent_negative=0;
        if(value.pos<value.len&&(value.text[value.pos]=='+'||value.text[value.pos]=='-'))exponent_negative=value.text[value.pos++]=='-';
        uint32_t exponent_digits=0u;
        while(value.pos<value.len&&value.text[value.pos]>='0'&&value.text[value.pos]<='9'){
            if(exponent<1000)exponent=exponent*10+(value.text[value.pos]-'0');
            value.pos++;exponent_digits++;
            }
        if(!exponent_digits||exponent>400)return mut_fail("TERRAX_VALIDATION_ERROR","floating point exponent is outside the supported range");
        if(exponent_negative)exponent=-exponent;
        }
    while(exponent>0){if(parsed>DBL_MAX/10.0)return mut_fail("TERRAX_VALIDATION_ERROR","floating point value is outside the supported range");parsed*=10.0;exponent--;}
    while(exponent<0){parsed/=10.0;exponent++;}
    if(!jp_end(&value))return 0;
    *out=negative?-parsed:parsed;return 1;
    }

static int patch_string(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                        const char *name,const char *current,char *out,uint32_t cap,uint32_t *out_len){
    TxPatchField *field=patch_find(fields,count,name);
    if (!field){
        uint32_t length=tx_strlen(current);
        if (length>=cap)return mut_fail("TERRAX_STATE_ERROR","current header string exceeds the encoder limit");
        memcpy(out,current,length+1u);
        if (out_len)*out_len=length;
        return 1;
        }
    TxJsonParser value;
    patch_parser(request,field,&value);
    return jp_string(&value,out,cap,out_len)&&jp_end(&value);
    }

static void buf_i32le(TxBuf *b,int32_t value){buf_u32le(b,(uint32_t)value);}
static void buf_f32le(TxBuf *b,float value){union{float f;uint32_t u;}bits;bits.f=value;buf_u32le(b,bits.u);}
static void buf_f64le(TxBuf *b,double value){union{double f;uint64_t u;}bits;bits.f=value;buf_u64le(b,bits.u);}

static int write_bool_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                            const char *name,uint8_t current){
    uint8_t value=0u;if(!patch_bool(request,fields,count,name,current,&value))return 0;buf_u8(b,value);return b->ok;
    }
static int write_u8_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                          const char *name,uint8_t current){
    int64_t value=0;if(!patch_i64(request,fields,count,name,current,0,UINT8_MAX,&value))return 0;buf_u8(b,(uint8_t)value);return b->ok;
    }
static int write_u16_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                           const char *name,uint16_t current){
    int64_t value=0;if(!patch_i64(request,fields,count,name,current,0,UINT16_MAX,&value))return 0;buf_u16le(b,(uint16_t)value);return b->ok;
    }
static int write_u32_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                           const char *name,uint32_t current){
    int64_t value=0;if(!patch_i64(request,fields,count,name,current,0,UINT32_MAX,&value))return 0;buf_u32le(b,(uint32_t)value);return b->ok;
    }
static int write_i32_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                           const char *name,int32_t current,int32_t *out){
    int64_t value=0;if(!patch_i64(request,fields,count,name,current,INT32_MIN,INT32_MAX,&value))return 0;
    buf_i32le(b,(int32_t)value);if(out)*out=(int32_t)value;return b->ok;
    }
static int write_u64_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                           const char *name,uint64_t current){
    uint64_t value=0u;if(!patch_u64(request,fields,count,name,current,&value))return 0;buf_u64le(b,value);return b->ok;
    }
static int write_f32_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                           const char *name,float current){
    double value=0.0;if(!patch_double(request,fields,count,name,current,&value))return 0;
    if(value>3.402823466e38||value<-3.402823466e38)return mut_fail("TERRAX_VALIDATION_ERROR","float is outside the supported range");
    buf_f32le(b,(float)value);return b->ok;
    }
static int write_f64_field(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                           const char *name,double current){
    double value=0.0;if(!patch_double(request,fields,count,name,current,&value))return 0;buf_f64le(b,value);return b->ok;
    }

static int write_fixed_u32_array(TxBuf *b,TxJsonParser *request,TxPatchField *fields,uint32_t count,
                                 const char *name,const uint32_t *current,uint32_t expected){
    TxPatchField *field=patch_find(fields,count,name);
    if (!field){for(uint32_t i=0;i<expected;i++)buf_u32le(b,current[i]);return b->ok;}
    TxJsonParser value;patch_parser(request,field,&value);
    if(!jp_take(&value,'['))return 0;
    int first=1,done=0;uint32_t index=0u;
    while(!done){
        if(!jp_array_next(&value,&first,&done))return 0;
        if(done)break;
        if(index>=expected)return mut_fail("TERRAX_VALIDATION_ERROR","fixed header array has too many values");
        int64_t item=0;if(!jp_integer(&value,0,UINT32_MAX,&item))return 0;
        buf_u32le(b,(uint32_t)item);index++;
        }
    if(index!=expected)return mut_fail("TERRAX_VALIDATION_ERROR","fixed header array has the wrong length");
    return jp_end(&value)&&b->ok;
    }

static void header_source_view(TxWorld *w,const uint8_t **data,uint32_t *len,uint32_t *base){
    if(w->section_overrides[0].active){*data=w->section_overrides[0].data;*len=w->section_overrides[0].len;*base=w->starts[0];}
    else{*data=w->file;*len=w->file_len;*base=0u;}
    }

static int patch_expected_count(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                                const char *name,uint32_t actual,uint32_t max_value){
    TxPatchField *field=patch_find(fields,count,name);
    if(!field)return 1;
    TxJsonParser value;int64_t expected=0;patch_parser(request,field,&value);
    if(!jp_integer(&value,0,max_value,&expected)||!jp_end(&value))return 0;
    if((uint32_t)expected!=actual)return mut_fail("TERRAX_VALIDATION_ERROR","dynamic header count does not match its array");
    return 1;
    }

static int encode_string_array(TxBuf *header,TxWorld *w,TxJsonParser *request,
                               TxPatchField *fields,uint32_t count){
    TxPatchField *field=patch_find(fields,count,"anglerWhoFinishedToday");
    TxBuf values={0};uint32_t actual=0u;
    if(field){
        buf_init(&values,256u);if(!values.ok)return 0;
        TxJsonParser value;patch_parser(request,field,&value);
        if(!jp_take(&value,'[')){tx_internal_free(values.data);return 0;}
        int first=1,done=0;
        while(!done){
            if(!jp_array_next(&value,&first,&done)){tx_internal_free(values.data);return 0;}
            if(done)break;
            char item[TX_MUTATOR_MAX_STRING_BYTES+1u];uint32_t length=0u;
            if(!jp_string(&value,item,sizeof(item),&length)){tx_internal_free(values.data);return 0;}
            buf_7bit(&values,length);buf_bytes(&values,item,length);actual++;
            if(actual>65535u){tx_internal_free(values.data);return mut_fail("TERRAX_VALIDATION_ERROR","angler array is too large");}
            }
        if(!jp_end(&value)||!values.ok){tx_internal_free(values.data);return 0;}
        }else{
        const uint8_t *source;uint32_t source_len,base;header_source_view(w,&source,&source_len,&base);
        uint32_t offset=w->anglersOff-base;actual=w->anglerFinishedSize;
        for(uint32_t i=0;i<actual;i++){
            int ok=0;uint32_t length=rd_7bit(source,source_len,&offset,&ok);
            if(!ok||offset>source_len||length>source_len-offset){tx_internal_free(values.data);return mut_fail("TERRAX_STATE_ERROR","angler data is outside the active header");}
            if(!values.data)buf_init(&values,length+16u);
            buf_7bit(&values,length);buf_bytes(&values,source+offset,length);offset+=length;
            }
        }
    if(!patch_expected_count(request,fields,count,"anglerWhoFinishedTodayCount",actual,UINT32_MAX)){tx_internal_free(values.data);return 0;}
    buf_u32le(header,actual);if(values.len)buf_bytes(header,values.data,values.len);tx_internal_free(values.data);return header->ok;
    }

static int encode_numeric_array(TxBuf *header,TxWorld *w,TxJsonParser *request,
                                TxPatchField *fields,uint32_t count,const char *array_name,
                                const char *count_name,uint32_t current_count,uint32_t absolute_offset,
                                uint32_t width,int signed_values,uint32_t max_count){
    TxPatchField *field=patch_find(fields,count,array_name);
    TxBuf values={0};uint32_t actual=0u;
    if(field){
        buf_init(&values,64u);if(!values.ok)return 0;
        TxJsonParser value;patch_parser(request,field,&value);
        if(!jp_take(&value,'[')){tx_internal_free(values.data);return 0;}
        int first=1,done=0;
        while(!done){
            if(!jp_array_next(&value,&first,&done)){tx_internal_free(values.data);return 0;}
            if(done)break;
            int64_t item=0;
            int64_t min_value=signed_values?(width==2u?INT16_MIN:INT32_MIN):0;
            int64_t max_value=signed_values?(width==2u?INT16_MAX:INT32_MAX):(width==2u?UINT16_MAX:UINT32_MAX);
            if(!jp_integer(&value,min_value,max_value,&item)){tx_internal_free(values.data);return 0;}
            if(width==2u)buf_u16le(&values,(uint16_t)item);else buf_u32le(&values,(uint32_t)item);
            actual++;
            if(actual>max_count){tx_internal_free(values.data);return mut_fail("TERRAX_VALIDATION_ERROR","dynamic header array is too large");}
            }
        if(!jp_end(&value)||!values.ok){tx_internal_free(values.data);return 0;}
        }else{
        const uint8_t *source;uint32_t source_len,base;header_source_view(w,&source,&source_len,&base);
        uint32_t offset=absolute_offset-base;uint64_t bytes=(uint64_t)current_count*width;
        if(offset>source_len||bytes>source_len-offset)return mut_fail("TERRAX_STATE_ERROR","dynamic array is outside the active header");
        actual=current_count;if(bytes){buf_init(&values,(uint32_t)bytes);buf_bytes(&values,source+offset,(uint32_t)bytes);}
        }
    if(!patch_expected_count(request,fields,count,count_name,actual,max_count)){tx_internal_free(values.data);return 0;}
    if(max_count==UINT16_MAX)buf_u16le(header,actual);else buf_u32le(header,actual);
    if(values.len)buf_bytes(header,values.data,values.len);tx_internal_free(values.data);return header->ok;
    }

static int encode_spawn_points(TxBuf *header,TxWorld *w,TxJsonParser *request,
                               TxPatchField *fields,uint32_t count){
    TxPatchField *field=patch_find(fields,count,"spawnPoints");
    TxBuf values={0};uint32_t actual=0u;
    if(field){
        buf_init(&values,32u);if(!values.ok)return 0;
        TxJsonParser value;patch_parser(request,field,&value);
        if(!jp_take(&value,'[')){tx_internal_free(values.data);return 0;}
        int first=1,done=0;
        while(!done){
            if(!jp_array_next(&value,&first,&done)){tx_internal_free(values.data);return 0;}
            if(done)break;
            if(!jp_take(&value,'{')){tx_internal_free(values.data);return 0;}
            int object_first=1,object_done=0;uint32_t seen=0u;int64_t x=0,y=0;
            while(!object_done){
                char name[8];uint32_t name_len=0u;
                if(!jp_member_next(&value,&object_first,&object_done)){tx_internal_free(values.data);return 0;}
                if(object_done)break;
                if(!jp_string(&value,name,sizeof(name),&name_len)||!jp_take(&value,':')){tx_internal_free(values.data);return 0;}
                uint32_t bit=0u;
                if(tx_streq_c(name,"x")){bit=1u;if(!jp_integer(&value,INT16_MIN,INT16_MAX,&x)){tx_internal_free(values.data);return 0;}}
                else if(tx_streq_c(name,"y")){bit=2u;if(!jp_integer(&value,INT16_MIN,INT16_MAX,&y)){tx_internal_free(values.data);return 0;}}
                else{tx_internal_free(values.data);return mut_fail("TERRAX_VALIDATION_ERROR","unknown spawn point field");}
                if(seen&bit){tx_internal_free(values.data);return mut_fail("TERRAX_VALIDATION_ERROR","duplicate spawn point field");}
                seen|=bit;
                }
            if(seen!=3u){tx_internal_free(values.data);return mut_fail("TERRAX_VALIDATION_ERROR","spawn point requires x and y");}
            buf_u16le(&values,(uint16_t)x);buf_u16le(&values,(uint16_t)y);actual++;
            if(actual>UINT8_MAX){tx_internal_free(values.data);return mut_fail("TERRAX_VALIDATION_ERROR","spawn point array is too large");}
            }
        if(!jp_end(&value)||!values.ok){tx_internal_free(values.data);return 0;}
        }else{
        const uint8_t *source;uint32_t source_len,base;header_source_view(w,&source,&source_len,&base);
        uint32_t offset=w->extradSpawnPointManagerOff-base;actual=w->numExtradSpawnPointManager;uint32_t bytes=actual*4u;
        if(offset>source_len||bytes>source_len-offset)return mut_fail("TERRAX_STATE_ERROR","spawn point data is outside the active header");
        if(bytes){buf_init(&values,bytes);buf_bytes(&values,source+offset,bytes);}
        }
    if(!patch_expected_count(request,fields,count,"spawnPointCount",actual,UINT8_MAX)){tx_internal_free(values.data);return 0;}
    buf_u8(header,(uint8_t)actual);if(values.len)buf_bytes(header,values.data,values.len);tx_internal_free(values.data);return header->ok;
    }

static int encode_manifest(TxBuf *header,TxWorld *w,TxJsonParser *request,
                           TxPatchField *fields,uint32_t count){
    TxPatchField *field=patch_find(fields,count,"manifestJson");
    if(field){
        uint32_t cap=field->value_end-field->value_start+1u;char *text=(char*)tx_alloc(cap);uint32_t length=0u;
        if(!text)return mut_fail("TERRAX_WASM_OOM","failed to allocate manifest string");
        TxJsonParser value;patch_parser(request,field,&value);int ok=jp_string(&value,text,cap,&length)&&jp_end(&value);
        if(ok){buf_7bit(header,length);buf_bytes(header,text,length);}tx_internal_free(text);return ok&&header->ok;
        }
    const uint8_t *source;uint32_t source_len,base;header_source_view(w,&source,&source_len,&base);
    uint32_t offset=w->maniFestOff-base;int ok=0;uint32_t length=rd_7bit(source,source_len,&offset,&ok);
    if(!ok||offset>source_len||length>source_len-offset)return mut_fail("TERRAX_STATE_ERROR","manifest is outside the active header");
    buf_7bit(header,length);buf_bytes(header,source+offset,length);return header->ok;
    }

static int header_versions_compatible(uint32_t current,uint32_t next){
    static const uint16_t gates[]={95u,99u,101u,104u,107u,108u,109u,112u,113u,118u,128u,129u,131u,135u,140u,141u,170u,174u,178u,179u,180u,181u,195u,196u,201u,204u,207u,208u,209u,211u,212u,215u,216u,217u,222u,223u,227u,238u,239u,240u,241u,249u,250u,251u,257u,259u,260u,261u,264u,266u,267u,284u,287u,288u,289u,291u,296u,297u,299u,302u,304u,313u,323u};
    /* A header-only patch cannot migrate the payload of another section. */
    static const uint16_t section_gates[]={116u,122u,189u,190u,210u,213u,220u,268u,294u,307u,308u,311u,312u,315u};
    if(next<88u||next>326u)return 0;
    for(uint32_t i=0;i<sizeof(gates)/sizeof(gates[0]);i++)if((current<gates[i])!=(next<gates[i]))return 0;
    for(uint32_t i=0;i<sizeof(section_gates)/sizeof(section_gates[0]);i++)if((current<section_gates[i])!=(next<section_gates[i]))return 0;
    return 1;
    }

static int encode_header_model(TxWorld *w,TxJsonParser *request,TxPatchField *fields,
                               uint32_t field_count,uint32_t version,TxBuf *header){
#define WB(name,member) do{if(!write_bool_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
#define WU8(name,member) do{if(!write_u8_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
#define WU16(name,member) do{if(!write_u16_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
#define WU32(name,member) do{if(!write_u32_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
#define WI32(name,member) do{if(!write_i32_field(header,request,fields,field_count,name,w->member,NULL))return 0;}while(0)
#define WU64(name,member) do{if(!write_u64_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
#define WF32(name,member) do{if(!write_f32_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
#define WF64(name,member) do{if(!write_f64_field(header,request,fields,field_count,name,w->member))return 0;}while(0)
    char text[TX_MAX_NAME];uint32_t length=0u;
    if(!patch_string(request,fields,field_count,"worldName",w->worldName,text,sizeof(text),&length))return 0;
    buf_7bit(header,length);buf_bytes(header,text,length);
    if(version>=179u){
        if(!patch_string(request,fields,field_count,"seed",w->seed,text,sizeof(text),&length))return 0;
        if(version==179u){uint64_t seed=0u;if(!decimal_u64(text,length,UINT32_MAX,&seed))return 0;buf_u32le(header,(uint32_t)seed);}
        else{buf_7bit(header,length);buf_bytes(header,text,length);}
        WU64("worldGeneratorVersion",worldGeneratorVersion);
        }
    if(version>=181u){
        if(!patch_string(request,fields,field_count,"uniqueId",w->uuid,text,sizeof(text),&length))return 0;
        uint8_t uuid[16];if(!uuid_text_to_bytes(text,length,uuid))return 0;buf_bytes(header,uuid,16u);
        }
    WI32("worldId",worldId);WI32("leftWorld",leftWorld);WI32("rightWorld",rightWorld);WI32("topWorld",topWorld);WI32("bottomWorld",bottomWorld);
    int32_t max_y=0,max_x=0,spawn_x=0,spawn_y=0;
    if(!write_i32_field(header,request,fields,field_count,"maxTilesY",w->maxTilesY,&max_y)||
       !write_i32_field(header,request,fields,field_count,"maxTilesX",w->maxTilesX,&max_x))return 0;
    if(max_x<=0||max_y<=0)return mut_fail("TERRAX_VALIDATION_ERROR","world dimensions must be positive");
    if(version>=209u){
        int32_t game_mode=0;if(!write_i32_field(header,request,fields,field_count,"gameMode",w->gameMode,&game_mode))return 0;
        if(game_mode<0||game_mode>3)return mut_fail("TERRAX_VALIDATION_ERROR","gameMode must be between 0 and 3");
        if(version>=222u)WB("drunkWorld",drunkWorld);if(version>=227u)WB("getGoodWorld",ftwWorld);
        if(version>=238u)WB("tenthAnniversaryWorld",tenthAnniversaryWorld);if(version>=239u)WB("dontStarveWorld",dontStarveWorld);
        if(version>=241u)WB("notTheBeesWorld",notTheBeesWorld);if(version>=249u)WB("remixWorld",remixWorld);
        if(version>=266u)WB("noTrapsWorld",noTrapsWorld);if(version>=267u)WB("zenithWorld",zenithWorld);
        if(version>=302u)WB("skyblockWorld",skyblockWorld);
        }else if(version==208u){int64_t mode=0;if(!patch_i64(request,fields,field_count,"gameMode",w->gameMode,0,2,&mode))return 0;buf_u8(header,mode==1?1u:0u);buf_u8(header,mode==2?1u:0u);}
    else if(version>=112u){int64_t mode=0;if(!patch_i64(request,fields,field_count,"gameMode",w->gameMode,0,1,&mode))return 0;buf_u8(header,(uint8_t)mode);}
    if(version>=141u)WU64("creationTime",creationTime);if(version>=284u)WU64("lastPlayed",lastPlayed);
    WU8("moonType",moonType);
    if(!write_fixed_u32_array(header,request,fields,field_count,"treeX",w->treeX,3u)||
       !write_fixed_u32_array(header,request,fields,field_count,"treeStyle",w->treeStyle,4u)||
       !write_fixed_u32_array(header,request,fields,field_count,"caveBackX",w->caveBackX,3u)||
       !write_fixed_u32_array(header,request,fields,field_count,"caveBackStyle",w->caveBackStyle,4u))return 0;
    WU32("iceBackStyle",iceBackStyle);WU32("jungleBackStyle",jungleBackStyle);WU32("hellBackStyle",hellBackStyle);
    if(!write_i32_field(header,request,fields,field_count,"spawnTileX",w->spawnTileX,&spawn_x)||
       !write_i32_field(header,request,fields,field_count,"spawnTileY",w->spawnTileY,&spawn_y))return 0;
    if(spawn_x<0||spawn_x>=max_x||spawn_y<0||spawn_y>=max_y)return mut_fail("TERRAX_VALIDATION_ERROR","spawn coordinates are outside the world bounds");
    WF64("worldSurface",worldSurface);WF64("rockLayer",rockLayer);WF64("time",gameTime);
    WB("dayTime",isDayTime);WU32("moonPhase",moonPhase);WB("bloodMoon",isBloodMoon);WB("eclipse",isEclipse);
    WI32("dungeonX",dungeonX);WI32("dungeonY",dungeonY);WB("crimson",isCrimson);
    WB("downedEyeOfCthulhu",downedEye);WB("downedEaterOfWorldsOrBrainOfCthulhu",downedEaterBrain);
    WB("downedSkeletron",downedSkeletron);WB("downedQueenBee",downedQueenBee);WB("downedDestroyer",downedDestroyer);
    WB("downedTwins",downedTwins);WB("downedSkeletronPrime",downedSkeletronPrime);WB("downedAnyMechBoss",downedAnyMech);
    WB("downedPlantera",downedPlantera);WB("downedGolem",downedGolem);if(version>=118u)WB("downedKingSlime",downedKingSlime);
    WB("savedGoblin",savedGoblin);WB("savedWizard",savedWizard);WB("savedMech",savedMech);WB("downedGoblins",downedGoblins);
    WB("downedClown",downedClown);WB("downedFrost",downedFrost);WB("downedPirates",downedPirates);
    WB("shadowOrbSmashed",shadowOrbSmashed);WB("spawnMeteor",spawnMeteor);WU8("shadowOrbCount",shadowOrbCount);
    WU32("altarCount",altarCount);WB("hardMode",hardMode);if(version>=257u)WB("afterPartyOfDoom",afterPartyOfDoom);
    WU32("invasionDelay",invasionDelay);WU32("invasionSize",invasionSize);WU32("invasionType",invasionType);WF64("invasionX",invasionX);
    if(version>=118u)WF64("slimeRainTime",slimeRainTime);if(version>=113u)WU8("sundialCooldown",sundialCooldown);
    WB("raining",isRaining);WU32("rainTime",rainTime);WF32("maxRain",maxRain);
    WI32("oreTierCobalt",oreTierCobalt);WI32("oreTierMythril",oreTierMythril);WI32("oreTierAdamantite",oreTierAdamantite);
    WU8("treeBG1",bgTree);WU8("corruptBG",bgCorruption);WU8("jungleBG",bgJungle);WU8("snowBG",bgSnow);
    WU8("hallowBG",bgHallow);WU8("crimsonBG",bgCrimson);WU8("desertBG",bgDesert);WU8("oceanBG",bgOcean);
    WI32("cloudBGActive",cloudBgActive);WU16("numClouds",numClouds);WF32("windSpeedTarget",windSpeedSet);
    if(version>=95u&&!encode_string_array(header,w,request,fields,field_count))return 0;
    if(version>=99u)WB("savedAngler",savedAngler);if(version>=101u)WU32("anglerQuest",anglerQuest);
    if(version>=104u)WB("savedStylist",savedStylist);if(version>=129u)WB("savedTaxCollector",savedTaxCollector);
    if(version>=201u)WB("savedGolfer",savedGolfer);if(version>=107u)WU32("invasionSizeStart",invasionSizeStart);
    if(version>=108u)WU32("cultistDelay",cultistDelay);
    if(version>=109u){
        if(!encode_numeric_array(header,w,request,fields,field_count,"killCount","killCountLength",w->numMobs,w->mobsOff,4u,0,UINT16_MAX))return 0;
        if(w->claimableBannersPresent&&
           !encode_numeric_array(header,w,request,fields,field_count,"claimableBanners","claimableBannersLength",w->numClaimableBanners,w->claimableBannersOff,2u,0,UINT16_MAX))return 0;
        }
    if(version>=128u){
        WB("fastForwardTimeToDawn",fastForwardTime);
        if(version>=131u){
            WB("downedFishron",downedFishron);
            WB("downedMartians",downedMartians);WB("downedAncientCultist",downedLunaticCultist);WB("downedMoonlord",downedMoonlord);
            WB("downedHalloweenKing",downedHalloweenKing);WB("downedHalloweenTree",downedHalloweenTree);
            WB("downedChristmasIceQueen",downedChristmasIceQueen);WB("downedChristmasSantank",downedSanta);WB("downedChristmasTree",downedChristmasTree);
            }
        }
    if(version>=140u){
        WB("downedTowerSolar",downedCelestialSolar);WB("downedTowerVortex",downedCelestialVortex);
        WB("downedTowerNebula",downedCelestialNebula);WB("downedTowerStardust",downedCelestialStardust);
        WB("towerActiveSolar",downedTowerSolar);WB("towerActiveVortex",downedTowerVortex);
        WB("towerActiveNebula",downedTowerNebula);WB("towerActiveStardust",downedTowerStardust);WB("lunarApocalypseIsUp",downedTowerAncient);
        }
    if(version>=170u){
        WB("partyManual",partyManual);WB("partyGenuine",partyGenuine);WU32("partyCooldown",partyCooldown);
        if(!encode_numeric_array(header,w,request,fields,field_count,"partyCelebratingNpcNetIds","partyCelebratingNpcCount",w->partyCelebratingNPCSize,w->partyCelebratingNPCsOff,4u,1,UINT32_MAX))return 0;
        }
    if(version>=174u){WB("sandstormHappening",sandstormHappening);WU32("sandstormTimeLeft",sandStormTime);WF32("sandstormSeverity",sandStormSeverity);WF32("sandstormIntendedSeverity",sandstormIntendedSeverity);}
    if(version>=178u){WB("savedBartender",savedBartender);WB("dd2DownedT1",downedInvasionT1);WB("dd2DownedT2",downedInvasionT2);WB("dd2DownedT3",downedInvasionT3);}
    if(version>194u)WU8("mushroomBG",mushroomBg);if(version>=215u)WU8("underworldBG",undergroundDesertBg);
    if(version>195u){WU8("treeBG2",bgTree2);WU8("treeBG3",bgTree3);WU8("treeBG4",bgTree4);}
    if(version>=204u)WB("combatBookWasUsed",combatBookUsed);
    if(version>=207u){WU32("lanternNightCooldown",lanternNightCooldown);WB("lanternNightGenuine",lanternNightGenuine);WB("lanternNightManual",lanternNightManual);WB("lanternNightNextNightIsGenuine",lanternNightNextNightIsGenuine);}
    if(version>=211u&&!encode_numeric_array(header,w,request,fields,field_count,"treeTopVariations","treeTopVariationCount",w->treetopSize,w->treeTopVariationsOff,4u,1,UINT32_MAX))return 0;
    if(version>=212u){WB("forceHalloweenForToday",forceHalloweenForToday);WB("forceXMasForToday",forceXMasForToday);}
    if(version>=216u){WU32("oreTierCopper",savedOreTiersCopper);WU32("oreTierIron",savedOreTiersIron);WU32("oreTierSilver",savedOreTiersSilver);WU32("oreTierGold",savedOreTiersGold);}
    if(version>=217u){WB("boughtCat",boughtCat);WB("boughtDog",boughtDog);WB("boughtBunny",boughtBunny);}
    if(version>=223u){WB("downedEmpressOfLight",downedEmpressOfLight);WB("downedQueenSlime",downedQueenSlime);}
    if(version>=240u)WB("downedDeerclops",downedDeerclops);if(version>=250u)WB("unlockedSlimeBlueSpawn",unlockedSlimeBlueSpawn);
    if(version>=251u){
        WB("unlockedMerchantSpawn",unlockedMerchantSpawn);WB("unlockedDemolitionistSpawn",unlockedDemolitionistSpawn);
        WB("unlockedPartyGirlSpawn",unlockedPartyGirlSpawn);WB("unlockedDyeTraderSpawn",unlockedDyeTraderSpawn);
        WB("unlockedTruffleSpawn",unlockedTruffleSpawn);WB("unlockedArmsDealerSpawn",unlockedArmsDealerSpawn);
        WB("unlockedNurseSpawn",unlockedNurseSpawn);WB("unlockedPrincessSpawn",unlockedPrincessSpawn);
        }
    if(version>=259u)WB("combatBookVolumeTwoWasUsed",combatBookVolumeTwoWasUsed);if(version>=260u)WB("peddlersSatchelWasUsed",peddlersSatchelWasUsed);
    if(version>=261u){
        WB("unlockedSlimeGreenSpawn",unlockedSlimeGreenSpawn);WB("unlockedSlimeOldSpawn",unlockedSlimeOldSpawn);
        WB("unlockedSlimePurpleSpawn",unlockedSlimePurpleSpawn);WB("unlockedSlimeRainbowSpawn",unlockedSlimeRainbowSpawn);
        WB("unlockedSlimeRedSpawn",unlockedSlimeRedSpawn);WB("unlockedSlimeYellowSpawn",unlockedSlimeYellowSpawn);WB("unlockedSlimeCopperSpawn",unlockedSlimeCopperSpawn);
        }
    if(version>=264u){WB("fastForwardTimeToDusk",fastForwardTimeToDusk);WU8("moondialCooldown",moondialCooldown);}
    if(version>=287u){WB("forceHalloweenForever",forceHalloweenForever);WB("forceXMasForever",forcexmasForever);}
    if(version>=288u)WB("vampireSeed",vampireSeed);if(version>=296u)WB("infectedSeed",infectedSeed);
    if(version>=291u){WU32("meteorShowerCount",tempmeteorShowerCount);WU32("coinRain",tempcoinRain);}
    if(version>=297u){WB("teamBasedSpawnsSeed",teambasedSpawnsSeed);if(!encode_spawn_points(header,w,request,fields,field_count))return 0;}
    if(version>=304u)WB("dualDungeonsSeed",dualdungeonsSeed);if(version>=299u&&version<313u)WU32("legacySkip",legacySkip);
    if(version>=323u){WB("moreLightningSeed",moreLightningSeed);WB("noLightningSeed",noLightningSeed);}
    if(version>=299u&&!encode_manifest(header,w,request,fields,field_count))return 0;
#undef WB
#undef WU8
#undef WU16
#undef WU32
#undef WI32
#undef WU64
#undef WF32
#undef WF64
    return header->ok;
    }

static int parse_format_bitmap(TxJsonParser *request,TxPatchField *fields,uint32_t count,
                               uint16_t tile_type_count,const uint8_t *current,uint32_t current_len,
                               TxBuf *bitmap,int *changed){
    TxPatchField *field=patch_find(fields,count,"tileFrameImportantBitmap");
    if(!field){*changed=0;return 1;}
    uint32_t byte_count=((uint32_t)tile_type_count+7u)/8u;buf_init(bitmap,byte_count?byte_count:1u);
    if(!bitmap->ok)return 0;for(uint32_t i=0;i<byte_count;i++)buf_u8(bitmap,0u);
    TxJsonParser value;patch_parser(request,field,&value);if(!jp_take(&value,'['))return 0;
    int first=1,done=0;uint32_t index=0u;
    while(!done){
        if(!jp_array_next(&value,&first,&done))return 0;if(done)break;
        if(index>=tile_type_count)return mut_fail("TERRAX_VALIDATION_ERROR","tile bitmap has too many values");
        int bit=0;if(!jp_bool(&value,&bit))return 0;if(bit)bitmap->data[index>>3u]|=(uint8_t)(1u<<(index&7u));index++;
        }
    if(index!=tile_type_count)return mut_fail("TERRAX_VALIDATION_ERROR","tile bitmap length must equal tileTypeCount");
    if(!jp_end(&value))return 0;*changed=bitmap->len!=current_len;
    if(!*changed)for(uint32_t i=0;i<bitmap->len;i++)if(bitmap->data[i]!=current[i]){*changed=1;break;}
    return 1;
    }

static void refresh_format_positions(TxWorld *w){
    uint32_t pointer_table_start=w->version>=135u?24u:4u;
    uint32_t position=pointer_table_start+2u+(uint32_t)w->pointer_count*4u+2u+w->important_len;
    for(uint32_t i=0;i<w->pointer_count&&i<TX_MAX_SECTIONS;i++){
        w->positions[i]=position;
        if(i<TX_MAX_SECTION_OVERRIDES&&w->section_overrides[i].active)
            position+=w->section_overrides[i].len;
        else position+=w->ends[i]-w->starts[i];
        }
    }

int tx_mutate_header_patch(TxWorld *w,const char *request_text,uint32_t request_len,TxBuf *response){
    if (!tx_world_require_writable(w)) return -1;
    if(!w||!request_text||request_len==0u||request_len>TX_MUTATOR_MAX_JSON_BYTES)return mut_error("TERRAX_INVALID_ARGUMENT","invalid header_patch request");
    if(w->legacy_wld)return mut_error("TERRAX_NOT_SUPPORTED","pre-88 worlds support reading and original-byte export only");
    if(w->pointer_count<1u)return mut_error("TERRAX_NOT_SUPPORTED","world has no header section");
    TxJsonParser request={request_text,request_len,0u};TxPatchField fields[TX_MAX_HEADER_PATCH_FIELDS];uint32_t field_count=0u;
    if(!parse_patch_fields(&request,fields,&field_count))return -1;
    int64_t version_value=w->version,type_value=w->file_type,revision_value=w->revision,tile_count_value=w->tile_type_count;uint64_t favorite=w->favorite;
    if(!patch_i64(&request,fields,field_count,"version",w->version,88,326,&version_value)||
       !patch_i64(&request,fields,field_count,"type",w->file_type,0,UINT8_MAX,&type_value)||
       !patch_i64(&request,fields,field_count,"revision",w->revision,0,UINT32_MAX,&revision_value)||
       !patch_u64(&request,fields,field_count,"favoriteFlags",w->favorite,&favorite)||
       !patch_i64(&request,fields,field_count,"tileTypeCount",w->tile_type_count,0,UINT16_MAX,&tile_count_value))return -1;
    uint32_t next_version=(uint32_t)version_value;
    if(next_version>=135u&&type_value!=2)return mut_error("TERRAX_VALIDATION_ERROR","WLD metadata must use world file type 2");
    if(!header_versions_compatible(w->version,next_version))return mut_error("TERRAX_NOT_SUPPORTED","version change crosses an unsupported header layout boundary");
    char magic[8];uint32_t magic_len=0u;
    if(!patch_string(&request,fields,field_count,"magic",w->magic[0]?w->magic:"relogic",magic,sizeof(magic),&magic_len))return -1;
    if(next_version>=135u&&(!tx_streq_c(magic,"relogic")&&!tx_streq_c(magic,"xindong")))return mut_error("TERRAX_VALIDATION_ERROR","magic must be relogic or xindong");
    TxBuf bitmap={0};int bitmap_changed=0;
    if(!parse_format_bitmap(&request,fields,field_count,(uint16_t)tile_count_value,w->important,w->important_len,&bitmap,&bitmap_changed)){tx_internal_free(bitmap.data);return -1;}
    if((uint16_t)tile_count_value!=w->tile_type_count&&!patch_find(fields,field_count,"tileFrameImportantBitmap")){
        tx_internal_free(bitmap.data);return mut_error("TERRAX_VALIDATION_ERROR","changing tileTypeCount requires tileFrameImportantBitmap");
        }
    uint32_t source_len=w->section_overrides[0].active?w->section_overrides[0].len:w->ends[0]-w->starts[0];
    TxBuf encoded;buf_init(&encoded,source_len+4096u);
    if(!encoded.ok){tx_internal_free(bitmap.data);return mut_error("TERRAX_WASM_OOM","failed to allocate header encoder");}
    if(!encode_header_model(w,&request,fields,field_count,next_version,&encoded)){tx_internal_free(encoded.data);tx_internal_free(bitmap.data);return -1;}
    for(uint32_t i=0;i<field_count;i++)if(!fields[i].used){
        tx_internal_free(encoded.data);tx_internal_free(bitmap.data);return mut_error("TERRAX_NOT_SUPPORTED","header or format field is not writable in this WLD version");
        }
    TxWorld candidate=*w;candidate.version=next_version;candidate.section_overrides[0].active=1u;
    candidate.section_overrides[0].data=encoded.data;candidate.section_overrides[0].len=encoded.len;
    if(!parse_header(&candidate)){tx_internal_free(encoded.data);tx_internal_free(bitmap.data);return -1;}
    TxBuf footer={0};
    uint32_t footer_index=next_version>=220u?10u:next_version>=210u?9u:next_version>=189u?8u:next_version>=170u?7u:next_version>=116u?6u:5u;
    if(candidate.worldId!=w->worldId||!tx_streq_c(candidate.worldName,w->worldName)){
        if(footer_index>=w->pointer_count){
            tx_internal_free(encoded.data);tx_internal_free(bitmap.data);
            return mut_error("TERRAX_NOT_SUPPORTED","world identity patch requires a footer section");
            }
        uint32_t name_len=tx_strlen(candidate.worldName);
        buf_init(&footer,name_len+10u);buf_u8(&footer,1u);buf_7bit(&footer,name_len);
        buf_bytes(&footer,candidate.worldName,name_len);buf_u32le(&footer,(uint32_t)candidate.worldId);
        if(!footer.ok){
            tx_internal_free(footer.data);tx_internal_free(encoded.data);tx_internal_free(bitmap.data);
            return mut_error("TERRAX_WASM_OOM","failed to allocate matching world footer");
            }
        }
    buf_cstr(response,"{\"status\":\"ok\",\"updated\":");json_u32(response,field_count);buf_u8(response,'}');
    int result=set_result_buf(response);if(result<0){tx_internal_free(footer.data);tx_internal_free(encoded.data);tx_internal_free(bitmap.data);return -1;}
    if(!set_section_override_data(w,0,encoded.data,encoded.len)){discard_response(response);tx_internal_free(footer.data);tx_internal_free(encoded.data);tx_internal_free(bitmap.data);return -1;}
    /* Both indices are already validated; publishing these owned buffers cannot allocate. */
    if(footer.data)set_section_override_data(w,(int)footer_index,footer.data,footer.len);
    w->version=next_version;memset(w->magic,0,sizeof(w->magic));memcpy(w->magic,magic,magic_len);
    w->file_type=(uint8_t)type_value;w->revision=(uint32_t)revision_value;w->favorite=favorite;w->tile_type_count=(uint16_t)tile_count_value;
    if(bitmap.data&&bitmap_changed){
        if(w->important_override)tx_internal_free(w->important_override);
        w->important_override=bitmap.data;w->important=bitmap.data;w->important_len=bitmap.len;bitmap.data=NULL;
        }
    tx_internal_free(bitmap.data);w->format_dirty=1u;refresh_format_positions(w);
    if(!parse_header(w)){discard_response(response);return mut_error("TERRAX_STATE_ERROR","encoded header could not be reopened");}
    return result;
    }

static int parse_item(TxJsonParser *p,TxBuf *items){
    jp_ws(p);
    if (p->pos<p->len&&p->text[p->pos]=='n'){
        if (!jp_null(p))return mut_fail("TERRAX_PARSE_ERROR","malformed null chest slot");
        buf_u16le(items,0u);
        return items->ok;
        }
    if (!jp_take(p,'{'))return 0;
    int first=1,done=0;
    uint32_t seen=0u;
    int64_t stack=0,item_type=0,prefix=0;
    while (!done){
        char key[32];uint32_t key_len=0u;
        if (!jp_member_next(p,&first,&done))return 0;
        if (done)break;
        if (!jp_string(p,key,sizeof(key),&key_len)||!jp_take(p,':'))return 0;
        (void)key_len;
        uint32_t bit;
        if (tx_streq_c(key,"stack")){bit=1u;if (!jp_integer(p,1,32767,&stack))return 0;}
        else if (tx_streq_c(key,"itemType")){bit=2u;if (!jp_integer(p,1,TX_MUTATOR_MAX_ITEM_TYPE,&item_type))return 0;}
        else if (tx_streq_c(key,"prefix")){bit=4u;if (!jp_integer(p,0,255,&prefix))return 0;}
        else return mut_fail("TERRAX_VALIDATION_ERROR","unknown chest item field");
        if (seen&bit)return mut_fail("TERRAX_VALIDATION_ERROR","duplicate chest item field");
        seen|=bit;
        }
    if (seen!=7u)return mut_fail("TERRAX_VALIDATION_ERROR","chest item requires stack, itemType, and prefix");
    buf_u16le(items,(uint32_t)stack);
    buf_u32le(items,(uint32_t)item_type);
    buf_u8(items,(uint8_t)prefix);
    return items->ok||mut_fail("TERRAX_WASM_OOM","failed to encode chest item");
    }

static int parse_items(TxJsonParser *p,TxBuf *items,uint32_t *item_count){
    int first=1,done=0;
    if (!jp_take(p,'['))return 0;
    while (!done){
        if (!jp_array_next(p,&first,&done))return 0;
        if (done)break;
        if (*item_count>=TX_MUTATOR_MAX_CHEST_ITEMS)
            return mut_fail("TERRAX_VALIDATION_ERROR","chest has too many item slots");
        if (!parse_item(p,items))return 0;
        (*item_count)++;
        }
    return 1;
    }

static int parse_chest(TxWorld *w,TxJsonParser *p,TxBuf *section,uint32_t shared_slots){
    int first=1,done=0;
    uint32_t seen=0u,item_count=0u;
    int64_t x=0,y=0,max_items=0;
    char name[TX_MUTATOR_MAX_STRING_BYTES+1u];uint32_t name_len=0u;
    TxBuf items={0};
    items.ok=1;
    if (!jp_take(p,'{')){tx_internal_free(items.data);return 0;}
    while (!done){
        char key[32];uint32_t key_len=0u;uint32_t bit=0u;
        if (!jp_member_next(p,&first,&done)){tx_internal_free(items.data);return 0;}
        if (done)break;
        if (!jp_string(p,key,sizeof(key),&key_len)||!jp_take(p,':')){tx_internal_free(items.data);return 0;}
        (void)key_len;
        if (tx_streq_c(key,"x")){bit=1u;if (!jp_integer(p,0,w->maxTilesX-1,&x)){tx_internal_free(items.data);return 0;}}
        else if (tx_streq_c(key,"y")){bit=2u;if (!jp_integer(p,0,w->maxTilesY-1,&y)){tx_internal_free(items.data);return 0;}}
        else if (tx_streq_c(key,"name")){bit=4u;if (!jp_string(p,name,sizeof(name),&name_len)){tx_internal_free(items.data);return 0;}}
        else if (tx_streq_c(key,"maxItems")){bit=8u;if (!jp_integer(p,0,TX_MUTATOR_MAX_CHEST_ITEMS,&max_items)){tx_internal_free(items.data);return 0;}}
        else if (tx_streq_c(key,"items")){bit=16u;if (!parse_items(p,&items,&item_count)){tx_internal_free(items.data);return 0;}}
        else{tx_internal_free(items.data);return mut_fail("TERRAX_VALIDATION_ERROR","unknown chest field");}
        if (seen&bit){tx_internal_free(items.data);return mut_fail("TERRAX_VALIDATION_ERROR","duplicate chest field");}
        seen|=bit;
        }
    if (seen!=31u||item_count!=(uint32_t)max_items){
        tx_internal_free(items.data);
        return mut_fail("TERRAX_VALIDATION_ERROR","chest requires x, y, name, maxItems, and exactly maxItems item slots");
        }
    if (w->version<294u&&(uint32_t)max_items!=shared_slots){
        tx_internal_free(items.data);
        return mut_fail("TERRAX_VALIDATION_ERROR","legacy chests must retain the world's shared item slot count");
        }
    buf_u32le(section,(uint32_t)x);
    buf_u32le(section,(uint32_t)y);
    buf_7bit(section,name_len);
    buf_bytes(section,name,name_len);
    if (w->version>=294u)buf_u32le(section,(uint32_t)max_items);
    buf_bytes(section,items.data,items.len);
    tx_internal_free(items.data);
    return section->ok||mut_fail("TERRAX_WASM_OOM","failed to encode chest section");
    }

static int parse_chests_request(TxWorld *w,TxJsonParser *p,TxBuf *section,uint32_t *chest_count,uint32_t shared_slots){
    int first=1,done=0,seen_chests=0;
    if (!jp_take(p,'{'))return 0;
    while (!done){
        char key[32];uint32_t key_len=0u;
        if (!jp_member_next(p,&first,&done))return 0;
        if (done)break;
        if (!jp_string(p,key,sizeof(key),&key_len)||!jp_take(p,':'))return 0;
        (void)key_len;
        if (!tx_streq_c(key,"chests"))return mut_fail("TERRAX_VALIDATION_ERROR","replace_chests accepts only the chests field");
        if (seen_chests)return mut_fail("TERRAX_VALIDATION_ERROR","duplicate chests field");
        seen_chests=1;
        int array_first=1,array_done=0;
        if (!jp_take(p,'['))return 0;
        while (!array_done){
            if (!jp_array_next(p,&array_first,&array_done))return 0;
            if (array_done)break;
            if (*chest_count>=TX_MUTATOR_MAX_CHESTS)
                return mut_fail("TERRAX_VALIDATION_ERROR","world has too many chests");
            if (!parse_chest(w,p,section,shared_slots))return 0;
            (*chest_count)++;
            }
        }
    if (!seen_chests)return mut_fail("TERRAX_VALIDATION_ERROR","replace_chests requires chests");
    return jp_end(p);
    }

int tx_mutate_replace_chests(TxWorld *w,const char *request,uint32_t request_len,TxBuf *response){
    if (!tx_world_require_writable(w)) return -1;
    if (!w||!request||request_len==0u||request_len>TX_MUTATOR_MAX_JSON_BYTES)
        return mut_error("TERRAX_INVALID_ARGUMENT","invalid replace_chests request");
    if (w->version<88u)
        return mut_error("TERRAX_NOT_SUPPORTED","replace_chests requires a sectioned WLD version 88 or newer");
    if (w->pointer_count<=2u)
        return mut_error("TERRAX_NOT_SUPPORTED","world has no chest section");
    uint32_t shared_slots=40u;
    if (w->version<294u){
        const uint8_t *source=w->section_overrides[2].data;
        uint32_t source_len=w->section_overrides[2].len;
        if (!w->section_overrides[2].active){
            if (w->starts[2]>w->ends[2]||w->ends[2]>w->file_len)
                return mut_error("TERRAX_VALIDATION_ERROR","invalid legacy chest section bounds");
            source_len=w->ends[2]-w->starts[2];
            source=w->file?w->file+w->starts[2]:NULL;
            }
        if (source_len){
            if (!source||source_len<4u)
                return mut_error("TERRAX_VALIDATION_ERROR","legacy chest section is missing its shared item slot count");
            shared_slots=(uint32_t)source[2]|((uint32_t)source[3]<<8u);
            if (shared_slots>TX_MUTATOR_MAX_CHEST_ITEMS)
                return mut_error("TERRAX_VALIDATION_ERROR","legacy shared item slot count exceeds the supported limit");
            }
        }
    TxBuf encoded;buf_init(&encoded,1024u);
    if (!encoded.ok)return mut_error("TERRAX_WASM_OOM","failed to allocate chest encoder");
    buf_u16le(&encoded,0u);
    if (w->version<294u)buf_u16le(&encoded,shared_slots);
    TxJsonParser parser={request,request_len,0u};
    uint32_t chest_count=0u;
    if (!parse_chests_request(w,&parser,&encoded,&chest_count,shared_slots)||!encoded.ok){
        tx_internal_free(encoded.data);
        if (!encoded.ok)mut_fail("TERRAX_WASM_OOM","failed to encode chest section");
        return -1;
        }
    encoded.data[0]=(uint8_t)chest_count;
    encoded.data[1]=(uint8_t)(chest_count>>8u);
    uint32_t override_mark=tx_mark();
    buf_cstr(response,"{\"status\":\"ok\",\"chestCount\":");
    json_u32(response,chest_count);buf_u8(response,'}');
    int result=set_result_buf(response);
    if (result<0){tx_internal_free(encoded.data);return -1;}
    if (!set_section_override_data(w,2,encoded.data,encoded.len)){
        tx_internal_free(encoded.data);discard_response(response);return -1;
        }
    w->heap_mark=override_mark;
    return result;
    }

static int parse_bestiary_entry(TxJsonParser *p,TxBuf *out,int with_count){
    int first=1,done=0;
    uint32_t seen=0u,name_len=0u;
    int64_t count=0;
    char name[TX_MUTATOR_MAX_STRING_BYTES+1u];
    if (!jp_take(p,'{'))return 0;
    while (!done){
        char key[32];uint32_t key_len=0u;uint32_t bit=0u;
        if (!jp_member_next(p,&first,&done))return 0;
        if (done)break;
        if (!jp_string(p,key,sizeof(key),&key_len)||!jp_take(p,':'))return 0;
        (void)key_len;
        if (tx_streq_c(key,"persistentNpcId")){bit=1u;if (!jp_string(p,name,sizeof(name),&name_len))return 0;}
        else if (with_count&&tx_streq_c(key,"killCount")){bit=2u;if (!jp_integer(p,0,TX_MUTATOR_MAX_KILL_COUNT,&count))return 0;}
        else return mut_fail("TERRAX_VALIDATION_ERROR","unknown bestiary entry field");
        if (seen&bit)return mut_fail("TERRAX_VALIDATION_ERROR","duplicate bestiary entry field");
        seen|=bit;
        }
    if (seen!=(with_count?3u:1u))
        return mut_fail("TERRAX_VALIDATION_ERROR",with_count?"kill entry requires persistentNpcId and killCount":"bestiary entry requires persistentNpcId");
    buf_7bit(out,name_len);buf_bytes(out,name,name_len);
    if (with_count)buf_u32le(out,(uint32_t)count);
    return out->ok||mut_fail("TERRAX_WASM_OOM","failed to encode bestiary entry");
    }

static int parse_bestiary_array(TxJsonParser *p,TxBuf *out,int with_count,uint32_t *entry_count){
    int first=1,done=0;
    if (!jp_take(p,'['))return 0;
    while (!done){
        if (!jp_array_next(p,&first,&done))return 0;
        if (done)break;
        if (*entry_count>=TX_MUTATOR_MAX_BESTIARY_ENTRIES)
            return mut_fail("TERRAX_VALIDATION_ERROR","bestiary array exceeds the entry limit");
        if (!parse_bestiary_entry(p,out,with_count))return 0;
        (*entry_count)++;
        }
    return 1;
    }

int tx_mutate_replace_bestiary(TxWorld *w,const char *request,uint32_t request_len,TxBuf *response){
    if (!tx_world_require_writable(w)) return -1;
    if (!w||!request||request_len==0u||request_len>TX_MUTATOR_MAX_JSON_BYTES)
        return mut_error("TERRAX_INVALID_ARGUMENT","invalid replace_bestiary request");
    if (w->version<210u||w->pointer_count<=8u)
        return mut_error("TERRAX_NOT_SUPPORTED","world has no bestiary section");
    TxBuf arrays[3];
    for (uint32_t i=0;i<3u;i++){
        buf_init(&arrays[i],256u);
        if (!arrays[i].ok){
            for (uint32_t j=0;j<=i;j++)if (arrays[j].data)tx_internal_free(arrays[j].data);
            return mut_error("TERRAX_WASM_OOM","failed to allocate bestiary encoder");
            }
        }
    TxJsonParser parser={request,request_len,0u};
    uint32_t counts[3]={0u,0u,0u};
    uint32_t seen=0u;
    int first=1,done=0,ok=jp_take(&parser,'{');
    while (ok&&!done){
        char key[32];uint32_t key_len=0u;uint32_t index;
        ok=jp_member_next(&parser,&first,&done);
        if (!ok||done)break;
        if (!jp_string(&parser,key,sizeof(key),&key_len)||!jp_take(&parser,':')){ok=0;break;}
        (void)key_len;
        if (tx_streq_c(key,"kills"))index=0u;
        else if (tx_streq_c(key,"sightings"))index=1u;
        else if (tx_streq_c(key,"chats"))index=2u;
        else{ok=mut_fail("TERRAX_VALIDATION_ERROR","unknown bestiary section field");break;}
        if (seen&(1u<<index)){ok=mut_fail("TERRAX_VALIDATION_ERROR","duplicate bestiary section field");break;}
        seen|=1u<<index;
        ok=parse_bestiary_array(&parser,&arrays[index],index==0u,&counts[index]);
        }
    if (ok&&seen!=7u)ok=mut_fail("TERRAX_VALIDATION_ERROR","replace_bestiary requires kills, sightings, and chats");
    if (ok)ok=jp_end(&parser);
    if (!ok){for (uint32_t i=0;i<3u;i++)tx_internal_free(arrays[i].data);return -1;}

    uint64_t total=12u;
    for (uint32_t i=0;i<3u;i++)total+=arrays[i].len;
    if (total>UINT32_MAX){for (uint32_t i=0;i<3u;i++)tx_internal_free(arrays[i].data);return mut_error("TERRAX_WASM_OOM","bestiary section exceeds WASM limits");}
    TxBuf encoded;buf_init(&encoded,(uint32_t)total);
    if (encoded.ok){
        for (uint32_t i=0;i<3u;i++){buf_u32le(&encoded,counts[i]);buf_bytes(&encoded,arrays[i].data,arrays[i].len);}
        }
    for (uint32_t i=0;i<3u;i++)tx_internal_free(arrays[i].data);
    if (!encoded.ok){if (encoded.data)tx_internal_free(encoded.data);return mut_error("TERRAX_WASM_OOM","failed to encode bestiary section");}
    uint32_t override_mark=tx_mark();
    buf_cstr(response,"{\"status\":\"ok\",\"kills\":");json_u32(response,counts[0]);
    buf_cstr(response,",\"sightings\":");json_u32(response,counts[1]);
    buf_cstr(response,",\"chats\":");json_u32(response,counts[2]);buf_u8(response,'}');
    int result=set_result_buf(response);
    if (result<0){tx_internal_free(encoded.data);return -1;}
    if (!set_section_override_data(w,8,encoded.data,encoded.len)){
        tx_internal_free(encoded.data);discard_response(response);return -1;
        }
    w->heap_mark=override_mark;
    return result;
    }
