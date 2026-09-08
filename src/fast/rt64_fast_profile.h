#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace RT64::FastProfile {
enum class Stage : size_t { Interpreter, Vertex, Triangle, Prepare, TMEM, Texture, Batch, Draw, Upload, Shader, Readback, Present, DepthResolve, DepthWait, DepthTransfer, Microcode, Sync, TextureLookup, TextureHash, TextureCompare, Count };
enum class Counter : size_t { Commands, TextureFastHits, TextureHits, TextureMisses, TmemBytes, GLDraws, UploadBytes, StateFlush, CapacityFlush, ShaderMisses, Snapshot, DepthQuery, TmemPlainBlockBytes, TmemPlainTileBytes, TmemPaletteBytes, TmemRgba32Bytes, TmemProvenanceBytes, TmemUnalignedBytes, TmemBulkBytes, VertexBulkRecords, Count };
constexpr size_t stageCount=size_t(Stage::Count),counterCount=size_t(Counter::Count);
inline constexpr const char *stageNames[]={"interpreter","vertex","triangle","prepare","tmem","texture","batch","draw","upload","shader","readback","present","depth_resolve","depth_wait","depth_transfer","microcode","sync","texture_lookup","texture_hash","texture_compare"};
inline constexpr const char *counterNames[]={"commands","texture_fast_hits","texture_hits","texture_misses","tmem_bytes","gl_draws","upload_bytes","state_flush","capacity_flush","shader_misses","snapshot","depth_query","tmem_plain_block_bytes","tmem_plain_tile_bytes","tmem_palette_bytes","tmem_rgba32_bytes","tmem_provenance_bytes","tmem_unaligned_bytes","tmem_bulk_bytes","vertex_bulk_records"};
static_assert(sizeof(stageNames)/sizeof(*stageNames)==stageCount);
static_assert(sizeof(counterNames)/sizeof(*counterNames)==counterCount);
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
constexpr bool selected(Stage stage) {
#ifdef RT64_FAST_PROFILE_COARSE
    return stage!=Stage::Triangle && stage!=Stage::Prepare && stage!=Stage::Texture && stage!=Stage::Batch;
#else
    return true;
#endif
}
constexpr bool selected(Counter counter) {
#ifdef RT64_FAST_PROFILE_COARSE
    return counter!=Counter::TextureFastHits && counter!=Counter::Commands
        && counter!=Counter::StateFlush && counter!=Counter::CapacityFlush;
#else
    return true;
#endif
}
struct InactiveScope { InactiveScope(Stage,uint64_t) {} };
template<Stage stage> using SelectedScope=std::conditional_t<selected(stage),Scope,InactiveScope>;
}
#ifdef RT64_FAST_PROFILE
#define RT64_FAST_SCOPE_NAMED(stage,items,name) RT64::FastProfile::SelectedScope<RT64::FastProfile::Stage::stage> name{RT64::FastProfile::Stage::stage,items}
#define RT64_FAST_SCOPE(stage,items) RT64_FAST_SCOPE_NAMED(stage,items,rt64_profile_scope)
#define RT64_FAST_COUNT(counter,items) do { if constexpr(RT64::FastProfile::selected(RT64::FastProfile::Counter::counter)) RT64::FastProfile::count(RT64::FastProfile::Counter::counter,items); } while(false)
#else
#define RT64_FAST_SCOPE_NAMED(stage,items,name) ((void)0)
#define RT64_FAST_SCOPE(stage,items) ((void)0)
#define RT64_FAST_COUNT(counter,items) ((void)0)
#endif
