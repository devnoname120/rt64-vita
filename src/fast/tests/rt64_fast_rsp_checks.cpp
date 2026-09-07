#include "fast/rt64_fast_interpreter.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <new>

namespace {
thread_local bool countAllocations=false;
thread_local size_t allocations=0;
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
struct Sink final : RT64::FastDrawSink {
    uint64_t draws=0;
    std::array<RT64::FastVertex,3> last{};
    RT64::FastDraw retained;
    bool retain=false;
    void draw(const RT64::FastDraw &draw) override {
        ++draws;
        check(draw.vertices.size()==3,"triangle vertex count changed");
        std::copy_n(draw.vertices.begin(),3,last.begin());
        if(retain)retained=draw;
    }
    void fullSync() override {}
    void present(uint32_t) override {}
};
struct Fixture {
    std::array<uint32_t,2048> memory{};
    Sink sink;
    RT64::State state{reinterpret_cast<uint8_t *>(memory.data()),sizeof(memory),sink};
    RT64::GBI gbi;
    Fixture() {
        gbi.ucode=RT64::GBIUCode::F3DEX2;
        RT64::GBI_RDP::setup(&gbi,true);RT64::GBI_F3DEX2::setup(&gbi);
        state.rsp->setGBI(&gbi);
        state.rdp->setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,320,0);
        for(unsigned i=0;i<32;++i) {
            const unsigned address=0x400+i*16;
            state.RDRAM[(address+12)^3]=uint8_t(127-i*7);
            state.RDRAM[(address+13)^3]=uint8_t(i*5-64);
            state.RDRAM[(address+14)^3]=uint8_t(90-i*3);
            state.RDRAM[(address+15)^3]=uint8_t(i*8);
        }
        auto &rsp=*state.rsp;
        rsp.geometryMode=G_LIGHTING|G_TEXTURE_GEN;
        rsp.lightCount=3;
        for(unsigned i=0;i<rsp.lights.size();++i) {
            rsp.lights[i].direction={float(i+1),float(3-int(i)),float(i%2)};
            rsp.lights[i].color={0.05f,0.08f,0.11f};
        }
        rsp.setVertex(0x400,32,0);
    }
};
std::array<float,3> direction(const RT64::FastRSP &rsp,const std::array<float,3> &value) {
    std::array<float,3> result{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)result[r]+=rsp.modelStack.back()[r][c]*value[c];
    const float length=std::sqrt(result[0]*result[0]+result[1]*result[1]+result[2]*result[2]);
    if(length>0)for(auto &v:result)v/=length;
    return result;
}
float dot(const std::array<float,3> &a,const std::array<float,3> &b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
void lighting(Fixture &fixture) {
    auto &rsp=*fixture.state.rsp;
    for(unsigned lights=0;lights<=7;++lights)for(unsigned mode=0;mode<4;++mode) {
        rsp.lightCount=lights;
        rsp.geometryMode=G_LIGHTING|(mode&1?G_TEXTURE_GEN:0)|(mode&2?G_TEXTURE_GEN_LINEAR:0);
        rsp.modelStack.back()[0][0]=float(mode)-1;
        rsp.modelStack.back()[1][1]=float(lights+1)/2;
        rsp.modelStack.back()[2][0]=0.125f*lights;
        rsp.lookAtX={1,float(mode),0};rsp.lookAtY={0,mode&1?0.0f:-1.0f,0};
        rsp.combinedChanged=true;
        rsp.setVertex(0x400,32,17);
        for(unsigned i=0;i<32;++i) {
            const unsigned address=0x400+i*16;
            std::array<float,3> normal{};
            for(unsigned c=0;c<3;++c)normal[c]=int8_t(fixture.state.readU8(address+12+c))/127.0f;
            auto expected=rsp.lights[lights].color;
            for(unsigned l=0;l<lights;++l) {
                const float intensity=std::max(0.0f,dot(normal,direction(rsp,rsp.lights[l].direction)));
                for(unsigned c=0;c<3;++c)expected[c]+=intensity*rsp.lights[l].color[c];
            }
            const auto &actual=rsp.vertices[17+i];
            for(unsigned c=0;c<3;++c)check(std::abs(actual.color[c]-std::min(expected[c],1.0f))<1e-6f,"batched lighting changed vertex color");
            check(actual.color[3]==fixture.state.readU8(address+15)/255.0f,"lighting changed vertex alpha");
            if(mode&1)for(unsigned axis=0;axis<2;++axis) {
                const float projected=std::clamp(dot(normal,direction(rsp,axis?rsp.lookAtY:rsp.lookAtX)),-1.0f,1.0f);
                const float uv=mode&2?std::acos(-projected)*(1024.0f/3.141592654f):(projected+1)*512;
                check(std::abs(actual.uv[axis]-uv)<0.0002f,"batched lighting changed texture generation");
            }
        }
    }
}
void triangles(Fixture &fixture,bool benchmark,FILE *output) {
    auto &rsp=*fixture.state.rsp;
    fixture.sink.retain=true;rsp.drawIndexedTri(17,18,19);fixture.sink.retain=false;
    const auto saved=fixture.sink.retained.vertices;
    allocations=0;countAllocations=true;
    for(unsigned i=0;i<1000;++i)rsp.drawIndexedTri(19,18,17);
    countAllocations=false;
    const auto measured=allocations;
    check(std::memcmp(saved.data(),fixture.sink.retained.vertices.data(),saved.size()*sizeof(RT64::FastVertex))==0,"triangle scratch storage changed an already submitted draw");
    std::fprintf(output,"triangle_allocations_per_1000=%llu\n",static_cast<unsigned long long>(measured));
    if(!benchmark)check(measured==0,"RSP still allocates a temporary vector for every triangle");
}
}
void *operator new(std::size_t size) {
    if(countAllocations)++allocations;
    if(void *value=std::malloc(size?size:1))return value;
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value,std::size_t) noexcept { std::free(value); }
int run_rsp_checks(bool benchmark,FILE *output,FILE *errors) {
    try {
        Fixture fixture;lighting(fixture);triangles(fixture,benchmark,output);
        if(benchmark)for(unsigned trial=0;trial<5;++trial) {
            auto &rsp=*fixture.state.rsp;
            rsp.lightCount=3;rsp.geometryMode=G_LIGHTING|G_TEXTURE_GEN;
            const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<20000;++i)rsp.setVertex(0x400,32,0);
            const auto lit=std::chrono::steady_clock::now();
            for(unsigned i=0;i<200000;++i)rsp.drawIndexedTri(0,1,2);
            const auto end=std::chrono::steady_clock::now();
            std::fprintf(output,"trial=%u vertices=640000 lighting_us=%lld triangles=200000 triangle_us=%lld\n",trial,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(lit-start).count()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(end-lit).count()));
        }
        std::fprintf(output,"RSP checks: 1024 lit/texgen vertices and immutable triangle copies passed\n");
        return 0;
    } catch(const std::exception &error) { countAllocations=false;std::fprintf(errors,"%s\n",error.what());return 1; }
}
#ifndef RT64_CPU_CONTROL
int main(int argc,char **argv) {
    return run_rsp_checks(argc>1 && std::string(argv[1])=="--benchmark",stdout,stderr);
}
#endif
