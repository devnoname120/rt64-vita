#include "rt64_fast.h"
#include "hle/rt64_vi.h"

namespace RT64 {
    void FastDrawSink::present(const VI &vi) { present(vi.origin); }
    bool FastDrawSink::readFramebuffer(uint32_t,uint32_t,std::vector<uint8_t>&) { return false; }
    void FastDrawSink::setRDRAM(const uint8_t*,size_t) {}
    std::shared_ptr<const FastFramebuffer> FastDrawSink::snapshotFramebuffer(uint32_t,uint32_t) { return {}; }
    bool FastDrawSink::readFramebufferSnapshot(const FastFramebuffer&,std::vector<uint8_t>&) { return false; }
}
