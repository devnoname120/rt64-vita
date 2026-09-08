#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace RT64 {
using FastTextureMemory = std::array<uint8_t,4096>;

// Cache hashes only select candidates. Always compare every TMEM byte before
// reusing an image. Byte-vector loads support the arrays' natural alignment.
inline bool fastTextureMemoryEqual(const FastTextureMemory &a,const FastTextureMemory &b) {
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(RT64_FAST_REFERENCE_TEXTURE_COMPARE)
    uint8x16_t d0=vdupq_n_u8(0),d1=d0,d2=d0,d3=d0;
    for(std::size_t i=0;i<a.size();i+=64) {
        d0=vorrq_u8(d0,veorq_u8(vld1q_u8(a.data()+i),vld1q_u8(b.data()+i)));
        d1=vorrq_u8(d1,veorq_u8(vld1q_u8(a.data()+i+16),vld1q_u8(b.data()+i+16)));
        d2=vorrq_u8(d2,veorq_u8(vld1q_u8(a.data()+i+32),vld1q_u8(b.data()+i+32)));
        d3=vorrq_u8(d3,veorq_u8(vld1q_u8(a.data()+i+48),vld1q_u8(b.data()+i+48)));
    }
    const uint64x2_t bits=vreinterpretq_u64_u8(vorrq_u8(vorrq_u8(d0,d1),vorrq_u8(d2,d3)));
    return (vgetq_lane_u64(bits,0)|vgetq_lane_u64(bits,1))==0;
#else
    return std::memcmp(a.data(),b.data(),a.size())==0;
#endif
}
}
