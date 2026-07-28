/* Verified WLD section encoders used by explicit mutating operations. */
#include "terra_types.h"
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
extern void buf_bytes(TxBuf *b,const void *p,uint32_t n);
extern void buf_cstr(TxBuf *b,const char *s);
extern void json_u32(TxBuf *b,uint32_t v);
extern int set_result_buf(TxBuf *b);
extern int set_section_override_data(TxWorld *w,int idx,uint8_t *data,uint32_t len);
extern void *memcpy(void *dst,const void *src,unsigned long n);
extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;

#define TX_MUTATOR_MAX_JSON_BYTES (1024u * 1024u)
#define TX_MUTATOR_MAX_STRING_BYTES 255u
#define TX_MUTATOR_MAX_CHESTS 1000u
#define TX_MUTATOR_MAX_CHEST_ITEMS 504u
#define TX_MUTATOR_MAX_BESTIARY_ENTRIES 4096u
#define TX_MUTATOR_MAX_KILL_COUNT 1000000u
#define TX_MUTATOR_MAX_ITEM_TYPE 1000000u

typedef struct TxJsonParser {
    const char *text;
    uint32_t len;
    uint32_t pos;
} TxJsonParser;

typedef struct TxHeaderEdit {
    uint32_t field_index;
    uint8_t value;
} TxHeaderEdit;

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

static int parse_header_patch(TxWorld *w,TxJsonParser *p,TxHeaderEdit *edits,uint32_t *edit_count){
    int first=1,done=0,seen_patch=0;
    if (!jp_take(p,'{'))return 0;
    while (!done){
        char key[64];uint32_t key_len=0u;
        if (!jp_member_next(p,&first,&done))return 0;
        if (done)break;
        if (!jp_string(p,key,sizeof(key),&key_len)||!jp_take(p,':'))return 0;
        (void)key_len;
        if (!tx_streq_c(key,"patch"))return mut_fail("TERRAX_VALIDATION_ERROR","header_patch accepts only the patch field");
        if (seen_patch)return mut_fail("TERRAX_VALIDATION_ERROR","duplicate patch field");
        seen_patch=1;
        if (!jp_take(p,'{'))return 0;
        int patch_first=1,patch_done=0;
        while (!patch_done){
            char field_name[96];uint32_t field_name_len=0u;int value=0;
            if (!jp_member_next(p,&patch_first,&patch_done))return 0;
            if (patch_done)break;
            if (!jp_string(p,field_name,sizeof(field_name),&field_name_len)||!jp_take(p,':')||!jp_bool(p,&value))return 0;
            (void)field_name_len;
            uint32_t field_index=UINT32_MAX;
            for (uint32_t i=0;i<w->header_bool_field_count;i++){
                if (tx_streq_c(field_name,w->header_bool_fields[i].json_name)){field_index=i;break;}
                }
            if (field_index==UINT32_MAX)
                return mut_fail("TERRAX_NOT_SUPPORTED","header field is not a whitelisted boolean for this world version");
            for (uint32_t i=0;i<*edit_count;i++)if (edits[i].field_index==field_index)
                return mut_fail("TERRAX_VALIDATION_ERROR","duplicate header patch field");
            if (*edit_count>=TX_MAX_HEADER_BOOL_FIELDS)
                return mut_fail("TERRAX_VALIDATION_ERROR","too many header patch fields");
            edits[*edit_count].field_index=field_index;
            edits[*edit_count].value=(uint8_t)value;
            (*edit_count)++;
            }
        }
    if (!seen_patch||*edit_count==0u)return mut_fail("TERRAX_VALIDATION_ERROR","header patch must not be empty");
    return jp_end(p);
    }

int tx_mutate_header_patch(TxWorld *w,const char *request,uint32_t request_len,TxBuf *response){
    if (!w||!request||request_len==0u||request_len>TX_MUTATOR_MAX_JSON_BYTES)
        return mut_error("TERRAX_INVALID_ARGUMENT","invalid header_patch request");
    if (w->pointer_count<1u)
        return mut_error("TERRAX_NOT_SUPPORTED","world has no header section");
    TxJsonParser parser={request,request_len,0u};
    TxHeaderEdit edits[TX_MAX_HEADER_BOOL_FIELDS];
    uint32_t edit_count=0u;
    if (!parse_header_patch(w,&parser,edits,&edit_count))return -1;

    const uint8_t *source;
    uint32_t source_len;
    if (w->section_overrides[0].active){source=w->section_overrides[0].data;source_len=w->section_overrides[0].len;}
    else{
        if (w->ends[0]<w->starts[0]||w->ends[0]>w->file_len)
            return mut_error("TERRAX_STATE_ERROR","header section bounds are invalid");
        source=w->file+w->starts[0];source_len=w->ends[0]-w->starts[0];
        }
    TxBuf encoded;
    buf_init(&encoded,source_len);
    if (!encoded.ok)return mut_error("TERRAX_WASM_OOM","failed to allocate header encoder");
    buf_bytes(&encoded,source,source_len);
    if (!encoded.ok){tx_internal_free(encoded.data);return mut_error("TERRAX_WASM_OOM","failed to copy header section");}
    for (uint32_t i=0;i<edit_count;i++){
        TxHeaderBoolField *field=&w->header_bool_fields[edits[i].field_index];
        if (field->section_offset>=encoded.len){
            tx_internal_free(encoded.data);
            return mut_error("TERRAX_STATE_ERROR","header field offset is outside the section");
            }
        encoded.data[field->section_offset]=edits[i].value;
        }
    uint32_t override_mark=tx_mark();
    buf_cstr(response,"{\"status\":\"ok\",\"updated\":");
    json_u32(response,edit_count);
    buf_u8(response,'}');
    int result=set_result_buf(response);
    if (result<0){tx_internal_free(encoded.data);return -1;}
    if (!set_section_override_data(w,0,encoded.data,encoded.len)){
        tx_internal_free(encoded.data);discard_response(response);return -1;
        }
    w->heap_mark=override_mark;
    for (uint32_t i=0;i<edit_count;i++){
        TxHeaderBoolField *field=&w->header_bool_fields[edits[i].field_index];
        *((uint8_t*)w+field->world_member_offset)=edits[i].value;
        }
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

static int parse_chest(TxWorld *w,TxJsonParser *p,TxBuf *section){
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
    buf_u32le(section,(uint32_t)x);
    buf_u32le(section,(uint32_t)y);
    buf_7bit(section,name_len);
    buf_bytes(section,name,name_len);
    buf_u32le(section,(uint32_t)max_items);
    buf_bytes(section,items.data,items.len);
    tx_internal_free(items.data);
    return section->ok||mut_fail("TERRAX_WASM_OOM","failed to encode chest section");
    }

static int parse_chests_request(TxWorld *w,TxJsonParser *p,TxBuf *section,uint32_t *chest_count){
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
            if (!parse_chest(w,p,section))return 0;
            (*chest_count)++;
            }
        }
    if (!seen_chests)return mut_fail("TERRAX_VALIDATION_ERROR","replace_chests requires chests");
    return jp_end(p);
    }

int tx_mutate_replace_chests(TxWorld *w,const char *request,uint32_t request_len,TxBuf *response){
    if (!w||!request||request_len==0u||request_len>TX_MUTATOR_MAX_JSON_BYTES)
        return mut_error("TERRAX_INVALID_ARGUMENT","invalid replace_chests request");
    if (w->version<294u)
        return mut_error("TERRAX_NOT_SUPPORTED","replace_chests requires WLD version 294 or newer");
    if (w->pointer_count<=2u)
        return mut_error("TERRAX_NOT_SUPPORTED","world has no chest section");
    TxBuf encoded;buf_init(&encoded,1024u);
    if (!encoded.ok)return mut_error("TERRAX_WASM_OOM","failed to allocate chest encoder");
    buf_u16le(&encoded,0u);
    TxJsonParser parser={request,request_len,0u};
    uint32_t chest_count=0u;
    if (!parse_chests_request(w,&parser,&encoded,&chest_count)||!encoded.ok){
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
    if (!w||!request||request_len==0u||request_len>TX_MUTATOR_MAX_JSON_BYTES)
        return mut_error("TERRAX_INVALID_ARGUMENT","invalid replace_bestiary request");
    if (w->pointer_count<=8u)
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
