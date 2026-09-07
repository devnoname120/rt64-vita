#include "rt64_fast.h"
#include "rt64_fast_profile.h"
#include <tuple>

namespace RT64 {
namespace {
    bool sameTile(const FastTile &a,const FastTile &b) {
        auto key=[](const FastTile &t) {
            return std::tie(t.fmt,t.siz,t.palette,t.cmt,t.cms,t.maskt,t.masks,t.shiftt,t.shifts,
                t.line,t.tmem,t.uls,t.ult,t.lrs,t.lrt);
        };
        return key(a)==key(b);
    }
    bool sameState(const FastDraw &a,const FastDraw &b) {
        auto key=[](const FastDraw &d) {
            return std::tie(d.otherMode.L,d.otherMode.H,d.combine.L,d.combine.H,d.primitive,d.environment,
                d.fogColor,d.blendColor,d.keyCenter,d.keyScale,d.lodFraction,d.k4,d.k5,
                d.colorAddress,d.depthAddress,d.memoryEpoch,d.width,d.height,d.colorBytes,d.scissor,
                d.depthTest,d.depthWrite,d.fog,d.cullFront,d.cullBack,d.rectangle,d.fill,d.clearDepth,
                d.fillColor,d.textures);
        };
        return key(a)==key(b) && sameTile(a.tiles[0],b.tiles[0]) && sameTile(a.tiles[1],b.tiles[1]);
    }
    class FastBatchingSink final : public FastDrawSink {
        static constexpr size_t maxVertices=6144;
        std::unique_ptr<FastDrawSink> backend;
        FastDraw pending;
        bool pendingOverlaps(uint32_t address,uint32_t size) const {
            if(pending.vertices.empty() || !size)return false;
            if(pending.width>1024 || pending.height>1024 || (pending.colorBytes!=2 && pending.colorBytes!=4))return true;
            const uint64_t end=uint64_t(pending.colorAddress)+uint64_t(pending.width)*pending.height*pending.colorBytes;
            return uint64_t(address)<end && uint64_t(pending.colorAddress)<uint64_t(address)+size;
        }
        void flush() {
            if(!pending.vertices.empty()) {
                backend->draw(pending);
                pending.vertices.clear();
            }
        }
    public:
        explicit FastBatchingSink(std::unique_ptr<FastDrawSink> backend) : backend(std::move(backend)) {}
        void draw(const FastDraw &draw) override {
            RT64_FAST_SCOPE(Batch,draw.vertices.size());
            // Depth clears derive their bounds from the individual rectangle.
            // Empty draws can still create targets in the backend.
            if(draw.clearDepth || draw.vertices.empty() || draw.vertices.size()>maxVertices) {
                flush(); backend->draw(draw); return;
            }
            if(!pending.vertices.empty()) {
                if(!sameState(pending,draw)) { RT64_FAST_COUNT(StateFlush,1);flush(); }
                else if(pending.vertices.size()+draw.vertices.size()>maxVertices) {
                    RT64_FAST_COUNT(CapacityFlush,1);flush();
                }
            }
            if(pending.vertices.empty()) {
                pending=draw;
                pending.vertices.reserve(maxVertices);
            } else pending.vertices.insert(pending.vertices.end(),draw.vertices.begin(),draw.vertices.end());
        }
        void fullSync() override { flush(); backend->fullSync(); }
        void flushDraws() override { flush(); backend->flushDraws(); }
        void present(uint32_t address) override { flush(); backend->present(address); }
        void present(const VI &vi) override { flush(); backend->present(vi); }
        bool readFramebuffer(uint32_t address,uint32_t size,std::vector<uint8_t> &bytes) override {
            flush(); return backend->readFramebuffer(address,size,bytes);
        }
        bool readDepthFramebuffer(uint32_t address,uint32_t size,std::vector<uint8_t> &bytes) override {
            flush(); return backend->readDepthFramebuffer(address,size,bytes);
        }
        void setRDRAM(const uint8_t *rdram,size_t size) override { flush(); backend->setRDRAM(rdram,size); }
        void setMemoryWriteTracking(std::function<void(uint32_t,uint32_t,bool)> watch) override {
            flush(); backend->setMemoryWriteTracking(std::move(watch));
        }
        void notifyMemoryWrites(const std::vector<FastMemoryWrite> &writes) override {
            flush(); backend->notifyMemoryWrites(writes);
        }
        std::shared_ptr<const FastFramebuffer> snapshotFramebuffer(uint32_t address,uint32_t size) override {
            // An ordinary RAM upload cannot observe queued color writes. Unknown
            // or resident views retain the barrier, including larger aliases.
            if(!maySnapshotFramebuffer(address,size))return {};
            flush(); return backend->snapshotFramebuffer(address,size);
        }
        bool maySnapshotFramebuffer(uint32_t address,uint32_t size) const override {
            return pendingOverlaps(address,size) || backend->maySnapshotFramebuffer(address,size);
        }
        bool readFramebufferSnapshot(const FastFramebuffer &snapshot,std::vector<uint8_t> &bytes) override {
            flush(); return backend->readFramebufferSnapshot(snapshot,bytes);
        }
    };
}
    std::unique_ptr<FastDrawSink> createFastBatchingSink(std::unique_ptr<FastDrawSink> backend) {
        return std::make_unique<FastBatchingSink>(std::move(backend));
    }
}
