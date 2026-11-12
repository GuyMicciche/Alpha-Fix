#include "AlphaFixDecoder.h"
#include <iostream>
#include <memory>
int main(int argc,char**argv){
 if(argc!=2)return 2;
 auto deleter=[](AlphaFixDecoder::Context* p){AlphaFixDecoder::Destroy(p);};
 std::unique_ptr<AlphaFixDecoder::Context,decltype(deleter)> ctx(AlphaFixDecoder::Create(argv[1]),deleter);
 if(!ctx){std::cerr<<"Could not open ProRes alpha fixture\n";return 1;}
 for(int frame:{0,1,0}){
  uint8_t* alpha=nullptr;int32_t width=0,height=0;
  if(AlphaFixDecoder::DecodeAlpha(ctx.get(),frame,&alpha,&width,&height)||!alpha||width!=16||height!=16){std::cerr<<"Decode dimensions/frame failed\n";return 1;}
  for(int i=0;i<width*height;++i)if(alpha[i]<126||alpha[i]>130){std::cerr<<"Unexpected alpha: "<<int(alpha[i])<<'\n';return 1;}
  AlphaFixDecoder::FreeAlphaBuffer(alpha);
 }
 std::cout<<"ProRes 4444 8-bit alpha fixture decoded correctly with minimal FFmpeg (including cached seek)\n";
 return 0;
}
