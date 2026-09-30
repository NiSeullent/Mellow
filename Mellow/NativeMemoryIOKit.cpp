// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "NativeMemoryIOKit.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

namespace MellowNative {
namespace {
bool same(DeviceIdentity a, DeviceIdentity b) {
    return a.vendor == b.vendor && a.device == b.device &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
bool same(MemoryHandle a, MemoryHandle b) {
    return a.slot == b.slot && a.generation == b.generation &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
bool empty(const GpuMapping &m) {
    return !m.cookie && !m.address && !m.bytes && !m.writable;
}
}
struct NativeMemoryIOKit::Resource {
    NativeMemoryIOKit *adapter {};
    DeviceIdentity device {};
    MemoryHandle handle {};
    uint64_t owner {}, bytes {};
    size_t pageCount {};
    IOBufferMemoryDescriptor *memory {};
    IODMACommand *command {};
    IOMapper *mapper {};
    uint64_t *pages {};
    bool descriptorAttempted {}, descriptorPrepared {}, commandAttempted {}, commandPrepared {};
    bool commandCompleteUncertain {}, descriptorCompleteUncertain {};
    bool directCoherent {}, mapAttempted {};
};

MemoryStatus NativeMemoryIOKit::attach(IOPCIDevice *device, IOService *owner,
        DeviceIdentity identity, MemoryLimits limits, PhysicalVmOps vm) {
    if (device_) return MemoryStatus::Busy;
    if (!device || !owner || !identity.vendor || identity.vendor == UINT16_MAX ||
        !identity.device || identity.device == UINT16_MAX || !identity.registryId || !identity.epoch ||
        limits.dmaAddressBits < 39 || limits.dmaAddressBits > 48 ||
        limits.gpuAddressBits < 39 || limits.gpuAddressBits > 48 ||
        !limits.maxAllocationBytes || limits.maxAllocationBytes > MemoryOwner::MaxAllocationBytes ||
        (limits.maxAllocationBytes & (MemoryOwner::PageBytes - 1)) ||
        limits.maxTotalBytes < limits.maxAllocationBytes ||
        limits.maxTotalBytes > MemoryOwner::MaxAllocationBytes * MemoryOwner::MaxAllocations)
        return MemoryStatus::Invalid;
    if (!vm.context || !vm.admitted || !vm.coherentSystem || !vm.map || !vm.unmap || !vm.retired || !vm.quiesce)
        return MemoryStatus::Unavailable;
    if (owner->getProvider() != device || !device->isOpen(owner) ||
        device->isInactive() || owner->isInactive()) return MemoryStatus::Ownership;
    // copyMapperForDevice also accepts ID properties and may wait for a matching
    // service. This bounded initial adapter accepts only the actual direct object.
    auto *property = device->copyProperty("iommu-parent");
    auto *mapper = OSDynamicCast(IOMapper, property);
    if (!mapper || mapper->isInactive()) {
        if (property) property->release();
        return MemoryStatus::Unavailable;
    }
    device_ = device; owner_ = owner; mapper_ = mapper;
    device_->retain(); owner_->retain(); // property already owns one mapper retain.
    identity_ = identity; limits_ = limits; vm_ = vm; stopped_ = false;
    if (!observe()) { detach(); return MemoryStatus::Ownership; }
    return MemoryStatus::Ok;
}
bool NativeMemoryIOKit::observe() const {
    if (!device_ || !owner_ || !mapper_ || stopped_ || owner_->getProvider() != device_ ||
        !device_->isOpen(owner_) || device_->isInactive() || owner_->isInactive() ||
        mapper_->isInactive()) return false;
    // The physical owner's power/reset lease authorizes even configuration
    // observation. Establish it before reading PCI state, under the shared lock.
    if (!vm_.admitted(vm_.context, device_, owner_, identity_)) return false;
    const uint32_t id = device_->configRead32(kIOPCIConfigVendorID);
    if (static_cast<uint16_t>(id) != identity_.vendor ||
        static_cast<uint16_t>(id >> 16) != identity_.device ||
        device_->getRegistryEntryID() != identity_.registryId) return false;
    auto *property = device_->copyProperty("iommu-parent");
    const bool mapperMatches = property == mapper_;
    if (property) property->release();
    return mapperMatches;
}
MemoryBackend NativeMemoryIOKit::backend() {
    return {this, admitted, pin, unpin, synchronize, map, unmap, retired, quiesce};
}
MemoryStatus NativeMemoryIOKit::detach() {
    for (auto *resource : resources_) if (resource) return MemoryStatus::Busy;
    if (pinnedBytes_) return MemoryStatus::Quarantined;
    if (mapper_) { mapper_->release(); mapper_ = nullptr; }
    if (owner_) { owner_->release(); owner_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    identity_ = {}; limits_ = {}; vm_ = {}; stopped_ = false;
    return MemoryStatus::Ok;
}
bool NativeMemoryIOKit::admitted(void *opaque, const DeviceIdentity &device) {
    if (!opaque) return false;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    return same(device, self.identity_) && self.observe();
}
void NativeMemoryIOKit::describe(Resource &r, DmaPin &out) {
    out = {&r, r.memory ? static_cast<uint8_t *>(r.memory->getBytesNoCopy()) : nullptr,
        r.pages, r.pageCount, r.bytes, r.device.registryId};
}
NativeMemoryIOKit::Resource *NativeMemoryIOKit::checked(const DeviceIdentity &device,
        uint64_t owner, const MemoryHandle &handle, const DmaPin &pin, bool complete) const {
    if (!device_ || !same(device, identity_) || !owner || handle.slot >= MemoryOwner::MaxAllocations ||
        handle.registryId != identity_.registryId || handle.epoch != identity_.epoch) return nullptr;
    auto *r = resources_[handle.slot];
    // Compare the opaque pointer to retained table metadata BEFORE dereferencing.
    if (!r || pin.cookie != r || r->adapter != this || r->owner != owner ||
        !same(r->device, device) || !same(r->handle, handle) ||
        pin.pages != r->pages || pin.pageCount != r->pageCount || pin.bytes != r->bytes ||
        pin.deviceRegistryId != r->device.registryId) return nullptr;
    if (complete && (r->commandCompleteUncertain || r->descriptorCompleteUncertain ||
        !r->descriptorPrepared || !r->commandPrepared || !r->directCoherent ||
        !r->memory || !r->command || !r->pages ||
        pin.cpu != r->memory->getBytesNoCopy())) return nullptr;
    return r;
}
MemoryStatus NativeMemoryIOKit::pin(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, uint64_t bytes, DmaPin &out) {
    out = {};
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    if (!owner || !bytes || (bytes & (MemoryOwner::PageBytes - 1)) || bytes > SIZE_MAX ||
        bytes > self.limits_.maxAllocationBytes || handle.slot >= MemoryOwner::MaxAllocations ||
        !handle.generation || handle.registryId != device.registryId || handle.epoch != device.epoch)
        return MemoryStatus::Invalid;
    if (!same(device, self.identity_) || !self.observe()) return MemoryStatus::Ownership;
    if (self.resources_[handle.slot]) return MemoryStatus::Busy;
    if (self.pinnedBytes_ > self.limits_.maxTotalBytes ||
        bytes > self.limits_.maxTotalBytes - self.pinnedBytes_) return MemoryStatus::Capacity;
    auto *r = static_cast<Resource *>(IOMalloc(sizeof(Resource)));
    if (!r) return MemoryStatus::Capacity;
    *r = {};
    r->adapter = &self; r->device = device; r->handle = handle; r->owner = owner; r->bytes = bytes;
    r->pageCount = static_cast<size_t>(bytes / MemoryOwner::PageBytes);
    r->mapper = self.mapper_; r->mapper->retain();
    self.resources_[handle.slot] = r; self.pinnedBytes_ += bytes;
    // Failures after acquisition ALWAYS return this retained cleanup resource.
    // The portable owner quarantines; unpin performs the authoritative inverse.
    r->pages = static_cast<uint64_t *>(IOMalloc(r->pageCount * sizeof(uint64_t)));
    if (!r->pages) { describe(*r, out); return MemoryStatus::IoFailure; }
    bzero(r->pages, r->pageCount * sizeof(uint64_t));
    r->memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
        static_cast<vm_size_t>(bytes), MemoryOwner::PageBytes);
    if (!r->memory || !r->memory->getBytesNoCopy()) { describe(*r, out); return MemoryStatus::IoFailure; }
    bzero(r->memory->getBytesNoCopy(), static_cast<size_t>(bytes));
    r->descriptorAttempted = true;
    if (r->memory->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    r->descriptorPrepared = true;
    r->command = IODMACommand::withSpecification(IODMACommand::OutputHost64,
        self.limits_.dmaAddressBits, MemoryOwner::PageBytes, IODMACommand::kMapped,
        bytes, MemoryOwner::PageBytes, r->mapper);
    if (!r->command || r->command->setMemoryDescriptor(r->memory, false) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    r->commandAttempted = true;
    if (r->command->prepare(0, bytes) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    r->commandPrepared = true;
    UInt64 offset = 0;
    const uint64_t limit = 1ULL << self.limits_.dmaAddressBits;
    for (size_t i = 0; i < r->pageCount; ++i) {
        IODMACommand::Segment64 segment {};
        UInt32 count = 1; const UInt64 before = offset;
        if (r->command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
            count != 1 || segment.fLength != MemoryOwner::PageBytes ||
            offset != before + MemoryOwner::PageBytes ||
            (segment.fIOVMAddr & (MemoryOwner::PageBytes - 1)) ||
            segment.fIOVMAddr > limit - MemoryOwner::PageBytes) {
            describe(*r, out); return MemoryStatus::IoFailure;
        }
        r->pages[i] = segment.fIOVMAddr;
        IOByteCount length = 0;
        const auto original = r->memory->getPhysicalSegment(before, &length, kIOMemoryMapperNone);
        // Full-buffer pointer equality alone misses page-level bounce copies.
        // Prove each actual DMA page resolves to its original descriptor page.
        if (length < MemoryOwner::PageBytes || (original & (MemoryOwner::PageBytes - 1)) ||
            r->mapper->mapToPhysicalAddress(segment.fIOVMAddr) != original ||
            r->command->getIOMemoryDescriptor() != r->memory) {
            describe(*r, out); return MemoryStatus::IoFailure;
        }
    }
    describe(*r, out);
    if (offset != bytes || !self.observe() ||
        !self.vm_.coherentSystem(self.vm_.context, device, out)) return MemoryStatus::IoFailure;
    r->directCoherent = true;
    return MemoryStatus::Ok;
}
MemoryStatus NativeMemoryIOKit::releaseResource(Resource &r) {
    if (r.mapAttempted) return MemoryStatus::Busy;
    // An inverse DMA error may have consumed the kernel's active counter while
    // leaving an IOMMU mapping unaccounted for. A later NotReady/clear success,
    // GPU reset or repeated complete cannot prove that mapping was reclaimed.
    // Keep the backing/mapper/charge until a future explicit recovery owner can
    // provide independent IOMMU/descriptor retirement authority.
    if (r.commandCompleteUncertain || r.descriptorCompleteUncertain)
        return MemoryStatus::Quarantined;
    if (r.command) {
        if (r.commandAttempted) {
            const auto result = r.command->complete(true, true);
            // NotReady is not a DMA-map retirement proof. This also applies
            // after failed prepare: its partial mapping outcome is unknown.
            if (result != kIOReturnSuccess) {
                r.commandCompleteUncertain = true;
                return MemoryStatus::IoFailure;
            }
            r.commandAttempted = false; r.commandPrepared = false;
        }
        if (r.command->getMemoryDescriptor()) {
            if (r.command->clearMemoryDescriptor(false) != kIOReturnSuccess ||
                r.command->getMemoryDescriptor()) return MemoryStatus::IoFailure;
        }
        r.command->release(); r.command = nullptr;
    }
    if (r.descriptorAttempted) {
        // Failure is preserved; do not infer that failed descriptor preparation
        // acquired no pin, or release its metadata while completion is unknown.
        if (!r.memory || r.memory->complete(kIODirectionInOut) != kIOReturnSuccess) {
            r.descriptorCompleteUncertain = true;
            return MemoryStatus::IoFailure;
        }
        r.descriptorAttempted = false; r.descriptorPrepared = false;
    }
    if (r.memory) { r.memory->release(); r.memory = nullptr; }
    if (r.mapper) { r.mapper->release(); r.mapper = nullptr; }
    if (r.pages) { IOFree(r.pages, r.pageCount * sizeof(uint64_t)); r.pages = nullptr; }
    pinnedBytes_ -= r.bytes; resources_[r.handle.slot] = nullptr;
    IOFree(&r, sizeof(Resource));
    return MemoryStatus::Ok;
}
MemoryStatus NativeMemoryIOKit::unpin(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, DmaPin &pin) {
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, false);
    if (!r) return MemoryStatus::Ownership;
    const auto result = self.releaseResource(*r);
    if (result == MemoryStatus::Ok) pin = {};
    else describe(*r, pin);
    return result;
}
MemoryStatus NativeMemoryIOKit::synchronize(void *opaque, const DeviceIdentity &device,
        uint64_t owner, const MemoryHandle &handle, const DmaPin &pin, CacheDirection direction) {
    if (!opaque) return MemoryStatus::Unavailable;
    if (direction != CacheDirection::ForDevice && direction != CacheDirection::ForCpu)
        return MemoryStatus::Invalid;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, true);
    if (!r) return MemoryStatus::Ownership;
    if (self.stopped_) {
        if (direction != CacheDirection::ForCpu) return MemoryStatus::Ownership;
    } else if (!self.observe()) return MemoryStatus::Ownership;
    // This adapter admitted direct coherent backing; synchronize cannot copy a
    // concurrently GPU-written bounce fence. Still neither engine cache flush
    // nor GPU completion. Queue transport supplies system ordering/acquire read.
    return r->command->synchronize(direction == CacheDirection::ForDevice ? kIODirectionOut : kIODirectionIn)
        == kIOReturnSuccess ? MemoryStatus::Ok : MemoryStatus::IoFailure;
}
MemoryStatus NativeMemoryIOKit::map(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, bool writable, GpuMapping &mapping) {
    mapping = {};
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, true);
    if (!r || !self.observe()) return MemoryStatus::Ownership;
    if (r->mapAttempted) return MemoryStatus::Busy;
    r->mapAttempted = true;
    const auto result = self.vm_.map(self.vm_.context, device, owner, handle, pin, writable, mapping);
    if (result == MemoryStatus::Unavailable && empty(mapping)) r->mapAttempted = false;
    return result;
}
MemoryStatus NativeMemoryIOKit::unmap(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, GpuMapping &mapping) {
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, true);
    if (!r) return MemoryStatus::Ownership;
    if (!r->mapAttempted) {
        // Exact retained generation proves the adapter never invoked VM map;
        // pin cannot publish GPU mappings. This is recorded no-publication,
        // not a fabricated hardware TLB acknowledgement. Portable uncertainty
        // still requires actual quiescence before reaching this inverse path.
        return empty(mapping) ? MemoryStatus::Ok : MemoryStatus::Ownership;
    }
    // Actual VM owner handles both published and unknown partial mappings.
    // Ok requires unpublication + completed TLB invalidation or reset authority.
    const auto result = self.vm_.unmap(self.vm_.context, device, owner, handle, pin, mapping);
    if (result == MemoryStatus::Ok) { r->mapAttempted = false; mapping = {}; }
    return result;
}
bool NativeMemoryIOKit::retired(void *opaque, const MemoryView &view) {
    if (!opaque) return false;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    return self.checked(view.device, view.owner, view.handle, view.pin, true) &&
        self.observe() && self.vm_.retired(self.vm_.context, view);
}
bool NativeMemoryIOKit::quiesce(void *opaque, const DeviceIdentity &device) {
    if (!opaque) return false;
    auto &self = *static_cast<NativeMemoryIOKit *>(opaque);
    if (!self.device_ || !same(device, self.identity_)) return false;
    if (self.stopped_) return true;
    if (!self.vm_.quiesce(self.vm_.context, device)) return false;
    self.stopped_ = true;
    return true;
}

} // namespace MellowNative
