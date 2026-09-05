#include "rt64_fast_interpreter.h"
#include "gbi/rt64_gbi_extended.h"

namespace RT64 {
    void Interpreter::setup(State *value) { state = value; state->ext.interpreter = this; }

    void Interpreter::loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask) {
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
        if (!state || !hleGBI) throw std::logic_error("RT64 Fast interpreter is not initialized");
        if (start != reinterpret_cast<DisplayList *>(state->fromRDRAM(address))) {
            throw std::invalid_argument("RT64 Fast display-list address mismatch");
        }
        state->returnAddressStack.clear();
        auto *dl = start;
        while (dl) {
            if (!budget--) throw std::runtime_error("RT64 Fast display-list command budget exceeded");
            const uintptr_t ptr = reinterpret_cast<uintptr_t>(dl), base = reinterpret_cast<uintptr_t>(state->RDRAM);
            if (ptr < base || ptr - base > UINT32_MAX || ((ptr - base) & 7)) {
                throw std::runtime_error("RT64 Fast invalid display-list pointer");
            }
            const uint32_t offset = uint32_t(ptr - base);
            state->fromRDRAM(offset, 8);
            const uint8_t op = dl->w0 >> 24;
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
    }
}
