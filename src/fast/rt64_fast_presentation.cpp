#include "rt64_fast.h"
#include "hle/rt64_vi.h"

namespace RT64 {
    void FastDrawSink::present(const VI &vi) { present(vi.origin); }
    bool FastDrawSink::readFramebuffer(uint32_t,uint32_t,std::vector<uint8_t>&) { return false; }
}
