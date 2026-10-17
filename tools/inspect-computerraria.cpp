#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Read-only inventory of the raw vanilla world tile stream. The layout follows
// Terraria.IO/WorldFile.cs at 8255d34616c780af12079425ac92a0a7aed87d71,
// LoadFileFormatHeader, LoadHeader and LoadWorldTiles. No .twld data is merged.
struct Reader {
  const uint8_t *p, *start, *end;
  void skip(size_t bytes) { if (bytes > size_t(end-p)) throw std::runtime_error("truncated world"); p+=bytes; }
  template<class T> T get() { if (sizeof(T) > size_t(end-p)) throw std::runtime_error("truncated world"); T v; std::memcpy(&v,p,sizeof v); p += sizeof v; return v; }
  std::string str() { uint32_t n=0;unsigned shift=0;uint8_t b;do { b=get<uint8_t>();if(shift==28&&(b&0xf0))throw std::runtime_error("bad string");n|=uint32_t(b&127)<<shift;shift+=7;}while(b&128); if(n>size_t(end-p))throw std::runtime_error("bad string length"); std::string s((const char*)p,n);p+=n;return s; }
};
std::string jsonString(const std::string& value) {
  static const char* hex="0123456789abcdef";std::string result="\"";
  for(unsigned char c:value){if(c=='"'||c=='\\'){result+='\\';result+=c;}else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}else result+=c;}
  return result+'"';
}
struct Tile { int type=-1,fx=-1,fy=-1,mask=0; };
struct Stats { uint64_t cells=0,wired=0; };
int main(int argc,char**argv) {
  if(argc!=2) { std::cerr<<"usage: inspect-computerraria file.wld\n";return 2; }
  try {
  int fd=open(argv[1],O_RDONLY);if(fd<0)throw std::runtime_error("cannot open world");struct stat st;
  if(fstat(fd,&st)<0||st.st_size<32){close(fd);throw std::runtime_error("invalid world size");}
  auto data=(const uint8_t*)mmap(nullptr,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
  if(data==MAP_FAILED){close(fd);throw std::runtime_error("cannot map world");}
  Reader r{data,data,data+st.st_size};
  auto version=r.get<int32_t>();if(version!=279)throw std::runtime_error("this pinned Computerraria inventory requires world format 279");
  r.skip(20);auto num=r.get<int16_t>();if(num<3||num>256)throw std::runtime_error("invalid section count");std::vector<int32_t> positions;
  while(num--){auto offset=r.get<int32_t>();if(offset<0||offset>st.st_size||(!positions.empty()&&offset<positions.back()))throw std::runtime_error("invalid section offset");positions.push_back(offset);}
  auto importance_count=r.get<uint16_t>();std::vector<bool> important;uint8_t bits=0;
  for(int i=0;i<importance_count;i++){if(i%8==0)bits=r.get<uint8_t>();important.push_back((bits>>(i%8))&1);}
  if(r.p-data!=positions[0])throw std::runtime_error("header boundary mismatch");
  auto name=r.str();auto seed=r.str();r.skip(8+16);auto id=r.get<int32_t>();r.skip(16);auto height=r.get<int32_t>(),width=r.get<int32_t>();
  if(width<=0||height<=0||width>65536||height>65536)throw std::runtime_error("invalid world dimensions");
  r.p=data+positions[1];
  uint64_t records=0,active=0,wire_cells=0,actuator=0,inactive=0,standard_faulty=0;
  std::array<uint64_t,16> masks{};std::array<uint64_t,4> colors{};
  std::map<int,Stats> types;std::map<int,uint64_t> gate_styles,gate_frames,lamp_frames;
  int minx=width,miny=height,maxx=-1,maxy=-1;
  for(int x=0;x<width;x++){
    Tile prev,prev2;
    for(int y=0;y<height;){
      ++records;Tile t;uint8_t a=r.get<uint8_t>(),b=0,c=0,d=0;
      if(a&1)b=r.get<uint8_t>();if(b&1)c=r.get<uint8_t>();if(c&1)d=r.get<uint8_t>();
      if(a&2){t.type=(a&32)?r.get<uint16_t>():r.get<uint8_t>();if(t.type>=importance_count)throw std::runtime_error("tile id exceeds flags");if(important[t.type]){t.fx=r.get<int16_t>();t.fy=r.get<int16_t>();}if(c&8)r.get<uint8_t>();}
      if(a&4){r.get<uint8_t>();if(c&16)r.get<uint8_t>();}
      if(a&24)r.get<uint8_t>();
      t.mask=((b>>1)&7)|((c&32)?8:0);
      if(c&64)r.get<uint8_t>();
      int mode=a>>6;int repeat=mode==0?0:mode==1?r.get<uint8_t>():r.get<int16_t>();int n=repeat+1;
      if(n<1||y+n>height)throw std::runtime_error("invalid RLE");
      masks[t.mask]+=n;if(t.mask){wire_cells+=n;minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y+n-1);for(int k=0;k<4;k++)if(t.mask&(1<<k))colors[k]+=n;}
      if(c&2)actuator+=n;if(c&4)inactive+=n;
      if(t.type>=0){active+=n;types[t.type].cells+=n;if(t.mask)types[t.type].wired+=n;}
      if(t.type==419)lamp_frames[t.fx]+=n;
      if(t.type==420){gate_styles[t.fy/18]+=n;gate_frames[t.fx]+=n;if(t.fx==36&&prev.type==419&&prev.fx!=36&&prev2.type==419&&prev2.fx==36)++standard_faulty;}
      if(n>=2){prev2=t;prev=t;}else{prev2=prev;prev=t;}
      y+=n;
    }
  }
  if(r.p-data!=positions[2])throw std::runtime_error("tile section boundary mismatch");
  std::cout<<"{\n\"format_version\":"<<version<<",\n\"name\":"<<jsonString(name)<<",\n\"world_id\":"<<id<<",\n\"width\":"<<width<<",\n\"height\":"<<height<<",\n\"world_cells\":"<<uint64_t(width)*height<<",\n\"file_bytes\":"<<st.st_size<<",\n\"tile_section_bytes\":"<<positions[2]-positions[1]<<",\n\"tile_records\":"<<records<<",\n\"active_tiles\":"<<active<<",\n\"wire_cells\":"<<wire_cells<<",\n\"wire_colors\":[";
  for(int i=0;i<4;i++){if(i)std::cout<<",";std::cout<<colors[i];}std::cout<<"],\n\"wire_masks\":[";for(int i=0;i<16;i++){if(i)std::cout<<",";std::cout<<masks[i];}
  std::cout<<"],\n\"wire_bounds\":["<<minx<<","<<miny<<","<<maxx<<","<<maxy<<"],\n\"actuator_cells\":"<<actuator<<",\n\"inactive_cells\":"<<inactive<<",\n\"standard_faulty_stacks\":"<<standard_faulty<<",\n\"gate_styles\":{";
  bool first=true;for(auto [k,v]:gate_styles){if(!first)std::cout<<",";first=false;std::cout<<"\""<<k<<"\":"<<v;}std::cout<<"},\n\"gate_frames\":{";
  first=true;for(auto [k,v]:gate_frames){if(!first)std::cout<<",";first=false;std::cout<<"\""<<k<<"\":"<<v;}std::cout<<"},\n\"lamp_frames\":{";
  first=true;for(auto [k,v]:lamp_frames){if(!first)std::cout<<",";first=false;std::cout<<"\""<<k<<"\":"<<v;}std::cout<<"},\n\"tile_types\":{";
  first=true;for(auto [k,v]:types){if(!first)std::cout<<",";first=false;std::cout<<"\""<<k<<"\":{\"cells\":"<<v.cells<<",\"wired\":"<<v.wired<<"}";}std::cout<<"},\n\"tile_section_boundary_verified\":true\n}\n";
  munmap((void*)data,st.st_size);close(fd);
  return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}
