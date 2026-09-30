// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// Portable production ownership logic; no physical adapter or GPU pass implied.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace MellowNative {

enum class MemoryStatus : uint8_t {
    Ok, Unavailable, Invalid, Ownership, StaleEpoch, Capacity, Busy,
    IoFailure, Quarantined, ClockRegression
};
struct DeviceIdentity {
    uint16_t vendor {}, device {};
    uint64_t registryId {}, epoch {};
};
enum class CacheDirection : uint8_t { ForDevice, ForCpu };
enum class MemoryState : uint8_t { Empty, Pinned, Mapped, Retiring, Quarantined };
struct MemoryHandle {
    uint32_t slot {UINT32_MAX};
    uint64_t generation {}, registryId {}, epoch {};
};
struct DmaPin {
    void *cookie {};
    uint8_t *cpu {};
    // Immutable device-visible IOVM addresses, not CPU/physical addresses.
    // Adapter owns this array, descriptor, mapper and CPU view until unpin Ok.
    const uint64_t *pages {};
    size_t pageCount {};
    uint64_t bytes {}, deviceRegistryId {};
};
struct GpuMapping {
    void *cookie {};
    uint64_t address {}, bytes {};
    bool writable {};
};
struct MemoryView {
    DeviceIdentity device {};
    MemoryHandle handle {};
    uint64_t owner {}, bytes {};
    MemoryState state {MemoryState::Empty};
    DmaPin pin {};
    GpuMapping mapping {};
    uint32_t jobHolds {};
};
struct MemoryLimits {
    uint8_t dmaAddressBits {46}, gpuAddressBits {48};
    uint64_t maxAllocationBytes {64ULL * 1024 * 1024};
    uint64_t maxTotalBytes {256ULL * 1024 * 1024};
};
struct MemoryBackend {
    void *context {};
    // Trusted physical owner: exclusive device+mapper and exact live epoch.
    bool (*admitted)(void *, const DeviceIdentity &) {};
    // pin may allocate/prepare but MUST NOT publish GPU mappings. Failure with
    // retained resources returns a cleanup pin; IoFailure with no cleanup token
    // is still quarantined, never treated as proof that nothing was acquired.
    // Only Unavailable/Invalid/Ownership/StaleEpoch/Capacity/Busy with an empty
    // pin mean clean rejection. Unknown status values retain cleanup ownership.
    MemoryStatus (*pin)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, uint64_t bytes, DmaPin &) {};
    // Failure retains cleanup ownership (possibly partly unwound) in pin.
    // Ok proves descriptor/DMA completion, mapper/pin/metadata release.
    MemoryStatus (*unpin)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, DmaPin &) {};
    MemoryStatus (*synchronize)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, CacheDirection) {};
    // Ok proves actual contiguous GPU VA ownership, exact held-page publication
    // and completed TLB invalidation. Unavailable guarantees no mapping change.
    // Other failures may have changed hardware and retain mapping/pin ownership.
    // Never synthesize a GPU VA from a CPU/DMA address or identity fallback.
    MemoryStatus (*map)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, bool writable, GpuMapping &) {};
    // Ok proves unpublication + completed invalidation and mapping-lease release.
    // Failure retains a retryable cleanup mapping; never releases the pin.
    MemoryStatus (*unmap)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, GpuMapping &) {};
    // Prove no current/future GPU/firmware/display consumer for this generation.
    // Hold that condition through sync/unmap/unpin under the shared owner lock.
    bool (*retired)(void *, const MemoryView &) {};
    // Actual stop/reset/DMA-idle plus IRQ/worker exclusion for the entire epoch.
    // True also prevents all future submissions; timeout alone cannot return true.
    bool (*quiesce)(void *, const DeviceIdentity &) {};
};

// Trusted kernel API, never user-client pointer input. Fixed bounded metadata;
// caller serializes ALL calls, queue transitions, reset/power and callbacks.
// DMA preparation/synchronization can sleep: no hard-IRQ/gated callback use.
// Ordinary CPU writers obey the ownership lock and cannot write with jobHolds>0.
// Before mutating mapped bytes they additionally prove backend retirement of
// every GPU/firmware/display consumer and hold that exclusion through writing.
// Zero job holds and a subsequent cache synchronization do not prove exclusion.
// A trusted queue may seal private commands/write its producer-owned ring slots
// while holding both channel and owner locks. It must exclude all GPU consumers
// of those bytes until publication; synchronization alone does not exclude them.
// Backend, views, mapper and this object stay alive until close returns Ok.
// No destructor frees live DMA backing. Missing callbacks always fail.
class MemoryOwner {
public:
    static constexpr uint32_t MaxAllocations = 32;
    static constexpr uint64_t PageBytes = 4096, MaxAllocationBytes = 64ULL * 1024 * 1024;
    MemoryOwner() = default;
    MemoryOwner(const MemoryOwner &) = delete;
    MemoryOwner &operator=(const MemoryOwner &) = delete;
    MemoryStatus initialize(DeviceIdentity, MemoryLimits, MemoryBackend);
    MemoryStatus allocate(uint64_t owner, uint64_t bytes, MemoryHandle &);
    MemoryStatus map(uint64_t owner, MemoryHandle, bool writable);
    MemoryStatus inspect(uint64_t owner, MemoryHandle, MemoryView &) const;
    // Queue retains before publishing work and releases ONLY after authoritative
    // completion/rejection or quiescence. A timeout/IRQ is not release authority.
    MemoryStatus retain(uint64_t owner, MemoryHandle);
    MemoryStatus release(uint64_t owner, MemoryHandle);
    MemoryStatus sync(uint64_t owner, MemoryHandle, CacheDirection);
    // Privileged serialized queue path for already retained mapped backing.
    // Cache visibility only: no mutation permission, retirement or completion.
    // A live GPU-written fence requires coherent, non-bounced storage and the
    // adapter's matching DMA/cache ordering plus a separate acquire fence read.
    // No unmap/unpin/hold release occurs, even if synchronization fails.
    MemoryStatus synchronizeRetained(uint64_t owner, MemoryHandle, CacheDirection);
    MemoryStatus retire(uint64_t owner, MemoryHandle);
    // Stops admission first; false keeps all resources and job holds intact.
    // Success does not clear job holds or manufacture completion. Queue drains
    // its references explicitly; retire then performs inverse DMA cleanup.
    MemoryStatus quiesce();
    MemoryStatus close();
    uint64_t chargedBytes() const { return charged_; }
    uint32_t allocations() const;
    bool draining() const { return draining_; }
    bool quiesced() const { return quiesced_; }
    DeviceIdentity device() const { return device_; }
private:
    struct Slot {
        MemoryView view {};
        bool pinOwned {}, mapAttempted {}, mappingUncertain {}, cpuSynchronized {};
    };
    Slot slots_[MaxAllocations] {};
    DeviceIdentity device_ {};
    MemoryLimits limits_ {};
    MemoryBackend backend_ {};
    uint64_t generation_ {1}, charged_ {};
    bool initialized_ {}, draining_ {}, quiesced_ {}, closed_ {};
    MemoryStatus lookup(uint64_t, MemoryHandle, uint32_t &) const;
    bool live() const;
    bool validPin(const Slot &) const;
    bool validMapping(const Slot &) const;
    MemoryStatus quarantine(Slot &);
    MemoryStatus cleanup(Slot &);
};

} // namespace MellowNative
