#include <vector>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <climits>
// Integer lower envelopes preserve the supplied total-order tie rank.
int main(int argc,char**argv){
 if(argc!=3)return 1;
 const int N=1<<24, INF=100000000; std::vector<int>d(N,INF),label(N,INT_MAX);
 std::ifstream in(argv[1]);int rgb,rank;while(in>>rgb>>rank){d[rgb]=0;label[rgb]=std::min(label[rgb],rank);}
 for(int stride: {1,256,65536})for(int outer=0;outer<N;outer+=stride*256)for(int inner=0;inner<stride;++inner){
  int f[256],l[256],v[256],start[256],n=0,base=outer+inner;
  for(int q=0;q<256;++q){f[q]=d[base+q*stride];l[q]=label[base+q*stride];}
  for(int q=0;q<256;++q){if(f[q]==INF)continue;int s=0;
   while(n){int p=v[n-1],a=f[q]+q*q-f[p]-p*p,den=2*(q-p);s=a/den;if(a>0&&a%den)++s; // ceil division, C++ truncation handles negative
    if(a%den==0&&l[q]>l[p])++s;
    if(s>start[n-1])break;--n;
   }
   if(!n)s=0;if(s<256){v[n]=q;start[n++]=s;}
  }
  if(!n)continue;int k=0;for(int x=0;x<256;++x){while(k+1<n&&start[k+1]<=x)++k;int p=v[k];d[base+x*stride]=f[p]+(x-p)*(x-p);label[base+x*stride]=l[p];}
 }
 std::ofstream out(argv[2],std::ios::binary);for(int v:label){uint16_t x=v;out.write((char*)&x,2);}return !out;
}
