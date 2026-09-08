#include "rt64_fast_interpreter.h"
#include "rt64_fast_profile.h"
#include "gbi/rt64_gbi_extended.h"
#include "gbi/rt64_gbi_f3d.h"
#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_f3dex2.h"

namespace RT64 {
    void Interpreter::setup(State *value) { state = value; state->ext.interpreter = this; }

    void Interpreter::loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask) {
        RT64_FAST_SCOPE(Microcode,1);
        textAddress &= 0xfffff8;
        dataAddress &= 0xfffff8;
        // GBIManager probes up to 0x2000 text bytes and 0x1000 data bytes.
        state->fromRDRAM(textAddress, 0x2000);
        state->fromRDRAM(dataAddress, 0x1000);
        hleGBI = gbiManager.getGBIForUCode(state->RDRAM, textAddress, dataAddress);
        if (!hleGBI) throw std::runtime_error("RT64 Fast: unrecognized graphics microcode");
        state->rsp->setGBI(hleGBI);
        auto reset = resetFromTask ? hleGBI->resetFromTask : hleGBI->resetFromLoad;
        if (reset) reset(state);
    }

    void Interpreter::processDisplayLists(uint32_t address, DisplayList *start, size_t budget) {
        RT64_FAST_SCOPE(Interpreter,1);
        if (!state || !hleGBI) throw std::logic_error("RT64 Fast interpreter is not initialized");
        if (start != reinterpret_cast<DisplayList *>(state->fromRDRAM(address))) {
            throw std::invalid_argument("RT64 Fast display-list address mismatch");
        }
        state->returnAddressStack.clear();
        if(!++state->memoryEpoch) ++state->memoryEpoch;
        struct RunGuard {
            FastRSP &rsp;
            ~RunGuard() { rsp.cancelTriangleRun(); }
        } runGuard{*state->rsp};
        GBI *triangleGBI=nullptr;
        std::array<bool,256> triangleCommands{};
        auto *dl = start;
        while (dl) {
            RT64_FAST_COUNT(Commands,1);
            if (!budget--) throw std::runtime_error("RT64 Fast display-list command budget exceeded");
            const uintptr_t ptr = reinterpret_cast<uintptr_t>(dl), base = reinterpret_cast<uintptr_t>(state->RDRAM);
            if (ptr < base || ptr - base > UINT32_MAX || ((ptr - base) & 7)) {
                throw std::runtime_error("RT64 Fast invalid display-list pointer");
            }
            const uint32_t offset = uint32_t(ptr - base);
            state->fromRDRAM(offset, 8);
            const uint8_t op = dl->w0 >> 24;
            if(hleGBI!=triangleGBI) {
                for(unsigned i=0;i<triangleCommands.size();++i) {
                    const auto fn=hleGBI->map[i];
                    triangleCommands[i]=batchTriangleRuns && (fn==GBI_F3D::tri1 || fn==GBI_F3D::quad
                        || fn==GBI_F3DEX::tri1 || fn==GBI_F3DEX::tri2 || fn==GBI_F3DEX::quad
                        || fn==GBI_F3DEX2::tri1 || fn==GBI_F3DEX2::tri2 || fn==GBI_F3DEX2::quad);
                }
                triangleGBI=hleGBI;
            }
            // Only known pure triangle handlers share preparation. Every other
            // command is an ordering boundary, including calls, loads and extensions.
#ifndef RT64_FAST_REFERENCE_DRAW
            if(triangleCommands[op] && (!extendedOpCode || op!=extendedOpCode))state->rsp->beginTriangleRun();
            else state->rsp->endTriangleRun();
#endif
            // Original HLE texture rectangles consume two trailing half commands.
            if (op == G_TEXRECT || op == G_TEXRECTFLIP) state->fromRDRAM(offset, 24);
            if (extendedOpCode && op == extendedOpCode) {
                GBI_EXTENDED::extendedOp(state, &dl);
            } else {
                auto fn = hleGBI->map[op];
                if (!fn) {
                    char message[100];
                    std::snprintf(message, sizeof(message), "RT64 Fast unsupported opcode 0x%02x at 0x%08x", op, offset);
                    throw std::runtime_error(message);
                }
                fn(state, &dl);
            }
            if (dl) ++dl;
        }
        state->rsp->endTriangleRun();
        state->flush();
    }
}
