#include "fast/rt64_fast_profile.h"
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
uint64_t now=0;
uint64_t clockNow() { return now; }
void check(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
}
int main() {
    using namespace RT64::FastProfile;
    try {
        Context context{clockNow};
        current=&context;
        {
            Scope outer{Stage::Interpreter};
            now=5;
            {
                Scope nested{Stage::Vertex,32};
                now=12;
                {
                    Scope inner{Stage::Texture};
                    now=15;
                }
                now=20;
            }
            now=25;
        }
        current=nullptr;
        check(context.stats.us[size_t(Stage::Interpreter)]==10,"interpreter includes child work");
        check(context.stats.us[size_t(Stage::Vertex)]==12,"vertex exclusive duration lost");
        check(context.stats.us[size_t(Stage::Texture)]==3,"texture duration lost");
        check(context.stats.calls[size_t(Stage::Vertex)]==1 && context.stats.items[size_t(Stage::Vertex)]==32,"profile counts differ");
        std::thread other{[&] { check(current==nullptr,"profile context leaked between workers"); }};
        other.join();
        auto previous=context.stats;
        { Scope inactive{Stage::Draw}; now=100; }
        check(previous.us==context.stats.us,"inactive scope modifies a prior sample");
        current=&context;
        count(Counter::TextureHits,7);
        current=nullptr;
        check(context.stats.counters[size_t(Counter::TextureHits)]==7,"counter increments lost");
        std::puts("Fast profiling: exclusive nesting, counts, inactive scopes and thread isolation passed");
        return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what());return 1; }
}
