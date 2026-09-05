#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "xxHash/xxh3.h"
#include "rt64_fast.h"
#include "gbi/rt64_display_list.h"
#include "hle/rt64_microcode.h"

#ifdef RT64_LOG_PRINTF
#undef RT64_LOG_PRINTF
#endif
#define RT64_LOG_PRINTF(...) do {} while (false)

namespace RT64 {
    struct State;
    struct GBI;
    struct Interpreter;

    enum class RDPTriangle {
        Base = G_RDPTRI_BASE, Depth = 1, Textured = 2, Shaded = 4,
        MaxValue = Base | Depth | Textured | Shaded
    };
    constexpr size_t triangleBaseWords = 4, triangleShadeWords = 8,
        triangleTexWords = 8, triangleDepthWords = 2;

    struct FastRSP {
        explicit FastRSP(State *state);
        State *state;
        GBI *gbi = nullptr;
        uint32_t geometryMode = 0, pushMask = 4, projMask = 1, loadMask = 2;
        uint32_t cullFrontMask = 0x1000, cullBackMask = 0x2000;
        std::array<uint32_t, 16> segments{};
        std::array<FastVertex, 256> vertices{};
        std::array<std::array<float,3>,256> screenPositions{};
        std::array<bool, 256> vertexValid{};
        std::vector<interop::float4x4> modelStack;
        interop::float4x4 projection = interop::float4x4::identity();
        interop::float4x4 combined = interop::float4x4::identity();
        bool combinedChanged = true;
        std::array<float, 3> viewportScale{160, 120, 0.5f}, viewportTranslate{160, 120, 0.5f};
        struct Light { std::array<float, 3> color{}, direction{}; };
        std::array<Light, 8> lights{};
        unsigned lightCount = 0;
        std::array<float, 3> lookAtX{1, 0, 0}, lookAtY{0, 1, 0};
        float fogMul = 1, fogOffset = 0, scaleS = 1, scaleT = 1;
        uint8_t textureTile = 0, textureLevels = 0;
        bool textureOn = false;

        void setGBI(GBI *value);
        uint32_t fromSegmented(uint32_t address) const;
        uint32_t fromSegmentedMasked(uint32_t address) const;
        void setSegment(uint32_t segment, uint32_t address);
        void matrix(uint32_t address, uint8_t params);
        void popMatrix(uint32_t count);
        void forceMatrix(uint32_t address);
        void insertMatrix(uint32_t offset, uint32_t value);
        void setModelViewProjChanged(bool value) { combinedChanged = value; }
        void specialComputeModelViewProj();
        void setVertex(uint32_t address, uint32_t count, uint32_t first);
        void modifyVertex(uint32_t vertex, uint32_t where, uint32_t value);
        void drawIndexedTri(uint32_t a, uint32_t b, uint32_t c);
        void branchZ(uint32_t address, uint32_t vertex, uint32_t z, DisplayList **dl);
        void branchW(uint32_t address, uint32_t vertex, uint32_t w, DisplayList **dl);
        void setGeometryMode(uint32_t bits) { geometryMode |= bits; }
        void clearGeometryMode(uint32_t bits) { geometryMode &= ~bits; }
        void modifyGeometryMode(uint32_t mask, uint32_t bits) { geometryMode = (geometryMode & mask) | bits; }
        void setViewport(uint32_t address);
        void setLight(uint32_t index, uint32_t address);
        void setLightCount(uint32_t count);
        void setLightColor(uint32_t index, uint32_t color);
        void setLookAt(uint32_t index, uint32_t address);
        void setLookAtVectors(hlslpp::float3 x, hlslpp::float3 y);
        void setFog(int16_t mul, int16_t offset);
        void setTexture(uint8_t tile, uint8_t levels, uint8_t on, uint16_t s, uint16_t t);
        void setOtherMode(uint32_t high, uint32_t low);
        void setOtherModeH(uint32_t size, uint32_t offset, uint32_t value);
        void setOtherModeL(uint32_t size, uint32_t offset, uint32_t value);
        void setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address);
        void setDepthImage(uint32_t address);
        void setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address);
        // Clip-ratio guards are an RSP rejection optimization; the GL clipper
        // performs geometric clipping. Perspective normalization only affects
        // fixed-point precision and is unnecessary in the float transform path.
        void setClipRatioAll(uint32_t) {}
        void setClipRatioEdge(uint32_t, int32_t) {}
        void setPerspNorm(uint32_t) {}
    };

    struct FastRDP {
        explicit FastRDP(State *state) : state(state) {}
        State *state;
        interop::OtherMode otherMode{};
        FastDraw parameters;
        std::array<FastTile, 8> tiles{};
        std::array<uint8_t, 4096> tmem{};
        struct FramebufferLoad {
            std::shared_ptr<const FastFramebuffer> image;
            std::shared_ptr<const std::vector<uint8_t>> bytes;
            uint32_t references=0;
        };
        std::array<uint32_t,4096> tmemFramebuffer{},tmemSource{};
        std::unordered_map<uint32_t,FramebufferLoad> framebufferLoads;
        uint32_t nextFramebufferLoad=0;
        uint32_t textureAddress = 0, textureWidth = 0;
        uint8_t textureSize = 0, textureFormat = 0, colorSize = 2;
        uint32_t fillColor = 0;
        float primitiveDepth = 0;
        uint64_t tmemGeneration = 0;
        std::array<std::shared_ptr<FastTexture>, 8> decodedTextures{};
        std::array<uint64_t, 8> decodedGenerations{};
        void setFramebufferByte(uint32_t tmemAddress,uint32_t load,uint32_t source);
        bool decodeFramebufferView(const FastTile &tile,FastTexture &texture);
        void materializeFramebufferTMEM(uint32_t load);
        uint8_t readTMEM(uint32_t address);
        std::vector<const DisplayList *> triPointerBuffer;
        std::vector<interop::float4> triPosWorkBuffer, triColWorkBuffer;
        std::vector<interop::float2> triTcWorkBuffer;

        void setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address);
        void setDepthImage(uint32_t address);
        void setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address);
        void setCombine(uint64_t value);
        void setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem,
            uint8_t palette, uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks, uint8_t shiftt, uint8_t shifts);
        void setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt);
        void loadTile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt);
        void loadBlock(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt);
        void loadTLUT(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt);
        void setEnvColor(uint32_t color);
        void setPrimColor(uint8_t lodFrac, uint8_t lodMin, uint32_t color);
        void setBlendColor(uint32_t color);
        void setFogColor(uint32_t color);
        void setFillColor(uint32_t color) { fillColor = color; }
        void setOtherMode(uint32_t high, uint32_t low) { otherMode = {low, high}; }
        void setPrimDepth(uint16_t z, uint16_t dz);
        void setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);
        void setConvert(int32_t k0, int32_t k1, int32_t k2, int32_t k3, int32_t k4, int32_t k5);
        void setKeyR(uint32_t center, uint32_t scale, uint32_t width);
        void setKeyGB(uint32_t cg, uint32_t sg, uint32_t wg, uint32_t cb, uint32_t sb, uint32_t wb);
        void fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);
        void drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile,
            int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip);
        void drawTris(uint32_t count, const float *pos, const float *tc, const float *col, uint8_t tile, uint8_t levels);
        FastDraw makeDraw(uint8_t tile, bool textured);
        std::shared_ptr<const FastTexture> decodeTexture(uint8_t tile);
        void loadTMEM(uint8_t tile, uint32_t start, uint32_t stride, uint32_t words,
            uint32_t rows, bool block, bool palette, uint16_t dxt = 0);
    };

    struct State {
        uint8_t *RDRAM;
        size_t rdramSize;
        FastDrawSink &sink;
        uint64_t memoryEpoch=0;
        std::unique_ptr<FastRSP> rsp;
        std::unique_ptr<FastRDP> rdp;
        Microcode microcode{};
        struct { Interpreter *interpreter = nullptr; } ext;
        struct { bool extendRDRAM = false; } extended;
        std::vector<DisplayList *> returnAddressStack;

        State(uint8_t *rdram, size_t size, FastDrawSink &sink);
        ~State();
        uint8_t *fromRDRAM(uint32_t address, size_t bytes = 8) const;
        uint8_t readU8(uint32_t address) const;
        uint16_t readU16(uint32_t address) const;
        uint32_t readU32(uint32_t address) const;
        void pushReturnAddress(DisplayList *dl);
        DisplayList *popReturnAddress();
        void flush() { sink.flushDraws(); }
        void fullSync() { sink.fullSync(); }
        void dpInterrupt() {} // The runtime owns task completion interrupts.
    };
}
