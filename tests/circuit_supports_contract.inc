/* Paired with the WASM acceptance test. Private user worlds are never fixtures. */
static uint32_t* support_row(uint32_t x,uint32_t y) {
    for(uint32_t i=0;i<fragment_row_count;i++)if(fragment_rows[i*8]==x&&fragment_rows[i*8+1]==y)return fragment_rows+i*8;
    return NULL;
}
static void circuit_supports_contract(const char* directory) {
    char path[1024];snprintf(path,sizeof(path),"%s/circuit-supports.wld",directory);load(path);
    uint32_t baseline=tx_native_heap_used();assert(!terra_circuit_world_begin(world,2,0,0,32u*1024u*1024u,hp));uint32_t handle=*hp;fragment_pump(handle);
    snprintf(path,sizeof(path),"%s/circuit-supports.geometry",directory);FILE* file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);uint32_t bytes=ftell(file);rewind(file);uint32_t* geometry=alloc(bytes);assert(fread(geometry,1,bytes,file)==bytes);fclose(file);
    TerraCircuitWorldCommand cmd={0};cmd.abi_version=1;cmd.kind=TCW_FRAGMENTS;cmd.count=32;cmd.data_ptr=(uint32_t)(uintptr_t)geometry;cmd.data_count=bytes/16;
    assert(!terra_circuit_world_command(handle,&cmd));fragment_pump(handle);assert(fragment_total==11&&fragment_row_count==11);
    uint32_t descriptors[11*8];memcpy(descriptors,fragment_rows,sizeof(descriptors));
    for(uint32_t i=0;i<11;i++) {
        uint32_t* d=descriptors+i*8;assert(!(d[7]&9u));
        cmd=(TerraCircuitWorldCommand){0};cmd.abi_version=1;cmd.kind=TCW_EXTRACT;cmd.mask=d[0];cmd.count=64;
        assert(!terra_circuit_world_command(handle,&cmd));fragment_pump(handle);assert(fragment_row_count==d[5]);
        if(d[1]==2&&d[2]==6) {
            assert(d[5]==12&&d[6]==11);assert(support_row(4,7)&&(support_row(4,7)[2]&65535)==54);
            assert((support_row(4,7)[4]>>16)==7);assert((support_row(4,6)[2]&65535)==135);assert((support_row(12,6)[2]&65535)==137);
            for(uint32_t x=5;x<=10;x++){uint32_t* r=support_row(x,6);assert(r&&!(r[2]&65536)&&!r[4]&&!(r[5]&0xffffff));}
        } else if(d[1]==8&&d[2]==3) { assert(d[5]==7);for(uint32_t n=0;n<fragment_row_count;n++)assert(fragment_rows[n*8+5]>>24==2); }
        else if(d[1]==2&&d[2]==13) { assert(d[5]==14);assert((support_row(10,13)[2]&65535)==138);assert((support_row(11,14)[2]&65535)==138);assert((support_row(10,15)[2]&65535)==130);assert(support_row(11,15)); }
        else if(d[1]==20&&d[2]==10) { assert(d[5]==10);for(uint32_t x=20;x<=22;x++){assert((support_row(x,11)[2]&65535)==14);assert((support_row(x,12)[2]&65535)==14);assert((support_row(x,13)[2]&65535)==54);} }
        else if(d[1]==2&&d[2]==19) { assert(d[5]==5);assert(support_row(2,19)&&support_row(2,23)&&!support_row(3,21)); }
        else if(d[1]==7) { assert(d[5]==2);assert(support_row(7,21)&&!support_row(9,21)); }
        else if(d[1]==12&&d[2]==20) { assert(d[5]==4);for(uint32_t n=0;n<4;n++)assert(fragment_rows[n*8+4]==(2u|(9u<<24))); }
        else if(d[1]==20&&d[2]==19) { assert(d[5]==10);assert(support_row(21,19)&&!support_row(20,19)&&!support_row(22,19)); }
        else if(d[1]==28&&d[2]==20) { assert(d[5]==14);assert(support_row(29,23)&&support_row(30,23)&&!support_row(28,23)&&!support_row(31,23)); }
        else if(d[1]==28&&d[2]==4) { assert(d[5]==6&&d[7]==2);assert(support_row(28,6)&&support_row(29,6)); }
        else if(d[1]==36) { assert(d[5]==1);assert(support_row(36,12)[4]==(2u|(5u<<16)|(9u<<24)));assert(!support_row(37,12)); }
        else assert(!"unexpected support fragment");
        if(d[5]>1){cmd.count=d[5]-1;assert(terra_circuit_world_command(handle,&cmd)==TCW_MEMORY);}
    }
    assert(!terra_circuit_world_close(handle));assert(tx_native_heap_used()==baseline);tx_free((uint32_t)(uintptr_t)geometry);
    free(fragment_scratch);fragment_scratch=NULL;fragment_scratch_size=0;check(terra_world_close(world));world=0;free(source);source=NULL;
    puts("circuit supports: natural traps, terrain filtering, doors, source materials, wall alternatives, recursive tables, exact anchors and bounds passed");
}
static void circuit_anchor_edges_contract(const char* directory) {
    char path[1024];snprintf(path,sizeof(path),"%s/circuit-anchor-edges.wld",directory);load(path);
    uint32_t baseline=tx_native_heap_used();assert(!terra_circuit_world_begin(world,2,0,0,32u*1024u*1024u,hp));uint32_t handle=*hp;fragment_pump(handle);
    snprintf(path,sizeof(path),"%s/circuit-anchor-edges.geometry",directory);FILE* file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);uint32_t bytes=ftell(file);rewind(file);uint32_t* geometry=alloc(bytes);assert(fread(geometry,1,bytes,file)==bytes);fclose(file);
    TerraCircuitWorldCommand cmd={0};cmd.abi_version=1;cmd.kind=TCW_FRAGMENTS;cmd.count=32;cmd.data_ptr=(uint32_t)(uintptr_t)geometry;cmd.data_count=bytes/16;
    assert(!terra_circuit_world_command(handle,&cmd));fragment_pump(handle);assert(fragment_total==15&&fragment_row_count==15);
    const uint32_t expected[][4]={{4,4,4,0},{12,5,1,8},{17,5,2,0},{24,4,3,0},{30,4,3,0},{36,4,5,0},{42,4,4,10},{50,3,3,0},{55,4,2,8},{60,3,2,0},{4,15,4,0},{12,16,2,0},{18,16,1,8},{24,16,1,8},{29,15,4,0}};
    uint32_t descriptors[15*8];memcpy(descriptors,fragment_rows,sizeof(descriptors));
    for(uint32_t i=0;i<15;i++) {
        const uint32_t* e=expected[i];uint32_t* d=NULL;
        for(uint32_t j=0;j<15;j++)if(descriptors[j*8+1]==e[0]&&descriptors[j*8+2]==e[1])d=descriptors+j*8;
        if(!d||d[5]!=e[2]||d[7]!=e[3])fprintf(stderr,"anchor edge mismatch at %u,%u: cells=%u flags=%u\n",e[0],e[1],d?d[5]:0,d?d[7]:0);
        assert(d&&d[5]==e[2]&&d[7]==e[3]);
        cmd=(TerraCircuitWorldCommand){0};cmd.abi_version=1;cmd.kind=TCW_EXTRACT;cmd.mask=d[0];cmd.count=d[5];
        assert(!terra_circuit_world_command(handle,&cmd));fragment_pump(handle);assert(fragment_row_count==d[5]);
        if(i==0||i==10||i==14){uint32_t x=i==14?29u:4u,y=i==0?4u:15u;for(uint32_t dy=0;dy<3;dy++)assert((support_row(x,y+dy)[2]&65535u)==5u);}
        if(i==2)assert((support_row(17,5)[2]&65535u)==124u);
        if(i==3||i==4)assert((support_row(e[0],4)[2]&65535u)==(i==3?427u:435u));
        if(i==5){assert((support_row(36,6)[2]&65535u)==130u);assert(!support_row(37,6));}
        if(i==6)assert(!support_row(42,6));
        if(i==7)assert((support_row(50,3)[2]&65535u)==1u);
        if(i==8)assert(!support_row(55,3));
        if(i==9)assert((support_row(60,3)[2]&65535u)==225u);
        if(i==11)assert((support_row(12,17)[2]&65535u)==435u);
    }
    assert(!terra_circuit_world_close(handle));assert(tx_native_heap_used()==baseline);tx_free((uint32_t)(uintptr_t)geometry);
    free(fragment_scratch);fragment_scratch=NULL;fragment_scratch_size=0;check(terra_world_close(world));world=0;free(source);source=NULL;
    puts("circuit anchor edges: three-cell trees, beams, platform alternates, boulder single holds, table restrictions and natural stalactite materials passed");
}
static void circuit_falling_contract(const char* directory) {
    char path[1024];snprintf(path,sizeof(path),"%s/circuit-falling.wld",directory);load(path);
    uint32_t baseline=tx_native_heap_used();assert(!terra_circuit_world_begin(world,2,0,0,32u*1024u*1024u,hp));uint32_t handle=*hp;fragment_pump(handle);
    snprintf(path,sizeof(path),"%s/circuit-falling.geometry",directory);FILE* file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);uint32_t bytes=ftell(file);rewind(file);uint32_t* geometry=alloc(bytes);assert(fread(geometry,1,bytes,file)==bytes);fclose(file);
    TerraCircuitWorldCommand cmd={0};cmd.abi_version=1;cmd.kind=TCW_FRAGMENTS;cmd.count=32;cmd.data_ptr=(uint32_t)(uintptr_t)geometry;cmd.data_count=bytes/16;
    assert(!terra_circuit_world_command(handle,&cmd));fragment_pump(handle);assert(fragment_total==9&&fragment_row_count==9);
    const uint32_t expected[][4]={{5,5,44,0},{15,5,3,0},{20,5,1,8},{25,5,1,0},{30,5,1,8},{35,5,6,2},{42,5,3,0},{48,5,2,8},{53,5,2,0}};
    uint32_t descriptors[9*8];memcpy(descriptors,fragment_rows,sizeof(descriptors));
    for(uint32_t i=0;i<9;i++) {
        const uint32_t* e=expected[i];uint32_t* d=NULL;
        for(uint32_t j=0;j<9;j++)if(descriptors[j*8+1]==e[0]&&descriptors[j*8+2]==e[1])d=descriptors+j*8;
        if(!d||d[5]!=e[2]||d[7]!=e[3])fprintf(stderr,"falling edge mismatch at %u,%u: cells=%u flags=%u\n",e[0],e[1],d?d[5]:0,d?d[7]:0);
        assert(d&&d[5]==e[2]&&d[7]==e[3]);
        cmd=(TerraCircuitWorldCommand){0};cmd.abi_version=1;cmd.kind=TCW_EXTRACT;cmd.mask=d[0];cmd.count=d[5];
        assert(!terra_circuit_world_command(handle,&cmd));fragment_pump(handle);assert(fragment_row_count==d[5]);
        if(i==0){for(uint32_t y=6;y<=47;y++)assert((support_row(5,y)[2]&65535u)==123u);assert((support_row(5,48)[2]&65535u)==1u);assert(!support_row(6,48));}
        if(i==1){assert((support_row(15,7)[2]&65535u)==1u);assert(!support_row(15,8));}
        if(i==2||i==3||i==4)assert(!support_row(e[0],6));
        if(i==3)assert(support_row(25,5)[2]&(4u<<16));
        if(i==5){assert((support_row(35,5)[2]&65535u)==21u);assert((support_row(36,6)[2]&65535u)==21u);assert(!support_row(35,8));}
        if(i==6){assert((support_row(42,6)[2]&65535u)==136u);assert((support_row(42,7)[2]&65535u)==1u);}
        if(i==7)assert((support_row(48,6)[2]&65535u)==53u);
        if(i==8)assert(!support_row(54,6));
    }
    assert(!terra_circuit_world_close(handle));assert(tx_native_heap_used()==baseline);tx_free((uint32_t)(uintptr_t)geometry);
    free(fragment_scratch);fragment_scratch=NULL;fragment_scratch_size=0;check(terra_world_close(world));world=0;free(source);source=NULL;
    puts("circuit falling supports: long sparse chains, exact stopping cells, inactive gaps, unsupported paths, holders and two-cell stops passed");
}
