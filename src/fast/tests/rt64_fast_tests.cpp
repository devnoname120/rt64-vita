#include "fast/rt64_fast_interpreter.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include <functional>
#include <fstream>
#include <iostream>
#include <map>

using namespace RT64;
namespace {
    void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
    void close(float actual, float expected, const char *message) { check(std::abs(actual - expected) < 0.0001f, message); }
    void rejects(const std::function<void()> &fn, const char *message) {
        try { fn(); } catch (const std::exception &) { return; }
        throw std::runtime_error(message);
    }
    struct Capture : FastDrawSink {
        std::vector<FastDraw> draws;
        unsigned syncs = 0;
        std::vector<uint32_t> presents;
        void draw(const FastDraw &value) override { draws.push_back(value); }
        void fullSync() override { ++syncs; }
        void present(uint32_t address) override { presents.push_back(address); }
    };
    void shaderProgramKeys() {
        // Compare source as the oracle: a shared cache key must never alias
        // different programs, including changes in each individual mode bit.
        std::map<std::array<uint32_t,4>,std::string> sourceByKey;
        auto visit=[&](const FastDraw &d) {
            const auto shader=fastFragmentShader(d);
            const auto [it,inserted]=sourceByKey.emplace(fastShaderKey(d),shader);
            check(inserted || it->second==shader,"shader cache aliases different GLSL");
        };
        FastDraw base; base.combine={0xfc127e24,0xfffff9fc};
        auto texture=std::make_shared<FastTexture>();
        for(unsigned cycle=0;cycle<4;++cycle) for(unsigned mask=0;mask<4;++mask)
            for(unsigned fog=0;fog<2;++fog) for(unsigned fill=0;fill<2;++fill) {
                FastDraw d=base; d.otherMode.H=cycle<<20; d.fog=fog; d.fill=fill;
                if(mask&1) d.textures[0]=texture;
                if(mask&2) d.textures[1]=texture;
                visit(d);
                for(unsigned bit=0;bit<32;++bit) {
                    auto changed=d; changed.otherMode.L^=1U<<bit; visit(changed);
                    changed=d; changed.otherMode.H^=1U<<bit; visit(changed);
                }
            }
        auto changed=base; changed.otherMode.L=Z_CMP|Z_UPD|FORCE_BL;
        check(fastShaderKey(base)==fastShaderKey(changed),"fixed-function state recompiles shaders");
    }
    void drawBatching() {
        auto output=std::make_unique<Capture>(); auto *capture=output.get();
        auto batch=createFastBatchingSink(std::move(output));
        FastDraw d; d.vertices.resize(3); d.vertices[0].position[0]=1;
        batch->draw(d); d.vertices[0].position[0]=2; batch->draw(d);
        check(capture->draws.empty(),"batch submitted before a boundary");
        batch->fullSync();
        check(capture->syncs==1 && capture->draws.size()==1 && capture->draws[0].vertices.size()==6,"matching triangles did not batch");
        check(capture->draws[0].vertices[0].position[0]==1 && capture->draws[0].vertices[3].position[0]==2,"batch changed triangle order or retained caller storage");
        capture->draws.clear();
        batch->draw(d); d.environment[0]=0.5f; batch->draw(d);
        check(capture->draws.size()==1 && capture->draws[0].environment[0]==0,"uniform change did not split batch");
        d.tiles[0].uls=4; batch->draw(d);
        check(capture->draws.size()==2,"tile change did not split batch");
        d.scissor[0]=4; batch->draw(d);
        check(capture->draws.size()==3,"scissor change did not split batch");
        d.clearDepth=true; batch->draw(d);
        check(capture->draws.size()==5 && capture->draws.back().clearDepth,"depth clear did not flush and retain ordering");
        d.clearDepth=false; batch->draw(d); batch->present(0x1234);
        check(capture->draws.size()==6 && capture->presents==std::vector<uint32_t>{0x1234},"presentation did not flush");
        capture->draws.clear();
        for(unsigned i=0;i<2049;++i) batch->draw(d);
        batch->fullSync();
        check(capture->draws.size()==2 && capture->draws[0].vertices.size()==6144 && capture->draws[1].vertices.size()==3,"batch exceeded its bounded vertex upload");
        capture->draws.clear();
        auto texture=std::make_shared<FastTexture>(); d.textures[0]=texture; batch->draw(d);
        d.textures[0]=std::make_shared<FastTexture>(); batch->draw(d); batch->fullSync();
        check(capture->draws.size()==2 && capture->draws[0].textures[0]==texture,"texture ownership or identity lost across batching");
    }
    struct Fixture {
        std::vector<uint32_t> memory = std::vector<uint32_t>(2 * 1024 * 1024);
        Capture capture;
        State state{reinterpret_cast<uint8_t *>(memory.data()), memory.size() * 4, capture};
        Interpreter interpreter;
        GBI gbi;
        Fixture() {
            gbi.ucode = GBIUCode::F3DEX2;
            GBI_RDP::setup(&gbi, true);
            GBI_F3DEX2::setup(&gbi);
            interpreter.setup(&state); interpreter.hleGBI = &gbi; state.rsp->setGBI(&gbi);
        }
        void byte(uint32_t address, uint8_t value) { state.RDRAM[address ^ 3] = value; }
        void half(uint32_t address, uint16_t value) { byte(address, value >> 8); byte(address+1, value); }
        void command(uint32_t address, uint32_t w0, uint32_t w1) { memory.at(address/4) = w0; memory.at(address/4+1) = w1; }
        void run(uint32_t address) { interpreter.processDisplayLists(address, reinterpret_cast<DisplayList *>(state.fromRDRAM(address))); }
    };
    void memoryAndControlFlow() {
        Fixture f;
        f.half(0, 0x1234); f.half(2, 0x5678);
        check(f.state.readU32(0) == 0x12345678, "word-swapped RDRAM");
        rejects([&]{f.state.readU32(uint32_t(f.memory.size()*4-2));}, "out-of-range read accepted");
        f.command(0x100, 0xde000000, 0x200); // nested display list
        f.command(0x108, 0xdf000000, 0);
        f.command(0x200, 0xe9000000, 0);
        f.command(0x208, 0xdf000000, 0);
        f.run(0x100); check(f.capture.syncs == 1, "display-list call/return");
        f.command(0x200, 0xde000000, 0x200);
        rejects([&]{f.run(0x100);}, "recursive display list accepted");
        f.command(0x100, 0x99000000, 0);
        rejects([&]{f.run(0x100);}, "unsupported opcode accepted");
        const uint32_t end = uint32_t(f.memory.size()*4-8);
        f.command(end, 0xe4000000, 0);
        rejects([&]{f.run(end);}, "truncated texture rectangle accepted");
    }
    void matrixStackBounds() {
        Fixture f;
        for(unsigned i=0;i<16;++i) f.half(0x1000+i*2,i%5==0?1:0);
        for(unsigned i=0;i<40;++i) f.command(0x100+i*8,0xda380002,0x1000); // F3DEX2 PUSH | LOAD.
        f.command(0x100+40*8,0xdf000000,0);
        f.run(0x100);
        auto &rsp=*f.state.rsp;
        check(rsp.modelStack.size()==32,"model matrix stack did not saturate");
        f.half(0x1000,2);
        rsp.matrix(0x1000,rsp.pushMask|rsp.loadMask);
        check(rsp.modelStack.size()==32 && rsp.modelStack.back()[0][0]==2,"saturated push lost the matrix update");
        rsp.popMatrix(UINT32_MAX);
        check(rsp.modelStack.size()==1 && rsp.modelStack.back()[0][0]==1,"matrix pop did not retain the base frame");
        rsp.forceMatrix(0x1000); rsp.popMatrix(0); rsp.popMatrix(1);
        check(!rsp.combinedChanged && rsp.combined[0][0]==2,"empty pop invalidated a forced matrix");
    }
    void triangleAndFill() {
        Fixture f;
        const int16_t positions[3][3] = {{-1,-1,0},{1,-1,0},{0,1,0}};
        for (unsigned i = 0; i < 3; ++i) {
            for (unsigned c = 0; c < 3; ++c) f.half(0x400+i*16+c*2, positions[i][c]);
            f.byte(0x400+i*16+12, i == 0 ? 255 : 0);
            f.byte(0x400+i*16+13, i == 1 ? 255 : 0);
            f.byte(0x400+i*16+14, i == 2 ? 255 : 0);
            f.byte(0x400+i*16+15, 255);
        }
        f.command(0x100, 0xd9ffffff, 0x200000); // smooth shading
        f.command(0x108, 0x01003006, 0x400); // F3DEX2 load 3 at 0
        f.command(0x110, 0x05000204, 0); // indices 0,1,2
        f.command(0x118, 0xdf000000, 0);
        f.run(0x100);
        check(f.capture.draws.size() == 1, "triangle did not reach sink");
        const auto &d = f.capture.draws[0];
        close(d.vertices[0].position[0], -1, "left clip position");
        close(d.vertices[1].position[0], 1, "right clip position");
        close(d.vertices[2].position[1], 1, "top clip position");
        close(d.vertices[1].color[1], 1, "vertex color");
        f.state.rsp->viewportScale={320,240,0.5f};
        f.state.rsp->viewportTranslate={320,240,0.5f};
        f.state.rsp->drawIndexedTri(0,1,2);
        close(f.capture.draws.back().vertices[1].position[0],1,"viewport change moved a cached vertex");
        f.state.rsp->modifyVertex(1,G_MWO_POINT_XYSCREEN,(160U*4)<<16|120U*4);
        f.state.rsp->drawIndexedTri(0,1,2);
        close(f.capture.draws.back().vertices[1].position[0],0,"screen-space vertex modification");
        f.state.rsp->setGeometryMode(G_FOG);
        f.state.rsp->drawIndexedTri(0,1,2);
        check(!f.capture.draws.back().fog,"geometry fog flag incorrectly enabled an inactive RDP fog blender");
        f.state.rdp->setOtherMode(G_CYC_2CYCLE,0xc8000000);
        f.state.rsp->drawIndexedTri(0,1,2);
        check(f.capture.draws.back().fog,"standard fog blender was not enabled");
        f.state.rdp->setOtherMode(G_CYC_FILL, 0);
        f.state.rdp->setFillColor(0xf801f801);
        f.state.rdp->fillRect(0,0,1276,956);
        const auto &fill = f.capture.draws.back();
        close(fill.vertices[2].position[0], 1, "inclusive fill right edge");
        close(fill.vertices[2].position[1], -1, "inclusive fill bottom edge");
        close(fill.fillColor[0], 1, "RGBA5551 fill red");
        f.state.rdp->setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,640,0x200000);
        f.state.rdp->setScissor(0,0,0,2560,1920);
        f.state.rdp->fillRect(0,0,2556,1916);
        const auto &high=f.capture.draws.back();
        check(high.height==480,"640-wide framebuffer height");
        close(high.vertices[2].position[1],-1,"480-line fill bottom edge");
        f.state.rdp->setScissor(0,0,160,2560,1760);
        check(f.state.rdp->parameters.height==480,"letterboxing resized the framebuffer");
        f.state.rdp->setColorImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,320,0x200000);
        check(f.state.rdp->parameters.height==240,"return to 240-line mode");
    }
    void tmemLoads() {
        Fixture f;
        auto &rdp = *f.state.rdp;
        for (unsigned y=0;y<2;++y) for (unsigned x=0;x<8;++x) f.half(0x1000+y*16+x*2,y ? 0x07c1 : 0xf801);
        rdp.setTextureImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,8,0x1000);
        rdp.setTile(0,G_IM_FMT_RGBA,G_IM_SIZ_16b,2,0,0,0,0,0,0,0,0);
        rdp.loadTile(0,0,0,28,4);
        auto tile = rdp.decodeTexture(0);
        check(tile->width==8 && tile->height==2, "RGBA16 dimensions");
        check(tile->rgba[0]==255 && tile->rgba[32+1]==255, "RGBA16 odd-row load");
        check(tile == rdp.decodeTexture(0), "texture cache did not reuse TMEM snapshot");
        // LoadBlock uses a zero-line loading tile and DXT to swap alternate rows.
        rdp.setTile(7,G_IM_FMT_RGBA,G_IM_SIZ_16b,0,0,0,0,0,0,0,0,0);
        rdp.loadBlock(7,0,0,15,1024);
        auto block = rdp.decodeTexture(0);
        check(block->rgba == tile->rgba, "load-block DXT differs from tile load");
        f.half(0x1000,0x003f); rdp.loadBlock(7,0,0,15,1024);
        check(rdp.decodeTexture(0)->rgba[2]==255, "TMEM reload kept stale texture");
        check(tile->rgba[0]==255, "previous draw texture was mutated");
        // 32-bit RGBA splits RG and BA into the two TMEM banks.
        for (unsigned i=0;i<16;++i) f.byte(0x2000+i,uint8_t(i*11));
        rdp.setTextureImage(G_IM_FMT_RGBA,G_IM_SIZ_32b,2,0x2000);
        rdp.setTile(0,G_IM_FMT_RGBA,G_IM_SIZ_32b,1,0,0,0,0,0,0,0,0);
        rdp.loadTile(0,0,0,4,4);
        auto rgba = rdp.decodeTexture(0);
        for (unsigned i=0;i<16;++i) check(rgba->rgba[i]==i*11,"RGBA32 bank split/row XOR");
    }
    void vertexProcessing() {
        Fixture f;
        auto &rsp=*f.state.rsp;
        for(unsigned offset:{0U,8U}) {
            f.half(0x300+offset,640); f.half(0x302+offset,480); f.half(0x304+offset,511);
        }
        rsp.setViewport(0x300);
        for(unsigned i=0;i<3;++i) { f.half(0x400+i*16+4,0); f.byte(0x400+i*16+15,255); }
        rsp.setVertex(0x400,3,0); rsp.drawIndexedTri(0,1,2);
        close(f.capture.draws.back().vertices[0].position[2],-1.0f/512,"N64 viewport depth normalization");
        auto *start=reinterpret_cast<DisplayList *>(f.state.fromRDRAM(0x100));
        auto *pc=start;
        rsp.branchZ(0x600,0,511U<<16,&pc);
        check(pc==start,"branch-Z equality must not branch");
        rsp.branchZ(0x600,0,512U<<16,&pc);
        check(pc==reinterpret_cast<DisplayList *>(f.state.fromRDRAM(0x600))-1,"branch-Z fixed-point units");
        pc=start; rsp.branchW(0x600,0,1,&pc);
        check(pc==start,"branch-W equality must not branch");
        const float positions[]={160,120,0.5f,1,160,120,0.5f,1,160,120,0.5f,1};
        const float uv[6]={},color[12]={};
        f.state.rdp->drawTris(1,positions,uv,color,0,0);
        close(f.capture.draws.back().vertices[0].position[2],0,"RDP triangle normalized depth");
        rsp.setGeometryMode(G_LIGHTING);
        rsp.setLightCount(1);
        rsp.lights[0].color={1,1,1}; rsp.lights[0].direction={1,1,0};
        rsp.modelStack.back()[0][0]=2;
        f.byte(0x40c,127); f.byte(0x40d,0); f.byte(0x40e,0);
        rsp.setVertex(0x400,1,0);
        close(rsp.vertices[0].color[0],2/std::sqrt(5.0f),"light direction transform under nonuniform scale");
        rsp.modelStack.back()=interop::float4x4::identity();
        rsp.setLookAtVectors(hlslpp::float3(1,0,0),hlslpp::float3(0,1,0));
        rsp.setGeometryMode(G_TEXTURE_GEN|G_TEXTURE_GEN_LINEAR);
        rsp.setVertex(0x400,1,0);
        close(rsp.vertices[0].uv[0],1024,"linear texgen sign");
        close(rsp.vertices[0].uv[1],512,"linear texgen midpoint");
        rsp.setGeometryMode(G_FOG); rsp.setFog(0,255); f.byte(0x40f,0);
        rsp.setVertex(0x400,1,0);
        close(rsp.vertices[0].color[3],1,"fog must replace shade alpha");
        rsp.modifyVertex(0,G_MWO_POINT_RGBA,0xffffffff);
        close(rsp.vertices[0].fog,1,"modified shade alpha feeds fog blender");
    }
    void paletteAndIntensity() {
        Fixture f;
        auto &rdp=*f.state.rdp;
        f.half(0x1000,0xf801); f.half(0x1002,0x07c1);
        rdp.setTextureImage(G_IM_FMT_RGBA,G_IM_SIZ_16b,2,0x1000);
        rdp.setTile(7,G_IM_FMT_RGBA,G_IM_SIZ_16b,0,256,0,0,0,0,0,0,0);
        rdp.loadTLUT(7,0,0,4,0);
        f.byte(0x2000,0x01);
        rdp.setTextureImage(G_IM_FMT_CI,G_IM_SIZ_8b,8,0x2000);
        rdp.setTile(0,G_IM_FMT_CI,G_IM_SIZ_4b,1,0,0,0,0,0,0,0,0);
        rdp.loadTile(0,0,0,4,0);
        rdp.setOtherMode(G_TT_RGBA16,0);
        auto ci=rdp.decodeTexture(0);
        check(ci->rgba[0]==255 && ci->rgba[5]==255,"CI4 TLUT color");
        rdp.setTile(0,G_IM_FMT_I,G_IM_SIZ_4b,1,0,0,0,0,0,0,0,0);
        auto intensity=rdp.decodeTexture(0);
        check(intensity->rgba[0]==0 && intensity->rgba[4]==17 && intensity->rgba[7]==17,"I4 intensity/alpha");
        rdp.setTile(0,G_IM_FMT_YUV,G_IM_SIZ_16b,1,0,0,0,0,0,0,0,0);
        rejects([&]{rdp.decodeTexture(0);},"unsupported YUV texture silently accepted");
    }
}
int main(int argc, char **argv) {
    try {
        memoryAndControlFlow(); matrixStackBounds(); triangleAndFill(); vertexProcessing(); tmemLoads(); paletteAndIntensity(); drawBatching(); shaderProgramKeys();
        std::cout << "RT64 Fast: RDRAM, GBI execution, vertices, fill, TMEM tile/block, RGBA32, CI4, cache, batching and rejection tests passed\n";
        if (argc == 2) {
            // Optional real-game microcode check. The ROM is never a test
            // dependency or distribution asset. Offsets follow DK64 US's
            // decompressed global overlay, including its graphics microcode.
            std::ifstream rom(argv[1], std::ios::binary);
            std::vector<uint8_t> overlay(0x165d50);
            rom.seekg(0x2000000); rom.read(reinterpret_cast<char *>(overlay.data()), overlay.size());
            check(bool(rom), "cannot read DK64 decompressed global overlay");
            Fixture f;
            for (size_t i=0;i<overlay.size();++i) f.byte(0x5fb300+i,overlay[i]);
            f.interpreter.loadUCodeGBI(0x741f40,0x760840,true);
            check(f.interpreter.hleGBI->ucode==GBIUCode::F3DEX2,"DK64 graphics microcode identification");
            std::cout << "DK64 US actual graphics microcode identified by RT64 as F3DEX2\n";
        }
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
