#include "fast/rt64_fast_interpreter.h"
#include "gbi/rt64_gbi_f3d.h"
#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace RT64;
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
struct Capture final : FastDrawSink {
    std::vector<FastDraw> triangles;
    unsigned calls=0;
    void draw(const FastDraw &draw) override {
        ++calls;
        check(draw.vertices.size()%3==0 && draw.vertices.size()<=6144,"Triangle run exceeded its bounded submission size");
        for(size_t i=0;i<draw.vertices.size();i+=3) {
            FastDraw triangle;
            static_cast<FastDrawParameters &>(triangle)=draw;
            triangle.textures=draw.textures;
            triangle.vertices.assign(draw.vertices.begin()+i,draw.vertices.begin()+i+3);
            triangles.push_back(std::move(triangle));
        }
    }
    void fullSync() override {}
    void present(uint32_t) override {}
};
void change(State *state,DisplayList **dl) {
    auto &rsp=*state->rsp;
    const unsigned variant=(*dl)->w1;
    rsp.geometryMode=variant&1?0x200001:1;
    rsp.modifyVertex(1,G_MWO_POINT_ST,0xffa00060U+variant);
    rsp.modifyVertex(2,G_MWO_POINT_XYSCREEN,0x04000600U+variant);
    rsp.modifyVertex(0,G_MWO_POINT_RGBA,0x90a0b0c0U+variant);
    state->rdp->setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,variant&2?37:320,0x10000+variant*0x1000);
    state->rdp->setPrimColor(0,0,variant*0x01020304U);
    state->rdp->setPrimDepth(1000+variant,0);
    state->rdp->setOtherMode(0,variant&4?0x34:0x30);
    rsp.textureOn=true;rsp.textureTile=variant&1;
    auto &rdp=*state->rdp;
    const unsigned input=variant&2?2:1;
    rdp.parameters.combine={0x00ffffff,(15U<<24)|(7U<<21)|(7U<<18)|(input<<6)|(7U<<3)|input};
    for(unsigned tile=0;tile<3;++tile) {
        rdp.setTile(tile,G_IM_FMT_I,G_IM_SIZ_8b,1,tile,0,0,0,0,0,0,0);
        rdp.setTileSize(tile,0,0,4,0);
        rdp.tmem[tile*8]=uint8_t(variant*17+tile);
        rdp.tmem[tile*8+1]=uint8_t(variant*23+tile);
    }
    ++rdp.tmemGeneration;
}
void unknownTriangle(State *state,DisplayList **) {
    state->rsp->drawIndexedTri(0,1,2);
    state->rdp->setPrimColor(0,0,0x12345678);
    state->rsp->drawIndexedTri(2,1,0);
}
struct Fixture {
    std::vector<uint32_t> memory=std::vector<uint32_t>(32768);
    Capture sink;
    State state{reinterpret_cast<uint8_t *>(memory.data()),memory.size()*4,sink};
    GBI gbi;
    Interpreter interpreter;
    explicit Fixture(GBIUCode ucode=GBIUCode::F3DEX2) {
        gbi.ucode=ucode;
        GBI_RDP::setup(&gbi,true);
        if(ucode==GBIUCode::F3D)GBI_F3D::setup(&gbi);
        else if(ucode==GBIUCode::F3DEX)GBI_F3DEX::setup(&gbi);
        else GBI_F3DEX2::setup(&gbi);
        gbi.map[0x70]=change;gbi.map[0x71]=unknownTriangle;
        state.rsp->setGBI(&gbi);
        interpreter.setup(&state);interpreter.hleGBI=&gbi;
        state.rdp->setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,320,0x10000);
        state.rsp->geometryMode=0x200001;
        for(unsigned i=0;i<16;++i) {
            auto &v=state.rsp->vertices[i];
            state.rsp->vertexValid[i]=true;
            v.position[3]=0.5f+i*0.25f;
            v.color[0]=i/16.0f;v.color[1]=0.25f;v.color[2]=0.75f;v.color[3]=1;
            v.uv[0]=i*1.5f;v.uv[1]=-float(i)*2.25f;v.fog=i*0.0625f;
            state.rsp->screenPositions[i]={float(i*7+1),float(i*3+2),float(i)/32};
        }
    }
    DisplayList *commands(const std::vector<DisplayList> &list) {
        auto *result=reinterpret_cast<DisplayList *>(memory.data()+0x100);
        std::copy(list.begin(),list.end(),result);
        return result;
    }
};
DisplayList command(uint32_t w0,uint32_t w1=0) {
    DisplayList result;result.w0=w0;result.w1=w1;return result;
}
DisplayList tri(unsigned a,unsigned b,unsigned c) {
    return command(0x05000000U|(a<<17)|(b<<9)|(c<<1));
}
void same(const Capture &a,const Capture &b) {
    check(a.triangles.size()==b.triangles.size(),"Triangle run changed primitive count");
    for(size_t i=0;i<a.triangles.size();++i) {
        const auto &x=a.triangles[i],&y=b.triangles[i];
        check(x.colorAddress==y.colorAddress && x.width==y.width && x.height==y.height
            && x.depthAddress==y.depthAddress && x.depthTest==y.depthTest && x.depthWrite==y.depthWrite
            && x.cullFront==y.cullFront && x.cullBack==y.cullBack && x.fog==y.fog
            && x.primitive==y.primitive && x.scissor==y.scissor && x.otherMode.L==y.otherMode.L
            && x.otherMode.H==y.otherMode.H,"Triangle run leaked state across a command boundary");
        check(x.combine.L==y.combine.L && x.combine.H==y.combine.H,"Triangle run changed the combiner");
        for(unsigned unit=0;unit<2;++unit) {
            const auto &xt=x.textures[unit],&yt=y.textures[unit];
            check(bool(xt)==bool(yt),"Triangle run changed texture usage");
            if(xt)check(xt->width==yt->width && xt->height==yt->height && xt->rgba==yt->rgba,
                        "Triangle run retained a stale texture across a TMEM command");
            const auto &a=x.tiles[unit],&b=y.tiles[unit];
            check(a.fmt==b.fmt && a.siz==b.siz && a.tmem==b.tmem && a.line==b.line
                && a.uls==b.uls && a.ult==b.ult && a.lrs==b.lrs && a.lrt==b.lrt,
                "Triangle run retained a stale tile");
        }
        check(std::memcmp(x.vertices.data(),y.vertices.data(),3*sizeof(FastVertex))==0,
              "Triangle run changed exact vertex order, flat shading, depth or coordinates");
    }
}
void run() {
    Fixture actual,reference;
    std::vector<DisplayList> list;
    for(unsigned variant=0;variant<8;++variant) {
        list.push_back(command(0x70000000,variant));
        for(unsigned i=0;i<24;++i)list.push_back(tri(i%16,(i+1)%16,(i+2)%16));
        list.push_back(command(0x06000204,0x00040600));
        list.push_back(command(0x07000408,0x0004080c));
        list.push_back(command(0x71000000));
    }
    for(unsigned i=0;i<2200;++i)list.push_back(tri(i%16,(i+1)%16,(i+2)%16));
    list.push_back(command(0xdf000000));
    auto *a=actual.commands(list),*b=reference.commands(list);
    reference.state.memoryEpoch=1;
    for(auto *p=b;p && p<b+list.size();) {
        reference.gbi.map[p->w0>>24](&reference.state,&p);
        if(p)++p;
    }
    actual.interpreter.processDisplayLists(0x400,a);
    same(actual.sink,reference.sink);
    check(actual.sink.calls<reference.sink.calls/8,"Interpreter still submits and prepares every triangle individually");
    actual.sink.triangles.clear();actual.sink.calls=0;
    auto *bad=actual.commands({tri(0,1,2),tri(0,1,127),command(0xdf000000)});
    bool rejected=false;
    try { actual.interpreter.processDisplayLists(0x400,bad); }
    catch(const std::runtime_error &) { rejected=true; }
    check(rejected,"Unloaded vertex was accepted inside a triangle run");
    actual.sink.triangles.clear();actual.sink.calls=0;
    auto *again=actual.commands({tri(3,4,5),command(0xdf000000)});
    actual.interpreter.processDisplayLists(0x400,again);
    check(actual.sink.triangles.size()==1,"An aborted run leaked vertices into the next task");
    std::puts("Triangle runs preserve primitive order, flat shading, state changes, capacity and error recovery");
}
void olderMicrocodes() {
    for(auto ucode:{GBIUCode::F3D,GBIUCode::F3DEX}) {
        Fixture actual{ucode},reference{ucode};
        auto opcode=[&](GBIFunction function) {
            for(unsigned i=0;i<256;++i)if(actual.gbi.map[i]==function)return i<<24;
            throw std::runtime_error("Expected triangle handler is absent");
        };
        const bool original=ucode==GBIUCode::F3D;
        const auto triOp=opcode(original?GBI_F3D::tri1:GBI_F3DEX::tri1);
        const auto quadOp=opcode(original?GBI_F3D::quad:GBI_F3DEX::quad);
        std::vector<DisplayList> list;
        for(unsigned variant=0;variant<4;++variant) {
            list.push_back(command(0x70000000,variant));
            for(unsigned n=0;n<40;++n) {
                const unsigned a=n%12,b=a+1,c=a+2;
                list.push_back(command(triOp,original?((a*10<<16)|(b*10<<8)|c*10):((a<<17)|(b<<9)|(c<<1))));
            }
            list.push_back(command(quadOp,original?0x000a141e:0x00020406));
            if(!original)list.push_back(command(opcode(GBI_F3DEX::tri2)|0x000204,0x00040600));
        }
        list.push_back(command(opcode(GBI_F3D::endDl)));
        auto *a=actual.commands(list),*b=reference.commands(list);
        reference.interpreter.batchTriangleRuns=false;
        reference.interpreter.processDisplayLists(0x400,b);
        actual.interpreter.processDisplayLists(0x400,a);
        same(actual.sink,reference.sink);
        check(actual.sink.calls<reference.sink.calls/8,"Older microcode did not group pure triangles");
    }
}
}
int main() {
    try { run();olderMicrocodes(); }
    catch(const std::exception &error) { std::fprintf(stderr,"%s\n",error.what());return 1; }
}
