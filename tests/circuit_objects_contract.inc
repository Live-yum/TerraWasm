/* Public synthetic WLD 326 contracts; included in the existing native/UBSan job. */
extern uint8_t* tx_alloc(uint32_t);
extern int set_section_override_data(TxWorld*,int,uint8_t*,uint32_t);
static uint8_t* object_file(const char* dir,const char* name,uint32_t* n){char path[1024];snprintf(path,sizeof(path),"%s/%s",dir,name);FILE* f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*n=(uint32_t)ftell(f);rewind(f);uint8_t* b=malloc(*n);assert(b&&fread(b,1,*n,f)==*n);fclose(f);return b;}
static void object_put(uint8_t* p,uint32_t n){for(uint32_t i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void circuit_objects_contract(const char* dir){
 char path[1024];snprintf(path,sizeof(path),"%s/circuit-objects.wld",dir);load(path);uint32_t baseline=tx_native_heap_used(),handle,geometry_size,expected_size;
 uint8_t* geometry_bytes=object_file(dir,"circuit-objects.geometry",&geometry_size),*expected=object_file(dir,"circuit-objects.cob",&expected_size);uint32_t* geometry=alloc(geometry_size);memcpy(geometry,geometry_bytes,geometry_size);free(geometry_bytes);
 assert(tx_get_world(world)->version==326);assert(!terra_circuit_world_begin(world,2,0,0,32u*1024u*1024u,hp));handle=*hp;fragment_pump(handle);
 TerraCircuitWorldCommand command={0};command.abi_version=1;command.kind=7;command.count=4;command.data_ptr=(uint32_t)(uintptr_t)geometry;command.data_count=geometry_size/16;
 assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total==1&&fragment_rows[7]==2);uint32_t fragment=fragment_rows[0],cells=fragment_rows[5];
 command=(TerraCircuitWorldCommand){0};command.abi_version=1;command.kind=8;command.mask=fragment;command.count=cells;command.flags=1;command.aux_source_id=11;command.width=4u*1024u*1024u;command.height=32768;
 assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_objects_size==expected_size&&!memcmp(fragment_objects,expected,expected_size));assert(fragment_total==cells&&fragment_row_count==cells);
 uint32_t* records=malloc(cells*32);assert(records);memcpy(records,fragment_rows,cells*32);for(uint32_t i=0;i<cells;i++){records[i*8]-=3;records[i*8+1]-=4;}
 uint32_t extraction_baseline=tx_native_heap_used();fragment_cancel_objects=1;assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(!fragment_cancel_objects&&tx_native_heap_used()==extraction_baseline);
 assert(!terra_circuit_world_close(handle));tx_free((uint32_t)(uintptr_t)geometry);free(fragment_scratch);fragment_scratch=NULL;fragment_scratch_size=0;assert(tx_native_heap_used()==baseline);
 stamp_objects=fragment_objects;stamp_objects_size=fragment_objects_size;
 char request[256];snprintf(request,sizeof(request),"{\"x\":3,\"y\":20,\"width\":90,\"height\":4,\"recordCount\":%u,\"recordSourceId\":9,\"mode\":\"overlay\",\"objectSourceId\":10,\"objectBytes\":%u,\"objectCount\":18}",cells,stamp_objects_size);
 uint32_t id=begin("stamp_tiles",request),n;uint8_t* output=stamp_pump(id,records,cells*32,&n,0,1);assert(output);check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 for(uint32_t section=2;section<=5;section++){if(section==4)continue;uint32_t prefix=section==5?4:2,old_start=get32(source+26+section*4),old_end=get32(source+30+section*4),start=get32(output+26+section*4),end=get32(output+30+section*4);uint32_t count=section==2?3:section==3?4:11;
  assert((section==5?get32(output+start):(uint32_t)output[start]|((uint32_t)output[start+1]<<8))==count*2);assert(!memcmp(source+old_start+prefix,output+start+prefix,old_end-old_start-prefix));uint32_t at=start+old_end-old_start,off=32,next_id=111;
  for(uint32_t i=0;i<18;i++){const uint8_t* h=expected+off;uint32_t sec=get32(h),kind=get32(h+4),x=3+get32(h+8),y=20+get32(h+12),length=get32(h+20);const uint8_t* payload=h+32;off+=32+length;if(sec!=section)continue;
   if(sec==3){assert(!memcmp(output+at,payload,length));at+=length;assert(get32(output+at)==x&&get32(output+at+4)==y);at+=8;}
   else{if(sec==5){assert(output[at]==kind&&get32(output+at+1)==next_id++);assert(output[at+5]==x&&output[at+6]==0&&output[at+7]==y&&output[at+8]==0);at+=9;}else{assert(get32(output+at)==x&&get32(output+at+4)==y);at+=8;}
    if(sec==5&&kind==0)assert(output[at]==255&&output[at+1]==255);else assert(!memcmp(output+at,payload,length));at+=length;
   }
  }assert(at==end);
 }
 /* Long UTF-8/NUL payloads and every untouched section retain exact bytes. */
 for(uint32_t sec=0;sec<11;sec++)if(sec!=1&&sec!=2&&sec!=3&&sec!=5){uint32_t a=get32(source+26+sec*4),b=sec==10?source_size:get32(source+30+sec*4),c=get32(output+26+sec*4),d=sec==10?n:get32(output+30+sec*4);assert(b-a==d-c&&!memcmp(source+a,output+c,b-a));}
 for(uint32_t which=0;which<6;which++){uint32_t offset=which==0?0:which==1?28:which==2?32+24:which==3?32+16:which==4?32+8:8,value=get32(fragment_objects+offset);object_put(fragment_objects+offset,which==4?90:which==5?87:0xffffffffu);id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,cells*32,&n,2,which&1));check(terra_world_stream_close(id));object_put(fragment_objects+offset,value);assert(tx_native_heap_used()==baseline);}
 /* One missing non-root object tile must not manufacture a partial object. */
 uint32_t changed=0;for(uint32_t i=0;i<cells;i++)if((records[i*8+2]&65535u)==378&&records[i*8+1]==2){changed=i;break;}assert(changed);uint32_t saved=records[changed*8+2];records[changed*8+2]=0;id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,cells*32,&n,2,0));check(terra_world_stream_close(id));records[changed*8+2]=saved;
 id=begin("stamp_tiles",request);check(terra_world_stream_step(id,1,ep));assert(ep->kind==TX_STREAM_NEED_SOURCE&&ep->source_id==10);check(terra_world_stream_acquire_input(id,lp));check(terra_world_stream_cancel(id));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 /* Even an orphan metadata anchor in a non-root destination cell collides. */
 TxWorld* queued=tx_get_world(world);uint32_t te_start=get32(source+46),te_len=get32(source+50)-te_start;uint8_t* orphan=tx_alloc(te_len+14);assert(orphan);memcpy(orphan,source+te_start,te_len);object_put(orphan,12);uint8_t* added=orphan+te_len;memset(added,0,14);added[0]=1;object_put(added+1,50000);added[5]=5;added[7]=20;
 assert(set_section_override_data(queued,5,orphan,te_len+14));id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,cells*32,&n,2,1));check(terra_world_stream_close(id));uint8_t* restored=tx_alloc(te_len);assert(restored);memcpy(restored,source+te_start,te_len);assert(set_section_override_data(queued,5,restored,te_len));
 /* Existing queued section edits participate in the same candidate, not the
  * original file bytes. A remote unrelated sign is retained exactly. */
 uint32_t sign_start=get32(source+38),sign_len=get32(source+42)-sign_start;uint8_t* pending=tx_alloc(sign_len+15);assert(pending);memcpy(pending,source+sign_start,sign_len);pending[0]=5;added=pending+sign_len;added[0]=6;memcpy(added+1,"queued",6);object_put(added+7,98);object_put(added+11,30);assert(set_section_override_data(queued,3,pending,sign_len+15));
 baseline=tx_native_heap_used();id=begin("stamp_tiles",request);uint8_t* queued_output=stamp_pump(id,records,cells*32,&n,0,1);assert(queued_output);uint32_t qs=get32(queued_output+38);assert(queued_output[qs]==9&&!memcmp(queued_output+qs+2,pending+2,sign_len+13));free(queued_output);check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 /* Same-coordinate append is rejected; the destination keeps its inventory. */
 check(terra_world_close(world));world=0;uint32_t size=get32(output+26+10*4)+(source_size-get32(source+26+10*4));check(terra_world_open_from_buffer(output,size,&world));baseline=tx_native_heap_used();id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,cells*32,&n,2,1));check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);check(terra_world_close(world));world=0;
 free(source);source=NULL;free(output);assert(!tx_native_heap_used());
 snprintf(path,sizeof(path),"%s/circuit-objects-old.wld",dir);load(path);
 id=begin("stamp_tiles",request);assert(!stamp_pump(id,records,cells*32,&n,2,1));check(terra_world_stream_close(id));
 uint8_t empty[32]={0};object_put(empty,0x31424f43);object_put(empty+4,1);object_put(empty+8,196);object_put(empty+16,32);stamp_objects=empty;stamp_objects_size=32;uint32_t wire[8]={0};wire[5]=1u<<24;
 id=begin("stamp_tiles","{\"x\":3,\"y\":20,\"width\":1,\"height\":1,\"recordCount\":1,\"recordSourceId\":9,\"mode\":\"overlay\",\"objectSourceId\":10,\"objectBytes\":32,\"objectCount\":0}");uint8_t* old_output=stamp_pump(id,wire,sizeof(wire),&n,0,1);assert(old_output);free(old_output);check(terra_world_stream_close(id));check(terra_world_close(world));world=0;free(source);source=NULL;
 free(records);free(expected);free(fragment_objects);fragment_objects=NULL;fragment_objects_size=0;stamp_objects=NULL;stamp_objects_size=0;assert(!tx_native_heap_used());
 puts("circuit objects: all 18 tiles/11 entities, long UTF-8, exact inventory/state, duplicate targets, malformed data, partial objects and cancel passed");
}
static void circuit_object_versions_stream_contract(const char* dir){
 const uint32_t versions[]={88,115,116,121,122,196,208,210,269,293,294,306,307,308,311,312,326};
 for(size_t v=0;v<sizeof versions/sizeof versions[0];v++){
  uint32_t version=versions[v],table=version>=135?26:6;char name[128],path[1024];
  snprintf(path,sizeof(path),"%s/circuit-objects-v%u.wld",dir,version);load(path);
  uint32_t baseline=tx_native_heap_used(),geometry_size,expected_size;
  snprintf(name,sizeof(name),"circuit-objects-v%u.geometry",version);uint8_t* geometry_bytes=object_file(dir,name,&geometry_size);
  snprintf(name,sizeof(name),"circuit-objects-v%u.cob",version);uint8_t* expected=object_file(dir,name,&expected_size);
  uint32_t* geometry=alloc(geometry_size);memcpy(geometry,geometry_bytes,geometry_size);free(geometry_bytes);
  assert(!terra_circuit_world_begin(world,2,0,0,32u*1024u*1024u,hp));uint32_t handle=*hp;fragment_pump(handle);
  TerraCircuitWorldCommand command={0};command.abi_version=1;command.kind=TCW_FRAGMENTS;command.count=4;command.data_ptr=(uint32_t)(uintptr_t)geometry;command.data_count=geometry_size/16;
  assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);assert(fragment_total==1&&fragment_rows[7]==2);
  uint32_t cells=fragment_rows[5],fragment=fragment_rows[0];
  command=(TerraCircuitWorldCommand){0};command.abi_version=1;command.kind=TCW_EXTRACT;command.mask=fragment;command.count=cells;command.flags=1;command.aux_source_id=11;command.width=4u*1024u*1024u;command.height=32768;
  assert(!terra_circuit_world_command(handle,&command));fragment_pump(handle);
  assert(fragment_objects_size==expected_size&&!memcmp(fragment_objects,expected,expected_size));
  uint32_t* records=malloc(cells*32);assert(records);memcpy(records,fragment_rows,cells*32);
  for(uint32_t i=0;i<cells;i++){records[i*8]-=3;records[i*8+1]-=4;}
  assert(!terra_circuit_world_close(handle));tx_free((uint32_t)(uintptr_t)geometry);free(fragment_scratch);fragment_scratch=NULL;fragment_scratch_size=0;
  assert(tx_native_heap_used()==baseline);stamp_objects=fragment_objects;stamp_objects_size=fragment_objects_size;
  char request[256];snprintf(request,sizeof request,"{\"x\":3,\"y\":20,\"width\":22,\"height\":4,\"recordCount\":%u,\"recordSourceId\":9,\"mode\":\"overlay\",\"objectSourceId\":10,\"objectBytes\":%u,\"objectCount\":%u}",cells,stamp_objects_size,get32(expected+12));
  uint32_t id=begin("stamp_tiles",request),n;uint8_t* output=stamp_pump(id,records,cells*32,&n,0,1);assert(output);
  check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
  if(version==115||version==121||version==306||version==307){
   uint32_t incompatible_size;snprintf(name,sizeof name,"circuit-objects-v%u.cob",version==115?116:308);
   uint8_t* incompatible=object_file(dir,name,&incompatible_size);stamp_objects=incompatible;stamp_objects_size=incompatible_size;
   snprintf(request,sizeof request,"{\"x\":3,\"y\":20,\"width\":22,\"height\":4,\"recordCount\":%u,\"recordSourceId\":9,\"mode\":\"overlay\",\"objectSourceId\":10,\"objectBytes\":%u,\"objectCount\":%u}",cells,incompatible_size,get32(incompatible+12));
   id=begin("stamp_tiles",request);uint32_t failed_size;assert(!stamp_pump(id,records,cells*32,&failed_size,2,1));
   check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);free(incompatible);
   stamp_objects=fragment_objects;stamp_objects_size=fragment_objects_size;
  }
  assert(get32(output)==version);
  uint32_t pointer_count=source[table-2]|((uint32_t)source[table-1]<<8);
  for(uint32_t section=0;section<pointer_count;section++){
   if(section==1)continue;
   uint32_t old_start=get32(source+table+section*4),old_end=section+1<pointer_count?get32(source+table+(section+1)*4):source_size;
   uint32_t start=get32(output+table+section*4),end=section+1<pointer_count?get32(output+table+(section+1)*4):n;
   if(section==2||section==3||(section==5&&version>=116)){
    uint32_t append_size;snprintf(name,sizeof name,"circuit-objects-v%u.append.%u",version,section);uint8_t* appended=object_file(dir,name,&append_size);
    uint32_t prefix=section==5||(section==2&&version<294)?4:2;
    assert(end-start==old_end-old_start+append_size-prefix);
    assert(!memcmp(source+old_start+prefix,output+start+prefix,old_end-old_start-prefix));
    assert(!memcmp(output+start+old_end-old_start,appended+prefix,append_size-prefix));
    if(section==2&&version<294)assert(output[start+2]==40&&output[start+3]==0);
    free(appended);
   }else assert(end-start==old_end-old_start&&!memcmp(source+old_start,output+start,end-start));
  }
  check(terra_world_close(world));world=0;check(terra_world_open_from_buffer(output,n,&world));assert(tx_get_world(world)->version==version);check(terra_world_close(world));world=0;
  free(output);free(source);source=NULL;free(expected);free(records);free(fragment_objects);fragment_objects=NULL;fragment_objects_size=0;stamp_objects=NULL;stamp_objects_size=0;
  assert(!tx_native_heap_used());
 }
 puts("circuit object public extraction/stamp: 17 sectioned versions, exact metadata append and unchanged sections passed");
}
