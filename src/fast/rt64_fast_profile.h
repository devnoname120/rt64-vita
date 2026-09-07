#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace RT64::FastProfile {
enum class Stage : size_t { Interpreter, Vertex, Triangle, Prepare, TMEM, Texture, Batch, Draw, Upload, Shader, Readback, Present, Count };
enum class Counter : size_t { Commands, TextureFastHits, TextureHits, TextureMisses, TmemBytes, GLDraws, UploadBytes, StateFlush, CapacityFlush, ShaderMisses, Snapshot, DepthQuery, Count };
constexpr size_t stageCount=size_t(Stage::Count),counterCount=size_t(Counter::Count);
inline constexpr const char *stageNames[]={"interpreter","vertex","triangle","prepare","tmem","texture","batch","draw","upload","shader","readback","present"};
inline constexpr const char *counterNames[]={"commands","texture_fast_hits","texture_hits","texture_misses","tmem_bytes","gl_draws","upload_bytes","state_flush","capacity_flush","shader_misses","snapshot","depth_query"};
struct Stats {
    std::array<uint64_t,stageCount> us{},calls{},items{};
    std::array<uint64_t,counterCount> counters{};
};
struct Context {
    uint64_t (*clock)();
    Stats stats{};
    Stage active=Stage::Count;
    uint64_t last=0;
};
inline thread_local Context *current=nullptr;
class Scope {
    Context *context=current;
    Stage previous=Stage::Count;
    void charge(uint64_t now) {
        if(context->active!=Stage::Count)
            context->stats.us[size_t(context->active)]+=now-context->last;
        context->last=now;
    }
public:
    Scope(Stage stage,uint64_t items=1) {
        if(!context)return;
        charge(context->clock());
        previous=context->active;context->active=stage;
        ++context->stats.calls[size_t(stage)];
        context->stats.items[size_t(stage)]+=items;
    }
    ~Scope() {
        if(!context)return;
        charge(context->clock());
        context->active=previous;
    }
    Scope(const Scope &)=delete;
    Scope &operator=(const Scope &)=delete;
};
inline void count(Counter counter,uint64_t items=1) {
    if(current)current->stats.counters[size_t(counter)]+=items;
}
}
#ifdef RT64_FAST_PROFILE
#define RT64_FAST_SCOPE(stage,items) RT64::FastProfile::Scope rt64_profile_scope{RT64::FastProfile::Stage::stage,items}
#define RT64_FAST_COUNT(counter,items) RT64::FastProfile::count(RT64::FastProfile::Counter::counter,items)
#else
#define RT64_FAST_SCOPE(stage,items) ((void)0)
#define RT64_FAST_COUNT(counter,items) ((void)0)
#endif
