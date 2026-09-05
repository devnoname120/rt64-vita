#include "rt64_fast.h"
#include <sstream>

namespace RT64 {
namespace {
    using CC = interop::ColorCombiner;
    std::string colorOperand(CC::ColorInput input, bool swap) {
        const char *t0 = swap ? "texel1" : "texel0", *t1 = swap ? "texel0" : "texel1";
        switch (input) {
        case CC::C_COMBINED: return "combined.rgb";
        case CC::C_TEXEL0: return std::string(t0) + ".rgb";
        case CC::C_TEXEL1: return std::string(t1) + ".rgb";
        case CC::C_PRIMITIVE: return "uPrimitive.rgb";
        case CC::C_SHADE: return "vColor.rgb";
        case CC::C_ENVIRONMENT: return "uEnvironment.rgb";
        case CC::C_KEY_CENTER: return "uKeyCenter";
        case CC::C_KEY_SCALE: return "uKeyScale";
        case CC::C_COMBINED_ALPHA: return "vec3(combined.a)";
        case CC::C_TEXEL0_ALPHA: return "vec3(" + std::string(t0) + ".a)";
        case CC::C_TEXEL1_ALPHA: return "vec3(" + std::string(t1) + ".a)";
        case CC::C_PRIMITIVE_ALPHA: return "vec3(uPrimitive.a)";
        case CC::C_SHADE_ALPHA: return "vec3(vColor.a)";
        case CC::C_ENV_ALPHA: return "vec3(uEnvironment.a)";
        case CC::C_LOD_FRACTION: return "vec3(0.0)";
        case CC::C_PRIM_LOD_FRAC: return "vec3(uLodFraction)";
        case CC::C_NOISE: return "vec3(noise)";
        case CC::C_K4: return "vec3(uK4)";
        case CC::C_K5: return "vec3(uK5)";
        case CC::C_ONE: return "vec3(1.0)";
        default: return "vec3(0.0)";
        }
    }
    std::string alphaOperand(CC::AlphaInput input, bool swap) {
        switch (input) {
        case CC::A_COMBINED: return "combined.a";
        case CC::A_TEXEL0: return swap ? "texel1.a" : "texel0.a";
        case CC::A_TEXEL1: return swap ? "texel0.a" : "texel1.a";
        case CC::A_PRIMITIVE: return "uPrimitive.a";
        case CC::A_SHADE: return "vColor.a";
        case CC::A_ENVIRONMENT: return "uEnvironment.a";
        case CC::A_LOD_FRACTION: return "0.0";
        case CC::A_PRIM_LOD_FRAC: return "uLodFraction";
        case CC::A_ONE: return "1.0";
        default: return "0.0";
        }
    }
    void textureFunctions(std::ostringstream &s, unsigned i, bool filter) {
        s << "uniform sampler2D uTex" << i << ";\n"
          << "uniform vec2 uSize" << i << ";\n"
          << "uniform vec4 uTile" << i << ";\n" // xy shifts, zw upper-left
          << "uniform vec2 uClamp" << i << ";\n" // negative disables clamp
          << "uniform vec2 uMask" << i << ";\n"
          << "uniform vec2 uMirror" << i << ";\n"
          << "vec4 tap" << i << "(vec2 p) {\n"
          << " if (uClamp" << i << ".x >= 0.0) p.x = clamp(p.x, 0.0, uClamp" << i << ".x);\n"
          << " if (uClamp" << i << ".y >= 0.0) p.y = clamp(p.y, 0.0, uClamp" << i << ".y);\n"
          << " vec2 period = uMask" << i << ";\n"
          // vitaGL's translator maps GLSL mod to Cg fmod, whose negative-input
          // result has the wrong sign for N64 wrapping. Spell out floor modulo
          // for both the texel coordinate and the odd/even mirror period.
          << " if (period.x > 0.0) { float wraps = floor(p.x / period.x); float q = wraps - 2.0 * floor(wraps * 0.5); p.x -= wraps * period.x; if (uMirror" << i << ".x > 0.0 && q > 0.0) p.x = period.x - 1.0 - p.x; }\n"
          << " if (period.y > 0.0) { float wraps = floor(p.y / period.y); float q = wraps - 2.0 * floor(wraps * 0.5); p.y -= wraps * period.y; if (uMirror" << i << ".y > 0.0 && q > 0.0) p.y = period.y - 1.0 - p.y; }\n"
          << " return texture2D(uTex" << i << ", (p + 0.5) / uSize" << i << ");\n}\n"
          << "vec4 sample" << i << "(vec2 uv) {\n"
          << " vec2 p = uv * uTile" << i << ".xy - uTile" << i << ".zw;\n";
        if (filter) {
            s << " vec2 f = fract(p); p = floor(p);\n"
              << " if (f.x + f.y <= 1.0) { vec4 c = tap" << i << "(p); return c + f.x * (tap" << i
              << "(p + vec2(1.0, 0.0)) - c) + f.y * (tap" << i << "(p + vec2(0.0, 1.0)) - c); }\n"
              << " vec4 c = tap" << i << "(p + vec2(1.0)); return c + (1.0 - f.x) * (tap" << i
              << "(p + vec2(0.0, 1.0)) - c) + (1.0 - f.y) * (tap" << i << "(p + vec2(1.0, 0.0)) - c);\n}\n";
        } else s << " return tap" << i << "(floor(p));\n}\n";
    }
}
    std::string fastVertexShader() {
        return R"(#version 100
precision highp float;
attribute vec4 aPosition;
attribute vec2 aUV;
attribute vec4 aColor;
attribute float aFog;
varying vec2 vUV;
varying vec4 vColor;
varying float vFog;
void main() {
    gl_Position = aPosition;
    vUV = aUV;
    vColor = aColor;
    vFog = aFog;
}
)";
    }
    std::array<uint32_t,4> fastShaderKey(const FastDraw &draw) {
        if(draw.fill) return {0,0,0,0x80000000U};
        const uint32_t cycle=draw.otherMode.cycleType();
        const bool filter=draw.otherMode.textFilt()!=G_TF_POINT && cycle!=G_CYC_COPY;
        const uint32_t mode=cycle | draw.otherMode.alphaCompare() | (uint32_t(filter)<<24)
            | (uint32_t(draw.otherMode.cvgXAlpha())<<8)
            | (uint32_t(draw.otherMode.alphaCvgSel() && !draw.otherMode.cvgXAlpha())<<9);
        const uint32_t inputs=uint32_t(bool(draw.textures[0])) | (uint32_t(bool(draw.textures[1]))<<1)
            | (uint32_t(draw.fog)<<2);
        return {cycle==G_CYC_COPY?0:draw.combine.L,cycle==G_CYC_COPY?0:draw.combine.H,mode,inputs};
    }
    std::string fastFragmentShader(const FastDraw &draw) {
        std::ostringstream s;
        s << R"(#version 100
precision highp float;
varying vec2 vUV;
varying vec4 vColor;
varying float vFog;
uniform vec4 uPrimitive, uEnvironment, uFogColor, uBlendColor, uFillColor;
uniform vec3 uKeyCenter, uKeyScale;
uniform float uLodFraction, uK4, uK5, uFrame;
uniform vec4 uTextureAlphaMix;
)";
        if (draw.fill) { s << "void main() { gl_FragColor = uFillColor; }\n"; return s.str(); }
        const bool filter = draw.otherMode.textFilt() != G_TF_POINT && draw.otherMode.cycleType() != G_CYC_COPY;
        for (unsigned i = 0; i < 2; ++i) if (draw.textures[i]) textureFunctions(s, i, filter);
        s << "void main() {\n";
        s << "float noise = fract(sin(dot(gl_FragCoord.xy + uFrame, vec2(12.9898, 78.233))) * 43758.5453);\n";
        for (unsigned i = 0; i < 2; ++i) {
            s << "vec4 texel" << i << " = ";
            if (draw.textures[i]) s << "sample" << i << "(vUV);\n";
            else s << "vec4(1.0);\n";
            // The zero-valued uniform keeps alpha live through texture filtering.
            // The tested Vita3K/Vulkan path misrenders the compiler's RGB-only
            // filtering variant, including when coverage later replaces alpha.
            if (draw.textures[i]) s << "texel" << i << " += texel" << i << ".aaaa * uTextureAlphaMix;\n";
        }
        s << "vec4 combined = vec4(0.0);\n";
        const bool twoCycle = draw.otherMode.cycleType() == G_CYC_2CYCLE;
        if (draw.otherMode.cycleType() == G_CYC_COPY) s << "combined = texel0;\n";
        else for (unsigned cycle = twoCycle ? 0 : 1; cycle < 2; ++cycle) {
            // In one-cycle mode the RDP uses the second mux. The texture swap
            // between cycle 0 and cycle 1 applies only in two-cycle mode.
            const bool swap = twoCycle && cycle == 1;
            auto c = [&](unsigned i) { return colorOperand(draw.combine.decodeColorInput(i, cycle == 1), swap); };
            auto a = [&](unsigned i) { return alphaOperand(draw.combine.decodeAlphaInput(i, cycle == 1), swap); };
            s << "combined = vec4((" << c(0) << " - " << c(1) << ") * " << c(2) << " + " << c(3)
              << ", (" << a(0) << " - " << a(1) << ") * " << a(2) << " + " << a(3) << ");\n";
        }
        s << "combined = clamp(combined, 0.0, 1.0);\n";
        if (draw.otherMode.alphaCompare() == G_AC_THRESHOLD) s << "if (combined.a < uBlendColor.a) discard;\n";
        if (draw.otherMode.alphaCompare() == G_AC_DITHER) s << "if (combined.a < noise) discard;\n";
        if (draw.otherMode.cvgXAlpha()) s << "if (combined.a < 0.125) discard;\n";
        if (draw.otherMode.alphaCvgSel() && !draw.otherMode.cvgXAlpha()) s << "combined.a = 1.0;\n";
        if (draw.fog) s << "combined.rgb = mix(combined.rgb, uFogColor.rgb, clamp(vFog, 0.0, 1.0));\n";
        s << "gl_FragColor = combined;\n}\n";
        return s.str();
    }
}
