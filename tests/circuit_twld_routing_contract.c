#include "terra_world.h"
#include "terra_circuit_world_internal.h"
#include "terra_circuit_geometry.h"
#include <assert.h>
#include <string.h>
#include <zlib.h>
extern TxWorld* tx_get_world(uint32_t);
typedef struct Buffer { uint8_t bytes[8192]; uint32_t count; } Buffer;
static void add(Buffer* b, const void* p, uint32_t n) { assert(b->count + n <= sizeof(b->bytes)); memcpy(b->bytes + b->count, p, n); b->count += n; }
static void byte(Buffer* b, uint8_t x) { add(b, &x, 1); }
static void be32(Buffer* b, uint32_t x) { byte(b, x >> 24); byte(b, x >> 16); byte(b, x >> 8); byte(b, x); }
static void string(Buffer* b, const char* s) { size_t n = strlen(s); byte(b, n >> 8); byte(b, n); add(b, s, (uint32_t)n); }
static void tag(Buffer* b, uint8_t type, const char* name) { byte(b, type); string(b, name); }
static void strtag(Buffer* b, const char* name, const char* value) { tag(b, 8, name); string(b, value); }
static void inttag(Buffer* b, const char* name, uint32_t value) { tag(b, 3, name); be32(b, value); }
static void map_entry(Buffer* b, uint32_t id, const char* mod, const char* name, uint8_t framed) {
    inttag(b, "value", id); strtag(b, "mod", mod); strtag(b, "name", name);
    tag(b, 1, "framed"); byte(b, framed); strtag(b, "unknown-map-data", "retained verbatim"); byte(b, 0);
}
static Buffer fixture(uint32_t* frame_offset, uint32_t* array_length, int color) {
    Buffer b = {0}; tag(&b, 10, "");
    strtag(&b, "future-header", "unknown metadata is retained");
    tag(&b, 10, "tiles");
    tag(&b, 7, "tileData"); *array_length = b.count; be32(&b, 14);
    uint8_t data[] = {0,0, 0xbb,0x02,0,18,0,36,0, 0xbc,0x02,7, 0,0};
    *frame_offset = b.count + 5; add(&b, data, sizeof(data));
    tag(&b, 9, "tileMap"); byte(&b, 10); be32(&b, 2);
    map_entry(&b, 699, "WireHead", color?"ColorPixelBox":"OtherTile", 1);
    map_entry(&b, 700, "UnknownMod", "NonFramedTile", 0);
    tag(&b, 7, "wallData"); be32(&b, 8); for (uint32_t i=0;i<8;++i) byte(&b, 0);
    tag(&b, 9, "wallMap"); byte(&b, 10); be32(&b, 0); byte(&b, 0);
    tag(&b, 9, "tileEntities"); byte(&b, 10); be32(&b, 1);
    strtag(&b, "mod", "Terraria"); strtag(&b, "name", "TETrainingDummy");
    inttag(&b, "X", 1); inttag(&b, "Y", 1); byte(&b, 0);
    tag(&b, 11, "future-int-array"); be32(&b, 2); be32(&b, 0xdeadbeef); be32(&b, 0x10203040);
    tag(&b, 12, "future-long-array"); be32(&b, 1); be32(&b, 0xabcdef00); be32(&b, 0x76543210);
    tag(&b, 9, "nested-list"); byte(&b, 9); be32(&b, 1); byte(&b, 8); be32(&b, 2); string(&b, "a"); string(&b, "b");
    byte(&b, 0); return b;
}
static Buffer gzip_bytes(const Buffer* raw) {
    Buffer out = {0}; z_stream z = {0};
    assert(deflateInit2(&z, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    z.next_in = (Bytef*)raw->bytes; z.avail_in = raw->count;
    z.next_out = out.bytes; z.avail_out = sizeof(out.bytes);
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END); out.count = sizeof(out.bytes) - z.avail_out;
    assert(deflateEnd(&z) == Z_OK); return out;
}

static void run(int color) {
 uint32_t frame,length,handle=0,session=0; Buffer raw=fixture(&frame,&length,color), gz=gzip_bytes(&raw);
 assert(terra_world_create(&handle)==0); TxWorld* source=tx_get_world(handle);
 source->maxTilesX=2;source->maxTilesY=2;source->version=source->original_version=269;
 assert(terra_circuit_world_begin(handle,77,88,gz.count,8u*1024u*1024u,&session)==0);
 CxWorld* w=cx_lookup(session); uint32_t reads=0;
 while(w->phase==CX_TWLD) {
  uint32_t work=1;int status=cx_twld_step(w,&work);assert(status>=0);
  if(w->event.kind==TCW_READ) {TerraCircuitWorldEvent e=w->event;
   assert(e.source_id==88&&w->read_target==CX_READ_TWLD);
   assert(terra_circuit_world_supply(session,e.source_id,e.offset,gz.bytes+e.offset,e.length)==0);++reads;
  }
 }
 assert(reads==2&&w->twld_state==(color?1u:0u));
 /* Valid metadata-only TWLD retains vanilla capture; color pixels retain the
    existing mod-order bypass. Generated streamed source is immutable. */
 uint32_t columns[]={16,18,20}; source->stream_columns=columns;w->world_source=7;
 w->tile_start=16;w->tile_end=20;w->wire_cells=0;w->phase=CX_COUNT;
 memset(w->column,0,w->height*sizeof(TxTile));memset(w->gate_at_y,0,w->height*4u);
 assert(cx_order_capture_column(w)==0);
 assert(color?(!w->order&&!w->geometry):(w->order&&w->geometry));
 source->stream_columns=NULL;
 assert(terra_circuit_world_close(session)==0);assert(terra_world_close(handle)==0);
}
int main(void){run(0);run(1);return 0;}
