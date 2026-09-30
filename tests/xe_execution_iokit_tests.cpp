// Actual production staging + VM + IOKit DMA adapter + execution scheduler.
// OS/mapper/GGTT/GuC/IRQ/coherence authority is SIMULATED; no GPU is exercised.
#include "xe_execution_iokit_shim.hpp"
#include "../Mellow/XeExecutionIOKit.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <vector>
using namespace XeContext;
namespace Mock = NativeMemoryShim;
static size_t checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Fixture {
    static constexpr uint64_t Owner = 51;
    XeMemory::VirtualMemory vm;
    XeMemory::Allocation slots[8] {};
    XeMemory::Handle handles[6] {};
    IOMapper *mapper {new IOMapper};
    XeMemory::IOKitContext pins;
    IOBufferMemoryDescriptor *buffers[6] {};
    IODMACommand *commands[6] {};
    uint8_t *cpu[6] {};
    uint32_t ring[1024] {}, tail {};
    bool admit {true}, stopped {true}, physicalHeld {}, retainOk {true}, releaseOk {true}, contextSyncOk {true};
    unsigned retained {}, released {}, contextSyncs {}, originalStages {}, syncFailAt {};
    bool verifyAtSync {};
    XeDispatch::Policy policy {112, 3, false, false};
    std::unique_ptr<XeDispatch::Prepared> prepared {new XeDispatch::Prepared};
    std::unique_ptr<IOKitExecutionStaging> adapter;
    ExecutionBackend executionBackend;
    LiveContext context() const { return {Owner, 7, 19, 5, 0x200000, 0x210000, 4096,
        const_cast<uint32_t *>(ring), const_cast<uint32_t *>(&tail), 0x200119, false}; }
    ExecutionBackend physical() {
        ExecutionBackend b; b.opaque = this;
        b.admitted = [](void *p, const LiveContext &c, const XeDispatch::Policy &policy) {
            auto &f = *static_cast<Fixture *>(p);
            return f.admit && c.owner == Owner && c.epoch == 7 && c.allocation == 19 &&
                policy.maxFrontEndThreads == 112 && policy.mocsIndex == 3;
        };
        b.freshStopped = [](void *p, const LiveContext &) { auto &f = *static_cast<Fixture *>(p); return f.stopped && !f.tail; };
        b.retainContext = [](void *p, const LiveContext &) {
            auto &f = *static_cast<Fixture *>(p); if (!f.retainOk || f.physicalHeld) return false;
            f.physicalHeld = true; ++f.retained; return true;
        };
        b.releaseContext = [](void *p, const LiveContext &) {
            auto &f = *static_cast<Fixture *>(p); if (!f.releaseOk || !f.stopped || !f.physicalHeld) return false;
            f.physicalHeld = false; ++f.released; return true;
        };
        b.stageHeaps = [](void *p, const LiveContext &, const XeMemory::Handle (&)[6], const XeDispatch::Prepared &) {
            ++static_cast<Fixture *>(p)->originalStages; return false;
        };
        b.synchronizeContext = [](void *p, const LiveContext &c) {
            auto &f = *static_cast<Fixture *>(p); ++f.contextSyncs;
            CHECK(f.physicalHeld && c.ringCpu == f.ring && c.lrcTailCpu == &f.tail);
            return f.contextSyncOk;
        };
        b.quiesced = [](void *p, const LiveContext &) { return static_cast<Fixture *>(p)->stopped; };
        return b;
    }
    Fixture(const XeZebin::Image &image, uint64_t dataBytes = 4096, uint64_t outputBytes = 0) {
        Mock::reset(); pins.mapper = mapper;
        auto b = XeMemory::makeIOKitPinBackend(pins); b.verifiedPatIndices = 1U << 3;
        b.bind = [](void *, uint64_t, const XeMemory::Pin &, uint8_t, bool) { return XeMemory::Status::Ok; };
        b.unbind = [](void *, uint64_t, uint64_t) { return XeMemory::Status::Ok; };
        b.fenceComplete = [](void *, XeMemory::Fence) { return true; };
        CHECK(vm.initialize(slots, 8, 0x10000, 0x100000000, b) == XeMemory::Status::Ok);
        for (size_t i = 0; i < 6; ++i) {
            CHECK(vm.reserve(Owner, i < 4 ? 4096 : (i == 4 || !outputBytes ? dataBytes : outputBytes),
                4096, handles[i]) == XeMemory::Status::Ok);
            CHECK(vm.pin(Owner, handles[i]) == XeMemory::Status::Ok);
            XeExecutionShim::disjointDma(slots[i].pin, i);
            CHECK(vm.bind(Owner, handles[i], 3, true) == XeMemory::Status::Ok);
            buffers[i] = Mock::lastBuffer; commands[i] = Mock::lastCommand;
            cpu[i] = static_cast<uint8_t *>(XeMemory::resolvePinnedBuffer(pins, Owner, slots[i].bytes, slots[i].pin));
            CHECK(cpu[i] != nullptr);
            for (size_t j = 0; j < slots[i].bytes; ++j) cpu[i][j] = uint8_t(i * 31 + j * 17);
        }
        CHECK(XeDispatch::prepareBoundEvidence(image, vm, Owner, handles, policy, 1234, 32, *prepared) == XeDispatch::Error::None);
        adapter.reset(new IOKitExecutionStaging(vm, pins, physical())); executionBackend = adapter->backend();
        CHECK(executionBackend.opaque == adapter.get() && executionBackend.stageHeaps);
        Mock::faults.syncContext = this; Mock::faults.afterSync = [](void *p, IOOptionBits direction) {
            auto &f = *static_cast<Fixture *>(p); CHECK(direction == kIODirectionOut);
            if (f.verifyAtSync) {
                const uint8_t *sources[] = {f.prepared->isa, f.prepared->indirect,
                    reinterpret_cast<const uint8_t *>(f.prepared->surface), reinterpret_cast<const uint8_t *>(f.prepared->batch)};
                for (size_t i = 0; i < 4; ++i) CHECK(std::memcmp(f.cpu[i], sources[i], 4096) == 0);
            }
            if (f.syncFailAt && Mock::syncCalls + 1 == f.syncFailAt) Mock::faults.synchronize = true;
        };
    }
    ~Fixture() {
        // Only simulated ownership is restored here; production inverses still
        // run through the real VM and IOKit adapter, with faults cleared.
        Mock::faults = {}; stopped = true; releaseOk = true;
        if (adapter->contextHeld()) CHECK(executionBackend.releaseContext(executionBackend.opaque, context()));
        for (size_t i = 0; i < 6; ++i) {
            while (slots[i].activeUses) CHECK(vm.releaseUse(Owner, handles[i]) == XeMemory::Status::Ok);
            CHECK(vm.retire(Owner, handles[i]) == XeMemory::Status::Ok);
            CHECK(vm.reclaim(Owner, handles[i]) == XeMemory::Status::Ok);
        }
        CHECK(!pins.pinnedBytes && !physicalHeld && originalStages == 0);
        adapter.reset(); mapper->release();
        CHECK(Mock::objects.empty() && Mock::allocations.empty());
        CHECK(Mock::unsafeClearCalls == 0 && Mock::prematureDestroy == 0);
    }
    void hold() {
        CHECK(executionBackend.admitted(executionBackend.opaque, context(), policy));
        CHECK(executionBackend.retainContext(executionBackend.opaque, context()));
        for (auto h : handles) CHECK(vm.retainUse(Owner, h) == XeMemory::Status::Ok);
    }
    bool stage() { return executionBackend.stageHeaps(executionBackend.opaque, context(), handles, *prepared); }
    void unmodified() const {
        for (size_t i = 0; i < 6; ++i) for (size_t j = 0; j < slots[i].bytes; ++j)
            CHECK(cpu[i][j] == uint8_t(i * 31 + j * 17));
        CHECK(Mock::syncCalls == 0 && adapter->report().copiedBytes == 0 && tail == 0);
    }
};
struct ExecutionFixture {
    Fixture &f;
    XeGuC::Transport transport;
    XeGuC::Descriptor h {}, g {};
    uint32_t hw[1024] {}, gw[2048] {};
    XeFence::Timeline fence;
    alignas(8) volatile uint64_t completion {};
    unsigned notifications {};
    explicit ExecutionFixture(Fixture &fixture) : f(fixture) {
        XeGuC::Ops o; o.opaque = this;
        o.acquire = o.release = [](void *) { return true; };
        o.admitted = [](void *p, uint64_t e) { return e == 7 && static_cast<ExecutionFixture *>(p)->f.admit; };
        o.authorizeAction = [](void *p, uint64_t, const XeGuC::Action &) {
            auto &s = *static_cast<ExecutionFixture *>(p);
            CHECK(s.f.physicalHeld && s.f.tail == 80 && Mock::syncCalls == 6);
            CHECK(s.f.adapter->report().status == IOKitStagingStatus::Staged); return true;
        };
        o.notify = [](void *p) { ++static_cast<ExecutionFixture *>(p)->notifications; return true; };
        MellowXe::FirmwareInfo fw; fw.release = {70, 53, 0}; fw.submission = {1, 26, 0};
        CHECK(transport.attach({&h, hw, 1024, 0x400000, 0x410000}, {&g, gw, 2048, 0x400040, 0x420000},
            o, 7, fw) == XeGuC::Status::Ok);
        XeFence::Ops fo; fo.opaque = this;
        fo.valid = [](void *p, const XeFence::Slot &s) { auto &t = *static_cast<ExecutionFixture *>(p); return t.f.admit && s.owner == Fixture::Owner && s.epoch == 7; };
        fo.stopped = [](void *p, const XeFence::Slot &) { return static_cast<ExecutionFixture *>(p)->f.stopped; };
        fo.retain = [](void *, const XeFence::Slot &) { return true; };
        fo.release = [](void *, const XeFence::Slot &) {};
        CHECK(fence.bind({&completion, 0x220000, 8, Fixture::Owner, 5, 7, 0, 0}, fo) == XeFence::Status::Ok);
    }
};
// This deliberately repurposes trailing private scratch in a malformed kernel
// fixture. It is not a normal backing allocation, portable layout API or
// production access path. The existing class declares that bounded uint64_t
// array last; these host targets have its alignment and no trailing padding.
static uint64_t *malformedScratch(IOKitExecutionStaging &adapter) {
    auto *end = reinterpret_cast<uint8_t *>(&adapter) + sizeof(adapter);
    auto *scratch = end - IOKitExecutionStaging::MaxPages * sizeof(uint64_t);
    CHECK((reinterpret_cast<uintptr_t>(scratch) & (alignof(uint64_t) - 1)) == 0);
    return reinterpret_cast<uint64_t *>(scratch);
}
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "compiler-evidence/mellow_evidence_mtl.bin";
    std::ifstream file(path, std::ios::binary); CHECK(bool(file));
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    XeZebin::Image image; CHECK(image.parse(data.data(), data.size()) == XeZebin::Error::None);
    { Fixture f(image); f.hold();
      // Handles overlapping report_ must be rejected before report_ is reset.
      struct SixHandles { XeMemory::Handle values[6]; };
      auto *storage = const_cast<IOKitStagingReport *>(&f.adapter->report());
      auto *aliased = new (storage) SixHandles;
      for (size_t i = 0; i < 6; ++i) aliased->values[i] = f.handles[i];
      XeMemory::Handle original[6]; std::memcpy(original, aliased->values, sizeof(original));
      CHECK(!f.executionBackend.stageHeaps(f.executionBackend.opaque, f.context(), aliased->values, *f.prepared));
      CHECK(std::memcmp(original, aliased->values, sizeof(original)) == 0);
      CHECK(Mock::syncCalls == 0 && f.tail == 0);
      aliased->~SixHandles(); new (storage) IOKitStagingReport; }
    { Fixture f(image, 64ULL * 1024 * 1024); f.hold();
      // The maximum page scratch would overwrite this complete immutable
      // Prepared before copying or validating its surfaces in the old source.
      const auto scratch = reinterpret_cast<uintptr_t>(malformedScratch(*f.adapter));
      const auto aligned = (scratch + alignof(XeDispatch::Prepared) - 1) &
          ~(uintptr_t(alignof(XeDispatch::Prepared)) - 1);
      auto *aliased = new (reinterpret_cast<void *>(aligned)) XeDispatch::Prepared(*f.prepared);
      std::vector<uint8_t> original(sizeof(*aliased)); std::memcpy(original.data(), aliased, original.size());
      CHECK(!f.executionBackend.stageHeaps(f.executionBackend.opaque, f.context(), f.handles, *aliased));
      CHECK(std::memcmp(original.data(), aliased, original.size()) == 0);
      CHECK(Mock::syncCalls == 0 && f.tail == 0);
      aliased->~Prepared(); }
    { Fixture f(image); f.hold();
      auto *scratch = malformedScratch(*f.adapter); const auto originalPage = f.slots[5].pin.dmaPages[0];
      scratch[0] = originalPage;
      { XeExecutionShim::AliasedPinPages alias(f.slots[5].pin, scratch);
        CHECK(!f.stage()); CHECK(scratch[0] == originalPage); }
      f.unmodified(); }
    { Fixture f(image); XeMemory::VirtualMemory malformedVm;
      auto adapter = std::make_unique<IOKitExecutionStaging>(malformedVm, f.pins, f.physical());
      struct EightAllocations { XeMemory::Allocation values[8]; };
      auto *storage = new (malformedScratch(*adapter)) EightAllocations;
      CHECK(malformedVm.initialize(storage->values, 8, 0x10000, 0x100000000, {}) == XeMemory::Status::Ok);
      auto b = adapter->backend(); CHECK(b.admitted(b.opaque, f.context(), f.policy));
      CHECK(b.retainContext(b.opaque, f.context()));
      for (auto h : f.handles) CHECK(f.vm.retainUse(Fixture::Owner, h) == XeMemory::Status::Ok);
      // Duplicate views only to construct forbidden storage; canonical pins
      // remain owned/cleaned exactly once by the original fixture VM.
      for (size_t i = 0; i < 8; ++i) storage->values[i] = f.slots[i];
      uint8_t original[sizeof(*storage)]; std::memcpy(original, storage, sizeof(original));
      CHECK(!b.stageHeaps(b.opaque, f.context(), f.handles, *f.prepared));
      CHECK(std::memcmp(original, storage, sizeof(original)) == 0);
      CHECK(Mock::syncCalls == 0 && f.tail == 0);
      CHECK(b.releaseContext(b.opaque, f.context()));
      storage->~EightAllocations(); }
    { Fixture f(image); f.hold(); f.verifyAtSync = true; CHECK(f.stage());
      CHECK(f.adapter->report().status == IOKitStagingStatus::Staged);
      CHECK(f.adapter->report().copiedHeaps == 4 && f.adapter->report().copiedBytes == 16384 &&
            f.adapter->report().synchronizedAllocations == 6 && Mock::syncCalls == 6);
      for (size_t i = 4; i < 6; ++i) for (size_t j = 0; j < f.slots[i].bytes; ++j)
          CHECK(f.cpu[i][j] == uint8_t(i * 31 + j * 17));
      CHECK(f.tail == 0 && f.contextSyncs == 0);
      f.contextSyncOk = false;
      CHECK(!f.executionBackend.synchronizeContext(f.executionBackend.opaque, f.context()) && f.contextSyncs == 1);
      f.releaseOk = false;
      CHECK(!f.executionBackend.releaseContext(f.executionBackend.opaque, f.context()) && f.adapter->contextHeld());
      CHECK(!f.executionBackend.retainContext(f.executionBackend.opaque, f.context()) && f.retained == 1); }
    { Fixture f(image); CHECK(!f.stage()); f.unmodified();
      CHECK(!f.executionBackend.synchronizeContext(f.executionBackend.opaque, f.context()) && f.contextSyncs == 0); }
    for (unsigned missing = 0; missing < 7; ++missing) { Fixture f(image); auto b = f.physical();
        if (missing == 0) b.opaque = nullptr;
        if (missing == 1) b.admitted = nullptr;
        if (missing == 2) b.freshStopped = nullptr;
        if (missing == 3) b.retainContext = nullptr;
        if (missing == 4) b.releaseContext = nullptr;
        if (missing == 5) b.synchronizeContext = nullptr;
        if (missing == 6) b.quiesced = nullptr;
        auto adapter = std::make_unique<IOKitExecutionStaging>(f.vm, f.pins, b);
        CHECK(!adapter->backend().opaque); f.unmodified(); }
    for (unsigned mode = 0; mode < 18; ++mode) { Fixture f(image); f.hold();
        const auto oldHandle = f.handles[5]; const auto oldState = f.slots[5].state;
        const auto oldBytes = f.buffers[5]->bytes; auto *oldCpu = f.buffers[5]->cpu;
        auto *oldDescriptor = f.commands[5]->memory; const auto oldPage = f.slots[5].pin.dmaPages[0];
        if (mode == 0) ++f.handles[5].generation;
        if (mode == 1) f.slots[5].state = XeMemory::State::Retiring;
        if (mode == 2) CHECK(f.vm.releaseUse(Fixture::Owner, f.handles[5]) == XeMemory::Status::Ok);
        if (mode == 3) CHECK(f.vm.retainUse(Fixture::Owner, f.handles[5]) == XeMemory::Status::Ok);
        if (mode == 4) f.buffers[5]->bytes = 8192;
        if (mode == 5) f.commands[5]->memory = nullptr;
        if (mode == 6) f.buffers[5]->cpu = f.cpu[4];
        if (mode == 7) const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[0] = f.slots[4].pin.dmaPages[0];
        if (mode == 8) ++const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[0];
        if (mode == 9) const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[0] = XeMemory::DmaLimit;
        if (mode == 10) ++f.prepared->layout.batch;
        if (mode == 11) f.prepared->surface[24] += 4096;
        if (mode == 12) f.prepared->count = 1025;
        if (mode == 13) f.prepared->batchDwords = 1025;
        if (mode == 14) f.admit = false;
        if (mode == 15) f.stopped = false;
        if (mode == 16) f.handles[5] = f.handles[4];
        if (mode == 17) f.prepared->walkerOffset = f.prepared->batchDwords;
        CHECK(!f.stage());
        f.handles[5] = oldHandle; f.slots[5].state = oldState;
        f.buffers[5]->bytes = oldBytes; f.buffers[5]->cpu = oldCpu;
        f.commands[5]->memory = oldDescriptor; const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[0] = oldPage;
        f.unmodified(); }
    { Fixture f(image); f.hold(); auto foreign = f.context(); ++foreign.owner;
      CHECK(!f.executionBackend.stageHeaps(f.executionBackend.opaque, foreign, f.handles, *f.prepared)); f.unmodified();
      CHECK(!f.executionBackend.releaseContext(f.executionBackend.opaque, foreign) && f.adapter->contextHeld()); }
    { Fixture f(image); XeMemory::IOKitContext foreign; foreign.mapper = f.mapper;
      auto adapter = std::make_unique<IOKitExecutionStaging>(f.vm, foreign, f.physical());
      auto b = adapter->backend(); CHECK(b.admitted(b.opaque, f.context(), f.policy));
      CHECK(b.retainContext(b.opaque, f.context()));
      for (auto h : f.handles) CHECK(f.vm.retainUse(Fixture::Owner, h) == XeMemory::Status::Ok);
      CHECK(!b.stageHeaps(b.opaque, f.context(), f.handles, *f.prepared));
      CHECK(adapter->report().status == IOKitStagingStatus::Invalid && adapter->report().copiedBytes == 0);
      CHECK(b.releaseContext(b.opaque, f.context()));
      CHECK(XeMemory::resolvePinnedBuffer(foreign, Fixture::Owner, 4096, f.slots[0].pin) == nullptr); f.unmodified(); }
    { Fixture f(image, 8192); f.hold();
      const auto old = f.slots[5].pin.dmaPages[1];
      const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[1] = f.slots[5].pin.dmaPages[0];
      CHECK(!f.stage()); const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[1] = old; f.unmodified(); }
    { Fixture f(image, 4096, 8192); f.hold(); CHECK(f.stage());
      CHECK(f.adapter->report().synchronizedAllocations == 6 && f.adapter->report().copiedBytes == 16384); }
    { Fixture f(image, 4096, 8192); f.hold(); const auto old = f.slots[5].pin.dmaPages[1];
      // Alias lies beyond the bytes touched by the evidence kernel; complete
      // retained extents, rather than just count*4, still have to be disjoint.
      const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[1] = f.slots[0].pin.dmaPages[0];
      CHECK(!f.stage()); const_cast<uint64_t *>(f.slots[5].pin.dmaPages)[1] = old; f.unmodified(); }
    { Fixture f(image); f.hold(); auto stale = f.handles[5];
      CHECK(f.vm.releaseUse(Fixture::Owner, stale) == XeMemory::Status::Ok);
      const auto address = f.slots[5].address;
      CHECK(f.vm.retire(Fixture::Owner, stale) == XeMemory::Status::Ok);
      CHECK(f.vm.reclaim(Fixture::Owner, stale) == XeMemory::Status::Ok);
      XeMemory::Handle replacement;
      CHECK(f.vm.reserveAt(Fixture::Owner, address, 4096, replacement) == XeMemory::Status::Ok);
      CHECK(replacement.slot == stale.slot && replacement.generation != stale.generation);
      CHECK(f.vm.pin(Fixture::Owner, replacement) == XeMemory::Status::Ok);
      XeExecutionShim::disjointDma(f.slots[5].pin, 5);
      CHECK(f.vm.bind(Fixture::Owner, replacement, 3, true) == XeMemory::Status::Ok);
      CHECK(f.vm.retainUse(Fixture::Owner, replacement) == XeMemory::Status::Ok);
      f.buffers[5] = Mock::lastBuffer; f.commands[5] = Mock::lastCommand;
      f.cpu[5] = static_cast<uint8_t *>(XeMemory::resolvePinnedBuffer(f.pins, Fixture::Owner, 4096, f.slots[5].pin));
      for (size_t j = 0; j < 4096; ++j) f.cpu[5][j] = uint8_t(5 * 31 + j * 17);
      CHECK(!f.stage()); f.unmodified();
      f.handles[5] = replacement; CHECK(f.stage()); }
    { Fixture f(image, 32768); f.hold();
      // The staging source itself may not live in any destination allocation,
      // including input/output whose CPU bytes this stage does not initialize.
      auto *aliased = new (f.cpu[4]) XeDispatch::Prepared(*f.prepared);
      CHECK(!f.executionBackend.stageHeaps(f.executionBackend.opaque, f.context(), f.handles, *aliased));
      CHECK(Mock::syncCalls == 0 && f.adapter->report().copiedBytes == 0);
      aliased->~Prepared(); }
    { Fixture f(image, 64ULL * 1024 * 1024); f.hold(); CHECK(f.stage());
      CHECK(f.adapter->report().copiedBytes == 16384 && Mock::syncCalls == 6);
      CHECK(f.cpu[5][f.slots[5].bytes - 1] == uint8_t(5 * 31 + (f.slots[5].bytes - 1) * 17)); }
    for (unsigned failure = 1; failure <= 6; ++failure) { Fixture f(image); f.hold();
        f.syncFailAt = failure; Mock::faults.synchronize = failure == 1;
        CHECK(!f.stage()); CHECK(f.adapter->report().status == IOKitStagingStatus::DmaFailure);
        CHECK(f.adapter->report().copiedBytes == 16384 &&
            f.adapter->report().synchronizedAllocations == failure - 1 && Mock::syncCalls == failure);
        CHECK(f.tail == 0 && f.contextSyncs == 0 && f.adapter->contextHeld());
    }
    for (unsigned failure = 0; failure < 3; ++failure) { Fixture f(image); ExecutionFixture t(f);
        auto execution = std::make_unique<EvidenceExecution>(f.vm, t.transport, t.fence, f.executionBackend);
        if (failure == 1) Mock::faults.synchronize = true;
        if (failure == 2) f.contextSyncOk = false;
        auto result = execution->begin(image, f.context(), f.handles, f.policy, 1234, 32, false, 1, 100);
        if (failure) {
            CHECK(result == (failure == 1 ? ExecutionStatus::Unavailable : ExecutionStatus::Quarantined) &&
                t.h.tail == 0 && t.notifications == 0);
            CHECK(execution->retainedVmUses() == 6 && execution->contextHeld());
            CHECK(f.tail == (failure == 1 ? 0U : 80U));
        } else {
            CHECK(result == ExecutionStatus::Pending && f.tail == 80 && t.notifications == 2 && t.h.tail == 17);
            CHECK(f.adapter->report().status == IOKitStagingStatus::Staged && f.contextSyncs == 1);
            CHECK(execution->retainedVmUses() == 6 && execution->contextHeld());
            // Simulated correlated GuC reply and GPU completion are deliberately
            // separate from staging/DMA synchronization in this host test.
            t.gw[0] = 3; t.gw[1] = 0x90001002; t.gw[2] = 5; t.gw[3] = 1; t.g.tail = 4;
            XeGuC::Message m; CHECK(t.transport.receive(7, 2, m) == XeGuC::Status::Ok);
            CHECK(execution->poll(2) == ExecutionStatus::Pending);
            t.completion = 1; CHECK(execution->poll(3) == ExecutionStatus::Ok);
            CHECK(execution->retainedVmUses() == 0);
        }
        Mock::faults.synchronize = false; f.stopped = true;
        CHECK(execution->close() == ExecutionStatus::Ok);
        CHECK(!execution->contextHeld() && !f.adapter->contextHeld() && f.released == 1);
    }
    std::printf("XeExecutionIOKit: PASS %zu checks; real staging/VM/IOKit/execution code, OS and hardware authority simulated\n", checks);
}
