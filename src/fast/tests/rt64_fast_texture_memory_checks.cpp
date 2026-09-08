#include "fast/rt64_fast_texture_memory.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
__attribute__((noinline)) bool referenceEqual(const RT64::FastTextureMemory &a,const RT64::FastTextureMemory &b) {
    return a==b;
}
__attribute__((noinline)) bool vectorEqual(const RT64::FastTextureMemory &a,const RT64::FastTextureMemory &b) {
    return RT64::fastTextureMemoryEqual(a,b);
}
}
int run_texture_memory_checks(FILE *output,FILE *errors,bool benchmark) {
    using Memory=RT64::FastTextureMemory;
    static_assert(alignof(Memory)==1);
    auto *storageA=static_cast<uint8_t *>(std::malloc(sizeof(Memory)+32));
    auto *storageB=static_cast<uint8_t *>(std::malloc(sizeof(Memory)+32));
    if(!storageA || !storageB) { std::free(storageA);std::free(storageB);return 1; }
    try {
        unsigned checked=0;
        for(unsigned alignment=0;alignment<16;++alignment) {
            auto &a=*new(storageA+alignment) Memory;
            auto &b=*new(storageB+15-alignment) Memory;
            for(size_t i=0;i<a.size();++i)a[i]=uint8_t(i*71+(i>>3)+alignment);
            b=a;
            check(vectorEqual(a,b),"Equal TMEM bytes differ");
            for(size_t i=0;i<a.size();++i) {
                b[i]^=uint8_t(1U<<(i&7));
                check(!vectorEqual(a,b),"TMEM comparison missed a changed byte");
                b[i]=a[i];++checked;
            }
            for(const auto pair:std::array<std::array<unsigned,2>,3>{{{0,16},{0,64},{4031,4095}}}) {
                b=a;b[pair[0]]^=1;b[pair[1]]^=1;
                check(!vectorEqual(a,b),"Differences cancelled across vector lanes or chunks");
            }
            a.~Memory();b.~Memory();
        }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        constexpr unsigned neon=1;
#else
        constexpr unsigned neon=0;
#endif
        std::fprintf(output,"PASS: %u byte mutations, 16 pairs of alignments, multiple differences; NEON=%u\n",checked,neon);
        if(benchmark) {
            auto &a=*new(storageA) Memory{};auto &b=*new(storageB) Memory{};
            for(unsigned trial=0;trial<5;++trial) {
                uint64_t elapsed[2]{};unsigned hits=0;
                for(unsigned version=0;version<2;++version) {
                    const auto begin=std::chrono::steady_clock::now();
                    for(unsigned i=0;i<10000;++i) {
                        const unsigned position=(i*67)&4095;
                        a[position]=b[position]=uint8_t(i+trial);
                        hits+=(version?vectorEqual(a,b):referenceEqual(a,b));
                    }
                    elapsed[version]=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
                }
                check(hits==20000,"Benchmark comparison result changed");
                std::fprintf(output,"trial=%u comparisons=10000 reference_us=%llu candidate_us=%llu\n",trial,
                    static_cast<unsigned long long>(elapsed[0]),static_cast<unsigned long long>(elapsed[1]));
            }
            a.~Memory();b.~Memory();
        }
    } catch(const std::exception &error) {
        std::fprintf(errors,"%s\n",error.what());std::free(storageA);std::free(storageB);return 1;
    }
    std::free(storageA);std::free(storageB);return 0;
}
#ifndef RT64_TEXTURE_MEMORY_NO_MAIN
int main(int argc,char **) { return run_texture_memory_checks(stdout,stderr,argc>1); }
#endif
