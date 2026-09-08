#include "fast/rt64_fast_state.h"
#include "fast/rt64_fast_profile.h"
#include <cstdio>
#include <string>

namespace VertexInputControl {
namespace {
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
struct Sink final : RT64::FastDrawSink {
    void draw(const RT64::FastDraw &) override {}
    void fullSync() override {}
    void present(uint32_t) override {}
};
void put16(uint8_t *memory,uint32_t address,uint16_t value) {
    memory[address^3]=uint8_t(value>>8);
    memory[(address+1)^3]=uint8_t(value);
}
void profileCheck(RT64::State &state) {
    using namespace RT64::FastProfile;
    const auto found=std::find_if(std::begin(counterNames),std::end(counterNames),
        [](const char *name){return std::string(name)=="vertex_bulk_records";});
    check(found!=std::end(counterNames),"Bulk vertex-input decoding is not recorded");
    Context context{+[]()->uint64_t{return 1;}};
    struct Reset { Context *old=current;~Reset(){current=old;} } reset;
    current=&context;
    state.rsp->setVertex(0,32,0);
    state.rsp->setVertex(8,7,17);
    state.rsp->setVertex(0,0,256);
#if defined(RT64_FAST_REFERENCE_VERTEX_INPUT)
    constexpr uint64_t expected=0;
#else
    const uint32_t endian=1;
    const uint64_t expected=*reinterpret_cast<const uint8_t *>(&endian)==1?39:0;
#endif
    check(context.stats.counters[size_t(found-std::begin(counterNames))]==expected,
        "Eligible vertices did not use the requested input-decoding path");
}
}
int run(bool profile,FILE *output,FILE *errors) {
    try {
        std::vector<uint32_t> memory(8192/4);
        auto *rdram=reinterpret_cast<uint8_t *>(memory.data());
        Sink sink;
        RT64::State state{rdram,memory.size()*4,sink};
        auto &rsp=*state.rsp;
        rsp.combinedChanged=false;
        rsp.scaleS=0.5f;rsp.scaleT=0.25f;
        for(uint32_t base=0;base<65536;base+=32) {
            const uint32_t address=(base&32)?8:0;
            for(uint32_t i=0;i<32;++i) {
                const uint32_t src=address+i*16;
                const uint16_t n=uint16_t(base+i);
                put16(rdram,src,n);put16(rdram,src+2,n^0x8001);
                put16(rdram,src+4,uint16_t(~n));put16(rdram,src+6,n^0x5a5a);
                put16(rdram,src+8,uint16_t(n*97));put16(rdram,src+10,uint16_t(n*13));
                for(unsigned c=0;c<4;++c)rdram[(src+12+c)^3]=uint8_t(n*7+c*67);
            }
            rsp.setVertex(address,32,17);
            for(uint32_t i=0;i<32;++i) {
                const auto &v=rsp.vertices[17+i];
                const uint16_t n=uint16_t(base+i);
                const float p[3]={float(int16_t(n)),float(int16_t(n^0x8001)),float(int16_t(uint16_t(~n)))};
                for(unsigned c=0;c<3;++c)check(v.position[c]==p[c],"Packed input changed vertex position");
                check(v.position[3]==1,"Packed input changed the homogeneous coordinate");
                check(v.uv[0]==float(int16_t(uint16_t(n*97)))/32.0f*rsp.scaleS,"Packed input changed S");
                check(v.uv[1]==float(int16_t(uint16_t(n*13)))/32.0f*rsp.scaleT,"Packed input changed T");
                for(unsigned c=0;c<4;++c)check(v.color[c]==uint8_t(n*7+c*67)/255.0f,"Packed input changed color or alpha");
                check(v.fog==v.color[3],"Packed input changed cached fog");
                check(rsp.vertexValid[17+i],"Loaded vertex was not marked valid");
                const auto &screen=rsp.screenPositions[17+i];
                check(screen[0]==p[0]*160+160 && screen[1]==-p[1]*120+120 && screen[2]==p[2]*0.5f+0.5f,
                    "Packed input changed the cached viewport transform");
            }
        }
        const auto vertices=rsp.vertices;
        const auto valid=rsp.vertexValid;
        const auto screen=rsp.screenPositions;
        for(const auto request:std::array<std::array<uint32_t,3>,3>{{{8184,1,0},{0,1,256},{0,2,255}}}) {
            bool rejected=false;
            try {rsp.setVertex(request[0],request[1],request[2]);}catch(const std::exception &){rejected=true;}
            check(rejected,"Invalid vertex range was accepted");
            check(std::memcmp(vertices.data(),rsp.vertices.data(),sizeof(vertices))==0 && valid==rsp.vertexValid && screen==rsp.screenPositions,
                "Invalid input changed the vertex cache");
        }
        if(profile)profileCheck(state);
        std::fprintf(output,"Vertex input: 65536 exact records, both alignments and invalid-range preservation passed\n");
        return 0;
    } catch(const std::exception &error) {std::fprintf(errors,"%s\n",error.what());return 1;}
}
}
#ifndef RT64_VERTEX_INPUT_CONTROL
int main(int argc,char **argv) {
    return VertexInputControl::run(argc>1 && std::string(argv[1])=="--expect-input-profile",stdout,stderr);
}
#endif
