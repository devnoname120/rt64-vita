#include "fast/rt64_fast_profile.h"
#include <cstdio>
#include <stdexcept>

namespace {
unsigned clocks=0;
uint64_t tick() { return ++clocks; }
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
}
int main() {
    using namespace RT64::FastProfile;
    try {
        Context context{tick};
        current=&context;
        { RT64_FAST_SCOPE(Triangle,1); }
        { RT64_FAST_SCOPE(Prepare,1); }
        { RT64_FAST_SCOPE(Batch,3); }
        { RT64_FAST_SCOPE(Texture,1); }
        check(clocks==0,"Coarse profiling still timestamps per-triangle scopes");
        { RT64_FAST_SCOPE(Vertex,32); }
        check(clocks==2 && context.stats.calls[size_t(Stage::Vertex)]==1,
              "Coarse profiling lost the vertex-load span");
        RT64_FAST_COUNT(TextureFastHits,1);
        RT64_FAST_COUNT(TextureHits,7);
        check(context.stats.counters[size_t(Counter::TextureFastHits)]==0,
              "Coarse profiling still accesses per-triangle TLS counters");
        check(context.stats.counters[size_t(Counter::TextureHits)]==7,"Coarse cache-miss path counters are missing");
        current=nullptr;
        const auto before=clocks;
        { RT64_FAST_SCOPE(Draw,3); }
        check(clocks==before,"Inactive coarse profiling reads the clock");
        std::puts("Coarse profiling excludes per-triangle clocks and hot counters while retaining outer work");
    } catch(const std::exception &error) { std::fprintf(stderr,"%s\n",error.what());return 1; }
}
