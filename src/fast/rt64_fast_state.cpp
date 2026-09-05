#include "rt64_fast_state.h"

namespace RT64 {
    State::State(uint8_t *rdram, size_t size, FastDrawSink &sink)
        : RDRAM(rdram), rdramSize(size), sink(sink),
          rsp(std::make_unique<FastRSP>(this)), rdp(std::make_unique<FastRDP>(this)) {
        if (!rdram || size < 4096 || (size & 3) || (reinterpret_cast<uintptr_t>(rdram) & 3)) {
            throw std::invalid_argument("RT64 Fast requires aligned, word-swapped RDRAM");
        }
        sink.setRDRAM(rdram,size);
    }
    State::~State() = default;
    uint8_t *State::fromRDRAM(uint32_t address, size_t bytes) const {
        if (address > rdramSize || bytes > rdramSize - address) {
            throw std::out_of_range("RT64 Fast RDRAM access outside supplied memory");
        }
        return RDRAM + address;
    }
    uint8_t State::readU8(uint32_t address) const { return *fromRDRAM(address ^ 3, 1); }
    uint16_t State::readU16(uint32_t address) const {
        return (uint16_t(readU8(address)) << 8) | readU8(address + 1);
    }
    uint32_t State::readU32(uint32_t address) const {
        return (uint32_t(readU16(address)) << 16) | readU16(address + 2);
    }
    void State::pushReturnAddress(DisplayList *dl) {
        if (returnAddressStack.size() >= 32) throw std::runtime_error("RT64 Fast display-list stack overflow");
        returnAddressStack.push_back(dl);
    }
    DisplayList *State::popReturnAddress() {
        if (returnAddressStack.empty()) return nullptr;
        auto *ret = returnAddressStack.back();
        returnAddressStack.pop_back();
        return ret;
    }
}
