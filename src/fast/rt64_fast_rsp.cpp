#include "rt64_fast_interpreter.h"
#include "gbi/rt64_f3d.h"
#include "shared/rt64_blender.h"

namespace RT64 {
namespace {
    interop::float4x4 multiply(const interop::float4x4 &a, const interop::float4x4 &b) {
        interop::float4x4 out{};
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                for (unsigned k = 0; k < 4; ++k) out[r][c] += a[r][k] * b[k][c];
        return out;
    }
    interop::float4x4 loadMatrix(State &state, uint32_t address) {
        state.fromRDRAM(address, 64);
        interop::float4x4 out{};
        for (unsigned i = 0; i < 16; ++i) {
            out[i / 4][i % 4] = float(int16_t(state.readU16(address + 2 * i))) +
                state.readU16(address + 32 + 2 * i) / 65536.0f;
        }
        return out;
    }
    void normalize(std::array<float, 3> &v) {
        float length = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (length > 0) for (auto &x : v) x /= length;
    }
    float dot(const std::array<float, 3> &a, const std::array<float, 3> &b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    }
    uint32_t replaceBits(uint32_t old, uint32_t size, uint32_t offset, uint32_t value) {
        if (!size || size > 32 || offset > 32 - size) throw std::runtime_error("RT64 Fast invalid other-mode field");
        uint32_t mask = (UINT32_MAX >> (32 - size)) << offset;
        return (old & ~mask) | (value & mask);
    }
}
    FastRSP::FastRSP(State *state) : state(state), modelStack{interop::float4x4::identity()} {}
    void FastRSP::setGBI(GBI *value) {
        gbi = value;
        pushMask = gbi->constants.at(F3DENUM::G_MTX_PUSH);
        projMask = gbi->constants.at(F3DENUM::G_MTX_PROJECTION);
        loadMask = gbi->constants.at(F3DENUM::G_MTX_LOAD);
        cullFrontMask = gbi->constants.at(F3DENUM::G_CULL_FRONT);
        cullBackMask = gbi->constants.at(F3DENUM::G_CULL_BACK);
        if (gbi->flags.pointLighting) throw std::runtime_error("RT64 Fast point lighting not implemented");
    }
    uint32_t FastRSP::fromSegmented(uint32_t address) const {
        if (state->extended.extendRDRAM && (address & 0x80000000)) return address & 0x7fffffff;
        return segments[(address >> 24) & 15] + (address & 0xffffff);
    }
    uint32_t FastRSP::fromSegmentedMasked(uint32_t address) const {
        return fromSegmented(address) & (state->extended.extendRDRAM && (address & 0x80000000) ? 0x7ffffff8 : 0xfffff8);
    }
    void FastRSP::setSegment(uint32_t index, uint32_t address) {
        if (index >= segments.size()) throw std::out_of_range("RT64 Fast segment index");
        segments[index] = address & 0xffffff;
    }
    void FastRSP::matrix(uint32_t address, uint8_t params) {
        auto loaded = loadMatrix(*state, fromSegmentedMasked(address));
        if (params & projMask) {
            projection = (params & loadMask) ? loaded : multiply(loaded, projection);
        } else {
            // Match RT64's bounded hardware stack: an extra push retains the
            // current top, and the matrix operation still applies to it.
            if ((params & pushMask) && modelStack.size()<32) {
                modelStack.push_back(modelStack.back());
            }
            modelStack.back() = (params & loadMask) ? loaded : multiply(loaded, modelStack.back());
        }
        combinedChanged = true;
    }
    void FastRSP::popMatrix(uint32_t count) {
        const size_t removed=std::min<size_t>(count,modelStack.size()-1);
        if(removed) {
            modelStack.resize(modelStack.size()-removed);
            combinedChanged = true;
        }
    }
    void FastRSP::forceMatrix(uint32_t address) {
        combined = loadMatrix(*state, fromSegmentedMasked(address));
        combinedChanged = false;
    }
    void FastRSP::specialComputeModelViewProj() {
        combined = multiply(modelStack.back(), projection);
        combinedChanged = false;
    }
    void FastRSP::insertMatrix(uint32_t offset, uint32_t value) {
        if (offset >= 64 || (offset & 3)) throw std::runtime_error("RT64 Fast invalid matrix insertion");
        if (combinedChanged) specialComputeModelViewProj();
        const unsigned i = (offset & 31) / 2;
        for (unsigned j = 0; j < 2; ++j) {
            auto &v = combined[(i + j) / 4][(i + j) % 4];
            const uint16_t half = value >> (16 * (1 - j));
            if (offset & 32) v = std::floor(v) + half / 65536.0f;
            else v = int16_t(half) + (v - std::floor(v));
        }
    }
    void FastRSP::setVertex(uint32_t address, uint32_t count, uint32_t first) {
        if (first > vertices.size() || count > vertices.size() - first) throw std::runtime_error("RT64 Fast vertex range");
        address = fromSegmentedMasked(address);
        state->fromRDRAM(address, count * 16);
        if (combinedChanged) specialComputeModelViewProj();
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t src = address + i * 16;
            auto &v = vertices[first + i];
            const float p[4] = {float(int16_t(state->readU16(src))), float(int16_t(state->readU16(src + 2))),
                float(int16_t(state->readU16(src + 4))), 1};
            for (unsigned c = 0; c < 4; ++c) {
                v.position[c] = 0;
                for (unsigned r = 0; r < 4; ++r) v.position[c] += p[r] * combined[r][c];
            }
            v.uv[0] = int16_t(state->readU16(src + 8)) / 32.0f * scaleS;
            v.uv[1] = int16_t(state->readU16(src + 10)) / 32.0f * scaleT;
            for (unsigned c = 0; c < 4; ++c) v.color[c] = state->readU8(src + 12 + c) / 255.0f;
            if (geometryMode & G_LIGHTING) {
                std::array<float, 3> normal{};
                for (unsigned c = 0; c < 3; ++c) normal[c] = int8_t(state->readU8(src + 12 + c)) / 127.0f;
                // Match RT64's computeDirLight/computeTextureGen: transform
                // directions into local space, then normalize the directions.
                // Normalizing a transformed vertex normal differs under scale.
                auto localDirection = [&](const std::array<float, 3> &direction) {
                    std::array<float, 3> local{};
                    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c)
                        local[r] += modelStack.back()[r][c] * direction[c];
                    normalize(local);
                    return local;
                };
                for (unsigned c = 0; c < 3; ++c) v.color[c] = lights[lightCount].color[c];
                for (unsigned l = 0; l < lightCount; ++l) {
                    const float intensity = std::max(0.0f, dot(normal, localDirection(lights[l].direction)));
                    for (unsigned c = 0; c < 3; ++c) v.color[c] += intensity * lights[l].color[c];
                }
                for (unsigned c = 0; c < 3; ++c) v.color[c] = std::min(v.color[c], 1.0f);
                if (geometryMode & G_TEXTURE_GEN) {
                    const float x = std::clamp(dot(normal, localDirection(lookAtX)), -1.0f, 1.0f);
                    const float y = std::clamp(dot(normal, localDirection(lookAtY)), -1.0f, 1.0f);
                    if (geometryMode & G_TEXTURE_GEN_LINEAR) {
                        v.uv[0] = std::acos(-x) * (1024.0f / 3.141592654f) * scaleS;
                        v.uv[1] = std::acos(-y) * (1024.0f / 3.141592654f) * scaleT;
                    } else {
                        v.uv[0] = (x + 1) * 512 * scaleS;
                        v.uv[1] = (y + 1) * 512 * scaleT;
                    }
                }
            }
            const float invW = v.position[3] != 0 ? 1.0f / v.position[3] : 0;
            if (geometryMode & G_FOG) {
                v.color[3] = std::clamp((std::max(v.position[2],0.0f) * invW * fogMul + fogOffset) / 255.0f, 0.0f, 1.0f);
            }
            v.fog = v.color[3];
            // Viewport transformation belongs to vertex loading, just like
            // matrix/lighting state. Later viewport commands must not move
            // vertices that are already in the RSP cache.
            if(v.position[3]==0) v.position[3]=1e-6f;
            const float w=v.position[3];
            screenPositions[first+i]={v.position[0]/w*viewportScale[0]+viewportTranslate[0],
                -v.position[1]/w*viewportScale[1]+viewportTranslate[1],
                v.position[2]/w*viewportScale[2]+viewportTranslate[2]};
            vertexValid[first + i] = true;
        }
    }
    void FastRSP::modifyVertex(uint32_t index, uint32_t where, uint32_t value) {
        if (index >= vertices.size() || !vertexValid[index]) throw std::runtime_error("RT64 Fast unloaded vertex modification");
        auto &v = vertices[index];
        switch (where) {
        case G_MWO_POINT_RGBA:
            for (unsigned c = 0; c < 4; ++c) v.color[c] = ((value >> (24 - 8*c)) & 255) / 255.0f;
            v.fog = v.color[3];
            break;
        case G_MWO_POINT_ST:
            v.uv[0] = int16_t(value >> 16) / 32.0f; v.uv[1] = int16_t(value) / 32.0f;
            break;
        case G_MWO_POINT_XYSCREEN:
            screenPositions[index][0]=int16_t(value>>16)/4.0f;
            screenPositions[index][1]=int16_t(value)/4.0f;
            break;
        case G_MWO_POINT_ZSCREEN:
            screenPositions[index][2]=value/65536.0f;
            break;
        default: throw std::runtime_error("RT64 Fast unsupported vertex modification");
        }
    }
    void FastRSP::drawIndexedTri(uint32_t a, uint32_t b, uint32_t c) {
        for (auto i : {a, b, c}) if (i >= vertices.size() || !vertexValid[i]) throw std::runtime_error("RT64 Fast triangle uses unloaded vertex");
        auto draw = state->rdp->makeDraw(textureTile, textureOn);
        draw.depthTest = (geometryMode & 1U) && draw.otherMode.zCmp();
        draw.depthWrite = (geometryMode & 1U) && draw.otherMode.zUpd();
        draw.cullFront = geometryMode & cullFrontMask;
        draw.cullBack = geometryMode & cullBackMask;
        draw.fog = interop::Blender::usesStandardFogCycle(draw.otherMode);
        draw.vertices.reserve(3);
        for (auto i : {a, b, c}) {
            auto v = vertices[i];
            const float w = v.position[3];
            // Convert cached N64 screen coordinates to this color image's
            // clip space; current RSP viewport state no longer participates.
            v.position[0] = (2*screenPositions[i][0]/draw.width-1)*w;
            v.position[1] = (1-2*screenPositions[i][1]/draw.height)*w;
            v.position[2] = (2*screenPositions[i][2]-1)*w;
            if (draw.otherMode.zSource() == G_ZS_PRIM) v.position[2] = state->rdp->primitiveDepth * w;
            draw.vertices.push_back(v);
        }
        if (!(geometryMode & gbi->constants.at(F3DENUM::G_SHADING_SMOOTH)))
            for (auto &v : draw.vertices) std::copy(std::begin(vertices[a].color), std::end(vertices[a].color), v.color);
        state->sink.draw(draw);
    }
    void FastRSP::branchZ(uint32_t address, uint32_t index, uint32_t z, DisplayList **dl) {
        if (index >= vertices.size() || !vertexValid[index]) throw std::runtime_error("RT64 Fast branch uses unloaded vertex");
        const float screenZ=screenPositions[index][2];
        if (screenZ * 1024.0f < z / 65536.0f) *dl = reinterpret_cast<DisplayList *>(state->fromRDRAM(fromSegmentedMasked(address))) - 1;
    }
    void FastRSP::branchW(uint32_t address, uint32_t index, uint32_t w, DisplayList **dl) {
        if (index >= vertices.size() || !vertexValid[index]) throw std::runtime_error("RT64 Fast branch uses unloaded vertex");
        if (vertices[index].position[3] < float(w)) *dl = reinterpret_cast<DisplayList *>(state->fromRDRAM(fromSegmentedMasked(address))) - 1;
    }
    void FastRSP::setViewport(uint32_t address) {
        address = fromSegmentedMasked(address);
        state->fromRDRAM(address, 16);
        for (unsigned i = 0; i < 3; ++i) {
            const float divisor = i == 2 ? 1024.0f : 4.0f;
            viewportScale[i] = int16_t(state->readU16(address + 2*i)) / divisor;
            viewportTranslate[i] = int16_t(state->readU16(address + 8 + 2*i)) / divisor;
        }
    }
    void FastRSP::setLight(uint32_t index, uint32_t address) {
        if (index >= lights.size()) throw std::runtime_error("RT64 Fast light index");
        address = fromSegmentedMasked(address);
        state->fromRDRAM(address, 12);
        for (unsigned c = 0; c < 3; ++c) {
            lights[index].color[c] = state->readU8(address + c) / 255.0f;
            lights[index].direction[c] = int8_t(state->readU8(address + 8 + c));
        }
        normalize(lights[index].direction);
    }
    void FastRSP::setLightCount(uint32_t count) {
        if (count > 7) throw std::runtime_error("RT64 Fast too many lights");
        lightCount = count;
    }
    void FastRSP::setLightColor(uint32_t index, uint32_t value) {
        if (index >= lights.size()) throw std::runtime_error("RT64 Fast light index");
        for (unsigned c = 0; c < 3; ++c) lights[index].color[c] = ((value >> (24 - 8*c)) & 255) / 255.0f;
    }
    void FastRSP::setLookAt(uint32_t index, uint32_t address) {
        if (index > 1) throw std::runtime_error("RT64 Fast look-at index");
        address = fromSegmentedMasked(address);
        auto &v = index == 0 ? lookAtX : lookAtY;
        for (unsigned c = 0; c < 3; ++c) v[c] = int8_t(state->readU8(address + 8 + c));
        normalize(v);
    }
    void FastRSP::setLookAtVectors(hlslpp::float3 x, hlslpp::float3 y) {
        for (unsigned c = 0; c < 3; ++c) { lookAtX[c] = x[c]; lookAtY[c] = y[c]; }
    }
    void FastRSP::setFog(int16_t mul, int16_t offset) { fogMul = mul; fogOffset = offset; }
    void FastRSP::setTexture(uint8_t tile, uint8_t levels, uint8_t on, uint16_t s, uint16_t t) {
        textureTile = tile; textureLevels = levels; textureOn = on != 0;
        scaleS = s / 65536.0f; scaleT = t / 65536.0f;
    }
    void FastRSP::setOtherMode(uint32_t high, uint32_t low) { state->rdp->setOtherMode(high, low); }
    void FastRSP::setOtherModeH(uint32_t size, uint32_t offset, uint32_t value) {
        auto &mode = state->rdp->otherMode; mode.H = replaceBits(mode.H, size, offset, value);
    }
    void FastRSP::setOtherModeL(uint32_t size, uint32_t offset, uint32_t value) {
        auto &mode = state->rdp->otherMode; mode.L = replaceBits(mode.L, size, offset, value);
    }
    void FastRSP::setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        state->rdp->setColorImage(fmt, siz, width, fromSegmented(address));
    }
    void FastRSP::setDepthImage(uint32_t address) { state->rdp->setDepthImage(fromSegmented(address)); }
    void FastRSP::setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        state->rdp->setTextureImage(fmt, siz, width, fromSegmented(address));
    }
}
