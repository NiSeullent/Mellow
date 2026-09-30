// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include "../Drivers/NativeGpu/MemoryOwner.hpp"
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNative {

// Mandatory trusted physical VM owner, all callbacks bound to one nonnull context.
// The owner retains the exact PCI device, power/reset/VM/IRQ leases and shared
// sleepable lock. Values cannot come from user input or source-family metadata.
struct PhysicalVmOps {
    void *context {};
    bool (*admitted)(void *, IOPCIDevice *, IOService *, const DeviceIdentity &) {};
    // Proves this actual device uses cache-coherent system RAM for these pages.
    // Does not prove command ordering or GPU job completion. Direct/no-bounce
    // backing is checked independently by the adapter before this callback.
    bool (*coherentSystem)(void *, const DeviceIdentity &, const DmaPin &) {};
    MemoryStatus (*map)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, bool writable, GpuMapping &) {};
    MemoryStatus (*unmap)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, GpuMapping &) {};
    bool (*retired)(void *, const MemoryView &) {};
    bool (*quiesce)(void *, const DeviceIdentity &) {};
};

// Real descriptor/DMA adapter for MemoryOwner. Initial slice accepts exclusively
// owned 4 KiB, coherent, direct system backing. A direct retained "iommu-parent"
// IOMapper object is required; numeric mapper IDs, lookup/wait, system mapper,
// identity addressing and fallback are unavailable. DMA width is configured
// from actual admitted hardware (39..48), independently of GPU address width.
//
// Provider must already be open by this owner. ALL operations, including PCI
// observation, DMA callbacks, submission, reset and detach share the physical
// owner's sleepable lock. IODMACommand may block: no gated/IRQ execution.
// This adapter does not open devices, enable DMA/bus mastering, write registers,
// load firmware, allocate a hardware VM or claim any accelerated GPU support.
// Missing GPU map/unmap/invalidation/retirement/quiesce ownership always fails.
// Failed DMA/descriptor completion latches uncertain cleanup: retry or GPU reset
// cannot repair an unaccounted IOMMU mapping. Resources remain quarantined until
// independent recovery ownership exists; this initial adapter has no such API.
// Calls are trusted kernel APIs; no caller-supplied pointer is dereferenced
// before its handle resolves against the adapter's retained allocation table.
class NativeMemoryIOKit {
public:
    NativeMemoryIOKit() = default;
    NativeMemoryIOKit(const NativeMemoryIOKit &) = delete;
    NativeMemoryIOKit &operator=(const NativeMemoryIOKit &) = delete;
    MemoryStatus attach(IOPCIDevice *, IOService *owner, DeviceIdentity, MemoryLimits,
                        PhysicalVmOps);
    MemoryBackend backend();
    // Requires MemoryOwner.close Ok first. Retained pins make detach Busy.
    // No destructor drops live DMA resources or physical-owner leases.
    MemoryStatus detach();
    uint64_t pinnedBytes() const { return pinnedBytes_; }
    bool attached() const { return device_ != nullptr; }
private:
    struct Resource;
    Resource *resources_[MemoryOwner::MaxAllocations] {};
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOMapper *mapper_ {};
    DeviceIdentity identity_ {};
    MemoryLimits limits_ {};
    PhysicalVmOps vm_ {};
    uint64_t pinnedBytes_ {};
    bool stopped_ {};
    bool observe() const;
    Resource *checked(const DeviceIdentity &, uint64_t, const MemoryHandle &,
                      const DmaPin &, bool complete) const;
    static void describe(Resource &, DmaPin &);
    MemoryStatus releaseResource(Resource &);
    static bool admitted(void *, const DeviceIdentity &);
    static MemoryStatus pin(void *, const DeviceIdentity &, uint64_t,
                            const MemoryHandle &, uint64_t, DmaPin &);
    static MemoryStatus unpin(void *, const DeviceIdentity &, uint64_t,
                              const MemoryHandle &, DmaPin &);
    static MemoryStatus synchronize(void *, const DeviceIdentity &, uint64_t,
                                    const MemoryHandle &, const DmaPin &, CacheDirection);
    static MemoryStatus map(void *, const DeviceIdentity &, uint64_t,
                            const MemoryHandle &, const DmaPin &, bool, GpuMapping &);
    static MemoryStatus unmap(void *, const DeviceIdentity &, uint64_t,
                              const MemoryHandle &, const DmaPin &, GpuMapping &);
    static bool retired(void *, const MemoryView &);
    static bool quiesce(void *, const DeviceIdentity &);
};

} // namespace MellowNative
