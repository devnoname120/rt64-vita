// RT64 reduced renderer. The public draw interface has no graphics API types.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "shared/rt64_color_combiner.h"

namespace RT64 {
    struct VI;
    struct FastVertex {
        float position[4] = {0, 0, 0, 1}; // OpenGL clip coordinates.
        float uv[2] = {};                // Unnormalized N64 texel coordinates.
        float color[4] = {1, 1, 1, 1};
        float fog = 0;
    };

    struct FastTile {
        uint8_t fmt = 0, siz = 0, palette = 0;
        uint8_t cmt = 0, cms = 0, maskt = 0, masks = 0, shiftt = 0, shifts = 0;
        uint16_t line = 0, tmem = 0, uls = 0, ult = 0, lrs = 0, lrt = 0;
    };

    struct FastTexture {
        uint32_t width = 0, height = 0;
        uint64_t hash = 0;
        std::vector<uint8_t> rgba;
    };

    struct FastDraw {
        interop::OtherMode otherMode{};
        interop::ColorCombiner combine{};
        std::array<float, 4> primitive{1, 1, 1, 1}, environment{}, fogColor{}, blendColor{};
        std::array<float, 3> keyCenter{}, keyScale{};
        float lodFraction = 0, k4 = 0, k5 = 0;
        uint32_t colorAddress = 0, depthAddress = 0;
        uint32_t width = 320, height = 240;
        uint32_t colorBytes = 2;
        std::array<int32_t, 4> scissor{0, 0, 1280, 960}; // 10.2 screen coordinates.
        bool depthTest = false, depthWrite = false, fog = false;
        bool cullFront = false, cullBack = false, rectangle = false;
        bool fill = false, clearDepth = false;
        std::array<float, 4> fillColor{};
        std::array<FastTile, 2> tiles{};
        std::array<std::shared_ptr<const FastTexture>, 2> textures{};
        std::vector<FastVertex> vertices;
    };

    // Called synchronously on the renderer thread. A sink must consume or copy
    // the draw before returning. Context creation and all GL calls use that thread.
    struct FastDrawSink {
        virtual ~FastDrawSink() = default;
        virtual void draw(const FastDraw &draw) = 0;
        virtual void fullSync() = 0;
        virtual void present(uint32_t colorAddress) = 0;
        // Real scanout carries visibility and gamma as well as an address. The
        // address-only form remains useful for isolated draw diagnostics.
        virtual void present(const VI &vi);
        // Read an already rendered color-image range in N64 byte order (RGBA16
        // or RGBA32), top scanline first. False means the range is not resident;
        // callers should retain the existing RDRAM contents in that case.
        virtual bool readFramebuffer(uint32_t address,uint32_t size,std::vector<uint8_t> &bytes);
    };

    // GLSL ES 1.00; combiner operands are decoded by RT64's shared ColorCombiner.
    std::string fastVertexShader();
    std::string fastFragmentShader(const FastDraw &draw);
    // Only state which changes generated GLSL belongs in the program cache.
    std::array<uint32_t,4> fastShaderKey(const FastDraw &draw);

    // Merge consecutive draws with identical state. Texture objects remain
    // immutable after publication; sync and presentation flush queued geometry.
    std::unique_ptr<FastDrawSink> createFastBatchingSink(std::unique_ptr<FastDrawSink> backend);

#ifdef RT64_FAST_VITAGL
    std::unique_ptr<FastDrawSink> createFastVitaGLSink(bool waitForVblank = true, bool batching = true);
#endif
#ifdef RT64_FAST_GLES2
    // The caller owns the current GLES2 context. This shares the draw/shader
    // implementation with Vita; the host probe uses it for differential tests.
    std::unique_ptr<FastDrawSink> createFastGLES2Sink(std::function<void()> swapBuffers, bool batching = true);
#endif
}
