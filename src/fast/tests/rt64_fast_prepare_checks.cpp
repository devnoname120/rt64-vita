#include "fast/rt64_fast_state.h"
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
struct Sink : RT64::FastDrawSink {
    void draw(const RT64::FastDraw &) override {}
    void fullSync() override {}
    void present(uint32_t) override {}
};
void snapshotBarriers() {
    struct QuerySink : Sink {
        unsigned draws=0,snapshots=0,vertices=0;
        bool resident=false;
        void draw(const RT64::FastDraw &draw) override { ++draws;vertices+=draw.vertices.size(); }
        bool maySnapshotFramebuffer(uint32_t,uint32_t) const override { return resident; }
        std::shared_ptr<const RT64::FastFramebuffer> snapshotFramebuffer(uint32_t,uint32_t) override {
            ++snapshots;return {};
        }
    };
    auto output=std::make_unique<QuerySink>();auto *observed=output.get();
    auto batch=RT64::createFastBatchingSink(std::move(output));
    RT64::FastDraw draw;draw.colorAddress=0x1000;draw.width=32;draw.height=24;draw.vertices.resize(3);
    batch->draw(draw);
    batch->snapshotFramebuffer(0x8000,64);
    check(observed->draws==0 && observed->snapshots==0,"Ordinary RAM texture lookup broke the pending batch");
    batch->draw(draw);batch->fullSync();
    check(observed->draws==1 && observed->vertices==6,"Ordinary RAM lookups prevented identical draws from batching");
    batch->draw(draw);batch->snapshotFramebuffer(0x1000,2);
    check(observed->draws==2 && observed->snapshots==1,"Pending first-use color target was not flushed before a snapshot");
    batch->draw(draw);observed->resident=true;batch->snapshotFramebuffer(0x8000,64);
    check(observed->draws==3 && observed->snapshots==2,"Resident or aliased target skipped its ordering barrier");
    observed->resident=false;draw.colorAddress=0xfffffff0;batch->draw(draw);
    batch->snapshotFramebuffer(0xfffffffe,2);
    check(observed->draws==4,"End-address arithmetic wrapped in the pending snapshot check");
}
void memoryAccess() {
    std::array<uint32_t,1024> memory{};
    Sink sink;
    RT64::State state{reinterpret_cast<uint8_t *>(memory.data()),sizeof(memory),sink};
    for(uint32_t i=0;i<sizeof(memory);++i)state.RDRAM[i^3]=uint8_t(i*71+(i>>3));
    for(uint32_t i=0;i<sizeof(memory);++i) {
        auto byte=[](uint32_t at){return uint8_t(at*71+(at>>3));};
        check(state.readU8(i)==byte(i),"Byte address changed");
        if(i+2<=sizeof(memory))check(state.readU16(i)==((uint32_t(byte(i))<<8)|byte(i+1)),"Unaligned halfword address changed");
        if(i+4<=sizeof(memory))check(state.readU32(i)==((uint32_t(byte(i))<<24)|(uint32_t(byte(i+1))<<16)
            |(uint32_t(byte(i+2))<<8)|byte(i+3)),"Unaligned word address changed");
    }
    for(uint32_t address:{4093U,4094U,4095U,4096U,0xfffffffcU,0xffffffffU}) {
        bool rejected=false;
        try { state.readU32(address); } catch(const std::out_of_range &) { rejected=true; }
        check(rejected,"Out-of-range word read was accepted");
    }
}
void preparation() {
    std::array<uint32_t,4096> memory{};
    Sink sink;
    RT64::State state{reinterpret_cast<uint8_t *>(memory.data()),sizeof(memory),sink};
    auto &rdp=*state.rdp;
    RT64::FastDraw draw;
    draw.vertices.resize(3);
    auto *storage=draw.vertices.data();
    rdp.prepareDraw(draw,0,false);
    check(draw.vertices.size()==3 && draw.vertices.data()==storage,"Preparing state discarded triangle storage");
    rdp.setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,320,0);
    rdp.setOtherMode(G_CYC_COPY,0);
    rdp.setTile(0,G_IM_FMT_I,G_IM_SIZ_8b,1,0,0,0,0,0,0,0,0);
    rdp.setTileSize(0,0,0,4,0);
    rdp.setTile(1,G_IM_FMT_I,G_IM_SIZ_8b,1,0,0,0,0,0,0,0,0);
    rdp.setTileSize(1,0,0,4,0);
    rdp.tmem[0]=0x55;rdp.tmem[1]=0xAA;++rdp.tmemGeneration;
    rdp.prepareDraw(draw,0,true);
    auto retained=draw;
    check(retained.textures[0] && retained.textures[0]->rgba[0]==0x55,"Prepared texture is missing");
    auto original=retained.textures[0];
    for(unsigned n=0;n<1000;++n)rdp.prepareDraw(draw,0,true);
    check(draw.textures[0]==original && draw.vertices.data()==storage,"Unchanged preparation lost ownership or storage");
    const auto staleEntry=rdp.cpuTextureCache.begin()->second;
    rdp.tmem[0]=0x33;++rdp.tmemGeneration;
    // Force the old image into the new hash bucket. Equality must still inspect
    // all TMEM bytes instead of treating the fingerprint as proof of identity.
    const uint64_t collisionKey=XXH3_64bits_withSeed(rdp.tmem.data(),rdp.tmem.size(),
        XXH3_64bits(staleEntry.layout.data(),sizeof(staleEntry.layout)));
    rdp.cpuTextureCache.emplace(collisionKey,staleEntry);
    rdp.cpuTextureCacheBytes+=sizeof(staleEntry)+staleEntry.texture->rgba.size();
    rdp.prepareDraw(draw,0,true);
    check(draw.textures[0]!=original && draw.textures[0]->rgba[0]==0x33 && original->rgba[0]==0x55,
        "TMEM mutation changed an already retained draw");
    draw.fill=draw.rectangle=draw.depthTest=draw.depthWrite=draw.cullFront=draw.cullBack=draw.clearDepth=true;
    rdp.setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_32b,37,0x1000);rdp.setDepthImage(0x2000);
    rdp.setPrimColor(7,0,0x10203040);rdp.setEnvColor(0xaabbccdd);
    rdp.setBlendColor(0x01020304);rdp.setFogColor(0x11223344);rdp.setScissor(0,1,2,147,80);
    state.memoryEpoch=91;
    rdp.prepareDraw(draw,0,false);
    check(!draw.textures[0] && !draw.textures[1] && !draw.fill && !draw.rectangle && !draw.depthTest
        && !draw.depthWrite && !draw.cullFront && !draw.cullBack && !draw.clearDepth,"Transient draw state leaked into preparation");
    check(draw.memoryEpoch==91 && draw.width==37 && draw.colorBytes==4 && draw.depthAddress==0x2000
        && draw.primitive==rdp.parameters.primitive && draw.environment==rdp.parameters.environment
        && draw.scissor==rdp.parameters.scissor && draw.fogColor==rdp.parameters.fogColor
        && draw.blendColor==rdp.parameters.blendColor,"Prepared parameters are stale");
    for(unsigned variant=0;variant<512;++variant) {
        rdp.parameters.combine={uint32_t(variant*0x127abeU),uint32_t(variant*0xabc713U)};
        rdp.otherMode={0,uint32_t((variant%4)<<20)};
        rdp.prepareDraw(draw,0,true);
        for(unsigned unit=0;unit<2;++unit)
            check(bool(draw.textures[unit])==draw.combine.usesTexture(draw.otherMode,unit,false),"Cached combiner texture usage is stale");
    }
}
}
int main() {
    try { preparation();snapshotBarriers();memoryAccess();std::puts("Prepared draw ownership, state transitions, TMEM changes, snapshot barriers and memory boundaries passed"); }
    catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what());return 1; }
}
