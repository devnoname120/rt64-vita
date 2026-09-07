#include "fast/rt64_fast_ranges.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
size_t allocations=0;
bool countAllocations=false;
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
}
void *operator new(size_t bytes) {
    if(countAllocations)++allocations;
    if(void *p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p,size_t) noexcept { std::free(p); }

int main() {
    try {
        std::map<uint64_t,uint64_t> ranges;
        RT64::fastMarkGpuBytes(ranges,32,64);
        check(RT64::fastContainsGpuBytes(ranges,32,64),"An exact owned range was not recognized");
        check(RT64::fastContainsGpuBytes(ranges,40,50),"Interior GPU ownership was not recognized");
        check(!RT64::fastContainsGpuBytes(ranges,31,64) && !RT64::fastContainsGpuBytes(ranges,32,65),"Ownership escaped range bounds");
        RT64::fastMarkGpuBytes(ranges,80,96);
        check(!RT64::fastContainsGpuBytes(ranges,48,88),"A row gap was treated as owned");
        countAllocations=true;
        for(unsigned n=0;n<1000;++n)RT64::fastMarkGpuBytes(ranges,32,64);
        countAllocations=false;
        std::printf("Repeated covered range allocations: %zu\n",allocations);
        check(allocations==0,"Identical GPU byte ownership recreated its map allocation");
        for(uint64_t high:{0ULL,0x100000000ULL}) {
            for(unsigned seed=0;seed<100;++seed) {
                ranges.clear();std::array<bool,128> oracle{};
                uint32_t random=seed+1;
                for(unsigned step=0;step<200;++step) {
                    random=random*1664525U+1013904223U;
                    const unsigned begin=(random>>16)%128;
                    random=random*1664525U+1013904223U;
                    const unsigned end=(random>>16)%129;
                    RT64::fastMarkGpuBytes(ranges,high+begin,high+end);
                    for(unsigned i=begin;i<end;++i)oracle[i]=true;
                    std::array<bool,128> actual{};
                    uint64_t last=0;bool first=true;
                    for(const auto &entry:ranges) {
                        check(entry.first<entry.second && (first || last<entry.first),"GPU byte ranges overlap or fail to coalesce");
                        for(uint64_t i=entry.first;i<entry.second;++i)actual.at(i-high)=true;
                        last=entry.second;first=false;
                    }
                    check(actual==oracle,"GPU byte ownership differs from the reference union");
                    if(begin<end) {
                        bool covered=true;
                        for(unsigned i=begin;i<end;++i)covered=covered && oracle[i];
                        check(RT64::fastContainsGpuBytes(ranges,high+begin,high+end)==covered,"Ownership containment differs from reference bytes");
                    }
                }
            }
        }
        std::puts("Range union boundaries, overlap, adjacency and covered-range reuse passed");
        return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what());return 1; }
}
