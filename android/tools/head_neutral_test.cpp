#include "../compat/include/thirdparty-stubs/head_neutral.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
static void word(std::vector<unsigned char> &b,size_t p,uint32_t n) {
    for(int i=0;i<4;++i)b[p+i]=(n>>(8*i))&255;
}
static void point(std::vector<unsigned char> &b,size_t p,float x,float y,float z) {
    float v[]={x,y,z}; for(int i=0;i<3;++i){uint32_t u;std::memcpy(&u,v+i,4);word(b,p+4*i,u);}
}
int main(int argc,char **argv) {
    std::vector<A5Head::Point> points;
    for(bool old : {false,true}) {
        const size_t header=old?28:36, moving=old?28:20, fixed=old?24:16;
        std::vector<unsigned char> data(header+moving+fixed,0);
        word(data,0,old?0xad5a018d:0x37d30dc0);word(data,8,2);word(data,12,1);
        word(data,header,1);point(data,header+(old?12:4),2,-3,4);
        word(data,header+moving,0);point(data,header+moving+(old?12:4),-5,6,7);
        assert(A5Head::Load(data.data(),data.size(),points));
        assert(points.size()==2 && points[0].x==-5 && points[1].y==-3);
        for(size_t len=0;len<data.size();++len)assert(!A5Head::Load(data.data(),len,points) && points.empty());
        word(data,header+moving,1);assert(!A5Head::Load(data.data(),data.size(),points));
        word(data,header+moving,0);word(data,header+moving+(old?12:4),0x7fc00000);
        assert(!A5Head::Load(data.data(),data.size(),points));
        word(data,8,0xffffffff);assert(!A5Head::Load(data.data(),data.size(),points));
    }
    size_t total=0;
    for(int i=1;i<argc;++i) {
        std::ifstream f(argv[i],std::ios::binary);
        std::vector<char> b((std::istreambuf_iterator<char>(f)),{});
        if(!A5Head::Load(b.data(),b.size(),points)){std::fprintf(stderr,"Failed: %s\n",argv[i]);return 1;}
        bool nonzero=false;for(const auto &p:points) nonzero |= std::fabs(p.x)+std::fabs(p.y)+std::fabs(p.z)>0.1f;
        assert(nonzero);total+=points.size();
    }
    std::printf("neutral heads: both formats, indices, truncation, NaN and counts passed; corpus %d streams / %zu vertices\n",argc-1,total);
}
