#ifndef TERRA_CIRCUIT_WORLD_INTERNAL_H
#define TERRA_CIRCUIT_WORLD_INTERNAL_H
#include "terra_circuit_world.h"
#include "terra_circuit_vm.h"
#include "terra_circuit_twld.h"
#include "terra_types.h"
#include <stddef.h>
#include <stdint.h>

#define CX_PAGE_SHIFT 16u
#define CX_PAGE_SIZE (1u << CX_PAGE_SHIFT)
#define CX_WORDS (CX_PAGE_SIZE / 4u)
#define CX_CHECK_COLUMNS 64u
#define CX_CACHE_PAGES 64u
#define CX_NONE UINT32_MAX
enum CxPhase {
    CX_TOPOLOGY=1, CX_ROOTS, CX_NUMBER, CX_REMAP, CX_MAP_SIZE, CX_MAP_ENCODE,
    CX_COUNT, CX_LAYOUT, CX_ZERO, CX_WRITE, CX_FLUSH, CX_INTERN, CX_IDLE,
    CX_QUERY, CX_RUN, CX_SAVE_PREFIX, CX_SAVE_TILES, CX_SAVE_SUFFIX, CX_SAVE_PATCH,
    CX_FAILED, CX_CANCELLED, CX_TWLD, CX_TICKS
};
typedef struct CxBytes { uint8_t** pages; uint32_t length, pages_count, pages_capacity; } CxBytes;
typedef struct CxWords { uint32_t** pages; uint32_t length, pages_count, pages_capacity; } CxWords;
typedef struct CxCheckpoint {
    uint32_t column, source_offset, map_offset, map_previous;
    uint32_t label_count, gate_count, offset, length;
} CxCheckpoint;
typedef struct CxCachePage { uint32_t page, stamp, dirty, valid; } CxCachePage;
typedef struct CxAction { uint32_t code, length, members; } CxAction;
typedef struct CxHash { uint64_t hash; uint32_t code, length; } CxHash;
typedef struct CxGeneralLamp {
    uint32_t x,y,nets[4],gate; uint8_t initial, faulty; uint16_t reserved;
} CxGeneralLamp;
typedef struct CxGeneralGate {
    uint32_t id,x,y,first,count,trigger_first,trigger_count,nets[4];
    uint8_t style,frame,faulty,initial_frame;
} CxGeneralGate;
typedef struct CxOverride { uint32_t gate, x, y, value; } CxOverride;
typedef struct CxBinding {
    uint32_t x,y,nets[4],opposite[4],gate; TxTile tile;
} CxBinding;
typedef struct CxInputPoint { uint32_t x,y,value,index; } CxInputPoint;
typedef struct CxDevice { uint32_t x,y,nets[4],pulse_nets[4],gate; TxTile tile,initial; uint32_t cooldown,saved_cooldown,wire_hit_mask; int32_t can_deactivate; } CxDevice;
typedef struct CxActuationPolicy { uint32_t device,x,y; uint32_t valid; TxTile above[10]; } CxActuationPolicy;
typedef struct CxPixel { uint32_t x,y,h[4],v[4]; uint8_t state,initial,hit_h,hit_v,custom,marked; uint8_t reserved[2]; } CxPixel;
typedef struct CxModTile { TerraTwldTile tile; TxTile vanilla; } CxModTile;
typedef struct CxPort { uint32_t net,pixel; uint8_t axis,colour; uint16_t reserved; } CxPort;
typedef struct CxWorld {
    uint32_t id, world_handle, maximum, bytes, peak, phase, error;
    TxWorld* world;
    uint32_t world_source, world_size, tile_start, tile_end, scratch_source;
    uint32_t twld_source, twld_size, twld_state;
    TerraTwld* twld; uint32_t twld_cursor,twld_mode,twld_output_offset,twld_saved_size;
    CxModTile* mods; uint32_t mod_count,mod_capacity,mod_cursor;
    TerraCircuitWorldEvent event;
    TerraCircuitWorldCommand command;
    uint8_t* input; uint32_t input_offset,input_length;
    uint32_t read_target,read_slot;
    uint32_t width,height,cursor,x,y,col_stage,col_colour,col_row;
    TxTile* column; uint32_t* front; uint32_t* ids; uint32_t* seed_ids; uint32_t* opposite_ids; uint32_t* gate_at_y; uint32_t* pixel_at_y;
    uint32_t up, labels_count, networks, label_cursor, map_cursor,map_previous;
    CxWords parents, code_ends, member_ends, previous_gate;
    CxBytes map, checkpoints, code, members;
    CxCheckpoint* checkpoint_index; uint32_t checkpoint_count;
    uint32_t* column_gates;
    uint32_t gate_cursor,gates,wire_cells,devices_count,min_x,min_y,max_x,max_y;
    uint32_t phase_cursor,map_size,action_count,code_size,member_size,store_size,max_group;
    uint32_t layout_code,layout_member;
    uint8_t* action_bits; uint32_t* action_rank; CxAction* actions;
    uint8_t* cache; CxCachePage* cache_pages; uint32_t* cache_map; uint8_t* stored_pages;
    uint32_t cache_count,store_pages,cache_clock,flush_index;
    uint8_t record[128]; uint32_t record_length,record_position,record_store,record_member_length;
    uint32_t emit_y,emit_colour,emit_phase,emit_gate,emit_general;
    uint32_t intern_group,intern_action,intern_stage,intern_start,intern_end,intern_member_start,intern_member_end;
    uint32_t intern_copy,intern_member_copy,intern_pool_code;
    uint8_t* intern_buffer; uint32_t intern_capacity;
    CxHash* hashes; uint32_t hash_capacity;
    CxGeneralGate* general; uint32_t general_count,general_capacity;
    CxGeneralLamp* lamps; uint32_t lamp_count,lamp_capacity;
    CxDevice* devices; uint32_t device_count,device_capacity;
    uint32_t column_device_first;
    TxTile* previous_columns[2]; CxActuationPolicy* policies; uint32_t policy_count,policy_capacity;
    uint32_t* mechs; uint32_t* mechs_snapshot; uint32_t mech_count,mech_saved_count,mech_capacity;
    uint32_t tick_remaining,tick_cursor,tick_stage;
    uint32_t seed_x,seed_y,seed_width,seed_height;
    uint32_t operation_active,interaction_pending; uint64_t ticks_saved;
    CxPixel* pixels; uint32_t pixel_count,pixel_capacity;
    CxPort* ports; uint32_t port_count;
    uint32_t* pixel_touched; uint32_t pixel_touched_count;
    uint8_t* pixel_snapshot;
    uint32_t vm_trip_index;
    uint32_t vm_source_gate;
    CxOverride* overrides; uint32_t override_count,override_capacity;
    CxOverride* override_snapshot; uint32_t override_snapshot_capacity,override_saved_count;
    CxBinding* bindings; uint32_t binding_count,binding_capacity;
    CxInputPoint* points; uint32_t point_count,point_index;
    uint32_t* result; uint32_t result_count,result_capacity;
    uint32_t query_x0,query_x1,query_y0,query_y1,query_stride,query_point;
    uint32_t* trigger_nets; uint32_t trigger_count,trigger_capacity,trigger_remaining;
    uint32_t seed_lamps;
    uint32_t eval_group,eval_offset,eval_member,eval_gate,eval_valid;
    int32_t rng[56]; uint32_t rng_i,rng_j; int32_t rng_saved[56]; uint32_t rng_saved_i,rng_saved_j;
    uint8_t* general_snapshot;
    uint64_t ticks;
    TerraCircuitVm* vm;
    uint32_t output_source,output_offset,save_cursor,save_phase,save_result_size;
    uint8_t* output; uint32_t output_length,output_capacity;
    TxTile save_previous; uint32_t save_repeat,save_previous_valid;
    uint32_t save_new_tile_end,save_prefix_length,save_suffix_length;
    uint8_t save_patch[64];
} CxWorld;

void* cx_alloc(CxWorld*,uint32_t);
void cx_free(CxWorld*,void*);
int cx_fail(CxWorld*,int,const char*);
int cx_bytes_append(CxWorld*,CxBytes*,const void*,uint32_t);
int cx_bytes_byte(CxWorld*,CxBytes*,uint8_t);
uint8_t cx_bytes_get(const CxBytes*,uint32_t);
void cx_bytes_copy(const CxBytes*,uint32_t,void*,uint32_t);
int cx_bytes_equal(const CxBytes*,uint32_t,const uint8_t*,uint32_t);
void cx_bytes_free(CxWorld*,CxBytes*);
int cx_words_size(CxWorld*,CxWords*,uint32_t);
uint32_t cx_word(const CxWords*,uint32_t);
void cx_set_word(CxWords*,uint32_t,uint32_t);
void cx_words_free(CxWorld*,CxWords*);
void cx_words_drop_page(CxWorld*,CxWords*,uint32_t);
uint32_t cx_var_size(uint32_t);
uint32_t cx_var_put(uint8_t*,uint32_t);
uint32_t cx_var_read(const CxBytes*,uint32_t*);
int cx_emit(CxWorld*,uint32_t,uint32_t,uint32_t,uint32_t,const void*);
int cx_world_tile(CxWorld*,TxTile*);
int cx_scan_step(CxWorld*,uint32_t*);
void cx_scan_reset(CxWorld*);
int cx_store(CxWorld*,uint32_t,uint8_t*,uint32_t,int);
int cx_cache_flush(CxWorld*);
int cx_compile_step(CxWorld*,uint32_t*);
int cx_compile_initialize(CxWorld*);
CxWorld* cx_lookup(uint32_t);
int cx_create_vm(CxWorld*);
uint32_t cx_action_index(const CxWorld*,uint32_t);
int cx_query_begin(CxWorld*);
int cx_query_step(CxWorld*,uint32_t*);
int cx_command_complete(CxWorld*);
int cx_save_step(CxWorld*,uint32_t*);
uint32_t cx_wire_mask(const TxTile*);
uint32_t cx_true_lamp(CxWorld*,const CxBinding*);
int cx_override_value(CxWorld*,uint32_t,uint32_t,uint32_t);
int cx_set_override(CxWorld*,uint32_t,uint32_t,uint32_t,uint32_t);
void cx_current_tile(CxWorld*,uint32_t,uint32_t,uint32_t,TxTile*,const uint32_t*);
int cx_general_lamp_value(CxWorld*,const CxGeneralLamp*);
int cx_twld_begin(CxWorld*);
int cx_twld_step(CxWorld*,uint32_t*);
int cx_twld_save_begin(CxWorld*);
void cx_twld_cancel_save(CxWorld*);
int cx_mod_column(CxWorld*);
int cx_devices_column(CxWorld*);
void cx_devices_bind_column(CxWorld*);
int cx_devices_compile(CxWorld*);
void cx_pixel_ports(CxWorld*,uint32_t,uint32_t,uint32_t,uint32_t);
int cx_trip_begin(void*);
int cx_net_hit(void*,uint32_t);
int cx_trip_end(void*);
int cx_wave_begin(void*);
int cx_wave_end(void*);
uint32_t cx_pixel_find(CxWorld*,uint32_t,uint32_t);
CxDevice* cx_device_find(CxWorld*,uint32_t,uint32_t);
int cx_interact(CxWorld*);
int cx_interaction_rect(CxWorld*,TerraCircuitWorldCommand*);
int cx_ticks_step(CxWorld*,uint32_t*);
uint32_t cx_vm_available(CxWorld*);
int cx_operation_begin(CxWorld*);
void cx_operation_commit(CxWorld*);
void cx_operation_rollback(CxWorld*);
int cx_seed_reserve(CxWorld*,uint32_t);
int cx_mod_vanilla(CxWorld*,uint32_t,uint32_t,TxTile*);
int cx_override_reserve(CxWorld*,uint32_t);
int cx_lazy_toggle(const TxTile*);
int cx_seed_contains(const CxWorld*,uint32_t,uint32_t);
int cx_inside_wiring(const CxWorld*,uint32_t,uint32_t);
int cx_devices_prepare(CxWorld*);
void cx_devices_end_columns(CxWorld*);

#endif
