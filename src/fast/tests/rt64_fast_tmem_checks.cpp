#include "fast/rt64_fast_state.h"
#include "fast/rt64_fast_profile.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

namespace TmemControl {
namespace {
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
struct Sink : RT64::FastDrawSink {
    std::shared_ptr<const RT64::FastFramebuffer> snapshot;
    unsigned snapshots=0;
    void draw(const RT64::FastDraw &) override {}
    void fullSync() override {}
    void present(uint32_t) override {}
    std::shared_ptr<const RT64::FastFramebuffer> snapshotFramebuffer(uint32_t,uint32_t) override {
        ++snapshots;
        return snapshot;
    }
};
struct Transfer {
    uint32_t start=0,stride=0,words=1,rows=1,line=0,tmem=0;
    uint16_t dxt=0;
    bool block=false,palette=false,rgba32=false;
};
void reference(std::array<uint8_t,4096> &tmem,const uint8_t *rdram,const Transfer &t) {
    const uint32_t mask=t.rgba32?2047:4095;
    uint32_t fraction=0,parity=0;
    for(uint32_t y=0;y<t.rows;++y) {
        uint32_t destination=(t.tmem*8+y*(t.line<<(t.palette?5:3)))&mask;
        uint32_t source=t.start+y*t.stride;
        for(uint32_t x=0;x<t.words;++x) {
            for(uint32_t lane=0;lane<8;++lane) {
                uint32_t at;
                if(t.rgba32)at=(((destination+(lane/4)*2+(lane&1))&mask)^parity)|((lane&2)?2048:0);
                else at=((destination+lane)&mask)^parity;
                tmem[at]=rdram[(source+(t.palette?(lane&1):lane))^3];
            }
            if(t.block) {
                fraction+=t.dxt;
                while(fraction>=2048) {
                    destination=(destination+(t.line<<(t.palette?5:3)))&mask;
                    fraction-=2048;
                    parity^=4;
                }
            }
            destination=(destination+(t.rgba32?4:8))&mask;
            source+=t.palette?2:8;
        }
        if(!t.block)parity^=4;
    }
}
void execute(RT64::FastRDP &rdp,const Transfer &t) {
    rdp.setTile(0,t.rgba32?G_IM_FMT_RGBA:G_IM_FMT_I,t.rgba32?G_IM_SIZ_32b:G_IM_SIZ_8b,
        t.line,t.tmem,0,0,0,0,0,0,0);
    rdp.loadTMEM(0,t.start,t.stride,t.words,t.rows,t.block,t.palette,t.dxt);
}
void checkTransfer(RT64::State &state,Sink &sink,const Transfer &t,unsigned &cases) {
    auto &rdp=*state.rdp;
    auto expected=rdp.tmem;
    reference(expected,state.RDRAM,t);
    const auto generation=rdp.tmemGeneration;
    const auto queries=sink.snapshots;
    execute(rdp,t);
    check(rdp.tmem==expected,"TMEM transfer differs from the independent byte reference");
    check(rdp.tmemGeneration==generation+1,"TMEM generation did not advance exactly once");
    check(sink.snapshots==queries+1,"TMEM copy skipped framebuffer snapshot ordering");
    ++cases;
}
void profileCheck(RT64::State &state) {
    using namespace RT64::FastProfile;
    const std::array<const char *,7> names={"tmem_plain_block_bytes","tmem_plain_tile_bytes","tmem_palette_bytes",
        "tmem_rgba32_bytes","tmem_provenance_bytes","tmem_unaligned_bytes","tmem_bulk_bytes"};
    std::array<size_t,7> indices{};
    for(size_t n=0;n<names.size();++n) {
        auto it=std::find_if(std::begin(counterNames),std::end(counterNames),[&](const char *name){return std::string(name)==names[n];});
        check(it!=std::end(counterNames),"TMEM transfer categories are not recorded by the profiler");
        indices[n]=size_t(it-std::begin(counterNames));
    }
    Context context{+[]()->uint64_t{return 1;}};
    current=&context;
    Transfer transfer;
    transfer.words=4;transfer.block=true;
    execute(*state.rdp,transfer);
    transfer.block=false;
    execute(*state.rdp,transfer);
    transfer.palette=true;
    execute(*state.rdp,transfer);
    transfer.palette=false;transfer.rgba32=true;
    execute(*state.rdp,transfer);
    transfer.rgba32=false;transfer.start=1;
    execute(*state.rdp,transfer);
    current=nullptr;
    check(context.stats.counters[indices[0]]==32,"Plain block transfer byte count is wrong");
    check(context.stats.counters[indices[1]]==96,"Plain tile transfer byte count is wrong");
    check(context.stats.counters[indices[2]]==8,"Palette source byte count is wrong");
    check(context.stats.counters[indices[3]]==32,"RGBA32 source byte count is wrong");
    check(context.stats.counters[indices[4]]==0,"Ordinary RAM was counted as framebuffer provenance");
    check(context.stats.counters[indices[5]]==32,"Unaligned source byte count is wrong");
#ifdef RT64_FAST_REFERENCE_TMEM_LOAD
    check(context.stats.counters[indices[6]]==0,"Reference build still uses the bulk copy path");
#else
    check(context.stats.counters[indices[6]]==64,"Eligible transfers did not use the bulk copy path");
#endif
}
}
int run(bool benchmark,bool profile,FILE *output,FILE *errors) {
    try {
        std::vector<uint32_t> memory(128*1024/4);
        auto *rdram=reinterpret_cast<uint8_t *>(memory.data());
        for(size_t i=0;i<memory.size()*4;++i)rdram[i]=uint8_t((i*97)^(i>>4)^(i>>11));
        Sink sink;
        RT64::State state{rdram,memory.size()*4,sink};
        if(profile)profileCheck(state);
        unsigned cases=0;
        for(unsigned mode=0;mode<8;++mode)for(unsigned alignment=0;alignment<8;++alignment)
        for(uint32_t stride:{541U,544U})
        for(uint16_t dxt:{0,1,127,128,255,256,1023,1024,2047,2048,4095,65535}) {
            Transfer t;
            t.start=16+alignment;t.words=67;t.rows=3;t.stride=stride;
            t.line=7;t.tmem=509;t.dxt=dxt;t.block=mode&1;t.palette=mode&2;t.rgba32=mode&4;
            checkTransfer(state,sink,t,cases);
        }
        for(uint32_t words:{1U,2U,7U,8U,9U,15U,16U,17U,511U,512U,513U})
        for(uint16_t dxt:{0,128,256,1024,2048,4095}) {
            Transfer t;
            t.start=4;t.words=words;t.line=7;t.tmem=509;t.block=true;t.dxt=dxt;
            checkTransfer(state,sink,t,cases);
        }
        std::mt19937 random{0x6474};
        for(unsigned n=0;n<1200;++n) {
            Transfer t;
            t.start=random()%1024;t.words=1+random()%1024;t.rows=1+random()%8;
            t.stride=random()%2048;t.line=random()%512;t.tmem=random()%512;
            t.dxt=random()%4096;t.block=random()&1;t.palette=random()&1;t.rgba32=random()&1;
            checkTransfer(state,sink,t,cases);
        }
        auto image=std::make_shared<RT64::FastFramebuffer>();
        image->address=0;image->width=64;image->height=32;image->colorBytes=2;
        sink.snapshot=image;
        Transfer provenance;
        provenance.words=512;
        checkTransfer(state,sink,provenance,cases);
        check(!state.rdp->framebufferLoads.empty(),"Framebuffer provenance was not retained");
        sink.snapshot.reset();provenance.start=32;
        checkTransfer(state,sink,provenance,cases);
        check(state.rdp->framebufferLoads.empty(),"Overwritten framebuffer provenance was retained");
        for(const auto value:state.rdp->tmemFramebuffer)check(value==0,"Stale framebuffer ownership after a plain transfer");
        const auto saved=state.rdp->tmem;
        Transfer invalid;
        invalid.start=uint32_t(memory.size()*4)-4;invalid.words=2;
        bool rejected=false;
        try {execute(*state.rdp,invalid);}catch(const std::out_of_range &){rejected=true;}
        check(rejected && state.rdp->tmem==saved,"Invalid source changed TMEM before bounds validation");
        std::fprintf(output,"TMEM: %u exact cases, alignment, DXT, wraps, palettes, bank splitting and provenance passed\n",cases);
        if(benchmark) {
            for(unsigned mode=0;mode<3;++mode) {
                Transfer t;t.words=256;t.block=mode==0;t.dxt=256;t.line=0;t.rgba32=mode==2;
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<20000;++i){t.start=(i&7)*2048;execute(*state.rdp,t);}
                const auto end=std::chrono::steady_clock::now();
                std::fprintf(output,"mode=%u transfers=20000 bytes=40960000 us=%lld\n",mode,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(end-start).count()));
            }
        }
        return 0;
    } catch(const std::exception &error) {std::fprintf(errors,"%s\n",error.what());return 1;}
}
}
#ifndef RT64_TMEM_CONTROL
int main(int argc,char **argv) {
    return TmemControl::run(argc>1 && std::string(argv[1])=="--benchmark",
        argc>1 && std::string(argv[1])=="--expect-transfer-profile",stdout,stderr);
}
#endif
