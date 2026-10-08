/* Compare the streamed prepared image against the public buffer renderer,
 * which has no prepared output and retains its original projection formula.
 * The rich fixture includes enough records per 32px bucket to saturate the
 * alpha counter, frame-important tiles, paint, walls, liquids and empty rows. */
static void stream_preview_fixture(const char*path,const uint32_t*widths,uint32_t count){
 load(path);uint8_t*expected[3]={0};uint32_t sizes[3]={0};
 uint32_t baseline=tx_native_heap_used();
 const uint32_t units[]={1,16,64};
 for(uint32_t i=0;i<count;i++)for(uint32_t step=0;step<3;step++){
  char request[64];snprintf(request,sizeof(request),"{\"max_w\":%u}",widths[i]);
  pump_work_units=units[step];uint32_t id=begin("render_thumbnail_png",request),n;
  uint8_t*png=pump(id,1,&n);assert(png&&sp->tile_scan_passes==1&&sp->input_copy_bytes==0);
  assert(sp->decoded_tile_bytes==get32(source+34)-get32(source+30));
  if(!step){expected[i]=png;sizes[i]=n;}
  else{assert(n==sizes[i]&&!memcmp(png,expected[i],n));free(png);}
  check(terra_world_stream_close(id));assert(tx_native_heap_used()==baseline);
 }
 pump_work_units=1;
 check(terra_world_close(world));check(terra_world_open_from_buffer(source,source_size,&world));
 for(uint32_t i=0;i<count;i++){
  char request[64],response[256];uint64_t required=0;uint32_t width,height;
  snprintf(request,sizeof(request),"{\"max_w\":%u}",widths[i]);
  check(terra_op_execute_json(world,"render_thumbnail_png",request,response,sizeof(response),&required));
  assert(terra_op_get_thumbnail_png(world,NULL,0,&required,&width,&height)==TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL);
  uint8_t*png=malloc(required);assert(png);
  check(terra_op_get_thumbnail_png(world,png,required,&required,&width,&height));
  assert(required==sizes[i]&&!memcmp(png,expected[i],sizes[i]));free(png);free(expected[i]);
 }
 check(terra_world_close(world));free(source);source=NULL;world=0;
}
static void stream_preview_contract(const char*directory){
 char path[1024];const uint32_t widths[]={32,256,960},narrow[]={16};
 snprintf(path,sizeof(path),"%s/preview-rich.wld",directory);stream_preview_fixture(path,widths,3);
 for(uint32_t height=8192;height<=8193;height++){
  snprintf(path,sizeof(path),"%s/preview-tall-%u.wld",directory,height);stream_preview_fixture(path,narrow,1);
 }
 puts("stream preview projection: 32/256/960px and row-cache boundary match buffer PNG bytes at work units 1/16/64");
}
