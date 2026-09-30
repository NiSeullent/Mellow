// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "MemoryOwner.hpp"

namespace MellowNative {
namespace {
bool empty(const DmaPin &pin) {
    return !pin.cookie && !pin.cpu && !pin.pages && !pin.pageCount &&
        !pin.bytes && !pin.deviceRegistryId;
}
bool empty(const GpuMapping &mapping) {
    return !mapping.cookie && !mapping.address && !mapping.bytes && !mapping.writable;
}
bool cleanPinRejection(MemoryStatus status) {
    return status == MemoryStatus::Unavailable || status == MemoryStatus::Invalid ||
        status == MemoryStatus::Ownership || status == MemoryStatus::StaleEpoch ||
        status == MemoryStatus::Capacity || status == MemoryStatus::Busy;
}
bool overlap(uint64_t first, uint64_t bytes, uint64_t other, uint64_t otherBytes) {
    // Callers establish nonoverflowing, nonempty ranges first.
    return first < other + otherBytes && other < first + bytes;
}
}

MemoryStatus MemoryOwner::initialize(DeviceIdentity device, MemoryLimits limits,
                                     MemoryBackend backend) {
    if (initialized_) return MemoryStatus::Busy;
    if (!device.vendor || device.vendor == UINT16_MAX || !device.device ||
        device.device == UINT16_MAX || !device.registryId || !device.epoch ||
        limits.dmaAddressBits < 39 || limits.dmaAddressBits > 48 ||
        limits.gpuAddressBits < 39 || limits.gpuAddressBits > 48 ||
        !limits.maxAllocationBytes || limits.maxAllocationBytes > MaxAllocationBytes ||
        (limits.maxAllocationBytes & (PageBytes - 1)) ||
        limits.maxTotalBytes < limits.maxAllocationBytes ||
        limits.maxTotalBytes > MaxAllocationBytes * MaxAllocations)
        return MemoryStatus::Invalid;
    if (!backend.admitted || !backend.pin || !backend.unpin || !backend.synchronize ||
        !backend.map || !backend.unmap || !backend.retired || !backend.quiesce)
        return MemoryStatus::Unavailable;
    if (!backend.admitted(backend.context, device)) return MemoryStatus::Ownership;
    device_ = device; limits_ = limits; backend_ = backend; initialized_ = true;
    return MemoryStatus::Ok;
}
bool MemoryOwner::live() const {
    return initialized_ && !closed_ && !draining_ &&
        backend_.admitted(backend_.context, device_);
}
MemoryStatus MemoryOwner::lookup(uint64_t owner, MemoryHandle handle, uint32_t &slot) const {
    if (!initialized_ || !owner || handle.slot >= MaxAllocations || !handle.generation)
        return MemoryStatus::Invalid;
    if (handle.registryId != device_.registryId) return MemoryStatus::Ownership;
    if (handle.epoch != device_.epoch) return MemoryStatus::StaleEpoch;
    const auto &view = slots_[handle.slot].view;
    if (view.state == MemoryState::Empty || view.owner != owner ||
        view.handle.generation != handle.generation) return MemoryStatus::Ownership;
    slot = handle.slot;
    return MemoryStatus::Ok;
}
bool MemoryOwner::validPin(const Slot &slot) const {
    const auto &pin = slot.view.pin;
    const uint64_t bytes = slot.view.bytes;
    if (!slot.pinOwned || !pin.cookie || !pin.cpu || !pin.pages ||
        pin.bytes != bytes || pin.deviceRegistryId != device_.registryId ||
        pin.pageCount != bytes / PageBytes || !pin.pageCount ||
        pin.pageCount > MaxAllocationBytes / PageBytes || bytes > SIZE_MAX)
        return false;
    const auto cpu = reinterpret_cast<uintptr_t>(pin.cpu);
    if ((cpu & (PageBytes - 1)) || cpu > UINTPTR_MAX - static_cast<uintptr_t>(bytes) ||
        (reinterpret_cast<uintptr_t>(pin.pages) & (alignof(uint64_t) - 1))) return false;
    const uint64_t dmaLimit = 1ULL << limits_.dmaAddressBits;
    for (size_t i = 0; i < pin.pageCount; ++i) {
        if ((pin.pages[i] & (PageBytes - 1)) || pin.pages[i] > dmaLimit - PageBytes)
            return false;
    }
    // The physical adapter additionally owns exclusive underlying DMA pages;
    // independent mapper aliases cannot be proven from numerical IOVM addresses.
    for (const auto &other : slots_) {
        if (&other == &slot || other.view.state == MemoryState::Empty ||
            !other.pinOwned || !other.view.pin.cpu) continue;
        const auto otherCpu = reinterpret_cast<uintptr_t>(other.view.pin.cpu);
        if (pin.cookie == other.view.pin.cookie ||
            (otherCpu <= UINTPTR_MAX - other.view.bytes &&
             overlap(cpu, bytes, otherCpu, other.view.bytes))) return false;
    }
    return true;
}
bool MemoryOwner::validMapping(const Slot &slot) const {
    const auto &mapping = slot.view.mapping;
    const uint64_t limit = 1ULL << limits_.gpuAddressBits;
    if (!slot.mapAttempted || !mapping.cookie || mapping.address < PageBytes ||
        (mapping.address & (PageBytes - 1)) || mapping.bytes != slot.view.bytes ||
        mapping.address >= limit || mapping.bytes > limit - mapping.address) return false;
    for (const auto &other : slots_) {
        if (&other == &slot || !other.mapAttempted || !other.view.mapping.address ||
            !other.view.mapping.bytes || other.view.mapping.address >= limit ||
            other.view.mapping.bytes > limit - other.view.mapping.address) continue;
        if (mapping.cookie == other.view.mapping.cookie ||
            overlap(mapping.address, mapping.bytes, other.view.mapping.address,
                    other.view.mapping.bytes)) return false;
    }
    return true;
}
MemoryStatus MemoryOwner::quarantine(Slot &slot) {
    slot.view.state = MemoryState::Quarantined;
    draining_ = true;
    return MemoryStatus::Quarantined;
}
MemoryStatus MemoryOwner::allocate(uint64_t owner, uint64_t bytes, MemoryHandle &out) {
    out = {};
    if (!initialized_) return MemoryStatus::Unavailable;
    if (!owner || !bytes || (bytes & (PageBytes - 1)) || bytes > limits_.maxAllocationBytes)
        return MemoryStatus::Invalid;
    if (!live()) return draining_ ? MemoryStatus::Quarantined : MemoryStatus::Ownership;
    if (generation_ == UINT64_MAX || charged_ > limits_.maxTotalBytes ||
        bytes > limits_.maxTotalBytes - charged_) return MemoryStatus::Capacity;
    uint32_t index = MaxAllocations;
    for (uint32_t i = 0; i < MaxAllocations; ++i) {
        if (slots_[i].view.state == MemoryState::Empty) { index = i; break; }
    }
    if (index == MaxAllocations) return MemoryStatus::Capacity;
    auto &slot = slots_[index];
    slot = {};
    slot.view.device = device_;
    slot.view.handle = {index, generation_++, device_.registryId, device_.epoch};
    slot.view.owner = owner; slot.view.bytes = bytes; slot.view.state = MemoryState::Pinned;
    slot.pinOwned = true; charged_ += bytes;
    const MemoryStatus status = backend_.pin(backend_.context, device_, owner,
        slot.view.handle, bytes, slot.view.pin);
    if (status == MemoryStatus::Ok && validPin(slot)) {
        out = slot.view.handle;
        return MemoryStatus::Ok;
    }
    if (cleanPinRejection(status) && empty(slot.view.pin)) {
        // Explicit clean rejection: the callback acquired no resources.
        charged_ -= bytes; slot = {};
        return status;
    }
    // Even a failure without a cleanup token can have acquired DMA ownership.
    // Publish the quarantine handle so the physical adapter can retry cleanup.
    out = slot.view.handle;
    return quarantine(slot);
}
MemoryStatus MemoryOwner::map(uint64_t owner, MemoryHandle handle, bool writable) {
    uint32_t index = 0;
    const auto lookupStatus = lookup(owner, handle, index);
    if (lookupStatus != MemoryStatus::Ok) return lookupStatus;
    auto &slot = slots_[index];
    if (!live()) return draining_ ? MemoryStatus::Quarantined : MemoryStatus::Ownership;
    if (slot.view.state != MemoryState::Pinned || slot.mapAttempted || slot.view.jobHolds)
        return MemoryStatus::Busy;
    if (!validPin(slot)) return quarantine(slot);
    const auto syncStatus = backend_.synchronize(backend_.context, device_, owner,
        handle, slot.view.pin, CacheDirection::ForDevice);
    if (syncStatus != MemoryStatus::Ok) return quarantine(slot);
    slot.cpuSynchronized = false;
    slot.mapAttempted = true;
    const auto mapStatus = backend_.map(backend_.context, device_, owner, handle,
        slot.view.pin, writable, slot.view.mapping);
    if (mapStatus == MemoryStatus::Unavailable && empty(slot.view.mapping)) {
        slot.mapAttempted = false;
        return mapStatus;
    }
    if (mapStatus != MemoryStatus::Ok || slot.view.mapping.writable != writable ||
        !validPin(slot) || !validMapping(slot)) {
        slot.mappingUncertain = true;
        return quarantine(slot);
    }
    slot.view.state = MemoryState::Mapped;
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::inspect(uint64_t owner, MemoryHandle handle, MemoryView &out) const {
    out = {};
    uint32_t index = 0;
    const auto status = lookup(owner, handle, index);
    if (status == MemoryStatus::Ok) out = slots_[index].view;
    return status;
}
MemoryStatus MemoryOwner::retain(uint64_t owner, MemoryHandle handle) {
    uint32_t index = 0;
    const auto status = lookup(owner, handle, index);
    if (status != MemoryStatus::Ok) return status;
    auto &slot = slots_[index];
    if (!live()) return draining_ ? MemoryStatus::Quarantined : MemoryStatus::Ownership;
    if (slot.view.state != MemoryState::Mapped) return MemoryStatus::Busy;
    if (!validPin(slot) || !validMapping(slot)) {
        slot.mappingUncertain = true;
        return quarantine(slot);
    }
    if (slot.view.jobHolds == UINT32_MAX) return MemoryStatus::Capacity;
    ++slot.view.jobHolds; slot.cpuSynchronized = false;
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::release(uint64_t owner, MemoryHandle handle) {
    uint32_t index = 0;
    const auto status = lookup(owner, handle, index);
    if (status != MemoryStatus::Ok) return status;
    auto &slot = slots_[index];
    if (!slot.view.jobHolds) return MemoryStatus::Ownership;
    --slot.view.jobHolds;
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::sync(uint64_t owner, MemoryHandle handle, CacheDirection direction) {
    if (direction != CacheDirection::ForDevice && direction != CacheDirection::ForCpu)
        return MemoryStatus::Invalid;
    uint32_t index = 0;
    const auto status = lookup(owner, handle, index);
    if (status != MemoryStatus::Ok) return status;
    auto &slot = slots_[index];
    if (!live()) return draining_ ? MemoryStatus::Quarantined : MemoryStatus::Ownership;
    if (slot.view.jobHolds || (slot.view.state != MemoryState::Pinned &&
        slot.view.state != MemoryState::Mapped)) return MemoryStatus::Busy;
    if (!validPin(slot)) return quarantine(slot);
    if (slot.mapAttempted && !backend_.retired(backend_.context, slot.view))
        return MemoryStatus::Busy;
    const auto result = backend_.synchronize(backend_.context, device_, owner, handle,
        slot.view.pin, direction);
    if (result != MemoryStatus::Ok) return quarantine(slot);
    slot.cpuSynchronized = direction == CacheDirection::ForCpu;
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::synchronizeRetained(uint64_t owner, MemoryHandle handle,
                                             CacheDirection direction) {
    if (direction != CacheDirection::ForDevice && direction != CacheDirection::ForCpu)
        return MemoryStatus::Invalid;
    uint32_t index = 0;
    const auto status = lookup(owner, handle, index);
    if (status != MemoryStatus::Ok) return status;
    auto &slot = slots_[index];
    if (!live()) return draining_ ? MemoryStatus::Quarantined : MemoryStatus::Ownership;
    if (!slot.view.jobHolds || slot.view.state != MemoryState::Mapped)
        return MemoryStatus::Busy;
    if (!validPin(slot) || !validMapping(slot)) {
        slot.mappingUncertain = true;
        return quarantine(slot);
    }
    // GPU may still write after this operation. Never reuse its visibility
    // result as the final inverse synchronization during resource retirement.
    slot.cpuSynchronized = false;
    const auto result = backend_.synchronize(backend_.context, device_, owner, handle,
        slot.view.pin, direction);
    if (result != MemoryStatus::Ok) return quarantine(slot);
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::cleanup(Slot &slot) {
    if (slot.view.jobHolds) return MemoryStatus::Busy;
    if (slot.mapAttempted) {
        if (slot.mappingUncertain && !quiesced_) return quarantine(slot);
        if (!quiesced_ && !backend_.retired(backend_.context, slot.view))
            return MemoryStatus::Busy;
        if (!validPin(slot)) return quarantine(slot);
        if (!slot.cpuSynchronized) {
            const auto syncStatus = backend_.synchronize(backend_.context, device_,
                slot.view.owner, slot.view.handle, slot.view.pin, CacheDirection::ForCpu);
            if (syncStatus != MemoryStatus::Ok) return quarantine(slot);
            slot.cpuSynchronized = true;
        }
        const auto unmapStatus = backend_.unmap(backend_.context, device_, slot.view.owner,
            slot.view.handle, slot.view.pin, slot.view.mapping);
        if (unmapStatus != MemoryStatus::Ok) {
            slot.mappingUncertain = true;
            return quarantine(slot);
        }
        slot.view.mapping = {}; slot.mapAttempted = false; slot.mappingUncertain = false;
    }
    if (slot.pinOwned) {
        const auto unpinStatus = backend_.unpin(backend_.context, device_, slot.view.owner,
            slot.view.handle, slot.view.pin);
        if (unpinStatus != MemoryStatus::Ok) return quarantine(slot);
        slot.pinOwned = false;
    }
    charged_ -= slot.view.bytes; slot = {};
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::retire(uint64_t owner, MemoryHandle handle) {
    uint32_t index = 0;
    const auto status = lookup(owner, handle, index);
    if (status != MemoryStatus::Ok) return status;
    auto &slot = slots_[index];
    if (slot.view.jobHolds) return MemoryStatus::Busy;
    if (slot.view.state != MemoryState::Quarantined) slot.view.state = MemoryState::Retiring;
    return cleanup(slot);
}
MemoryStatus MemoryOwner::quiesce() {
    if (!initialized_) return MemoryStatus::Unavailable;
    if (closed_ || quiesced_) return MemoryStatus::Ok;
    draining_ = true;
    const bool stopped = backend_.quiesce(backend_.context, device_);
    for (auto &slot : slots_) {
        if (slot.view.state == MemoryState::Empty) continue;
        // Earlier CPU synchronization may precede late DMA during failed stop.
        // Always synchronize again after the actual quiescence boundary.
        slot.cpuSynchronized = false;
        if (!stopped) {
            slot.view.state = MemoryState::Quarantined;
            if (slot.mapAttempted) slot.mappingUncertain = true;
        }
    }
    if (!stopped) return MemoryStatus::Quarantined;
    quiesced_ = true;
    return MemoryStatus::Ok;
}
MemoryStatus MemoryOwner::close() {
    const auto status = quiesce();
    if (status != MemoryStatus::Ok) return status;
    if (closed_) return MemoryStatus::Ok;
    // Reverse acquisition order even when freed slots were reused.
    for (uint32_t count = 0; count < MaxAllocations; ++count) {
        Slot *latest = nullptr;
        for (auto &slot : slots_) {
            if (slot.view.state != MemoryState::Empty &&
                (!latest || slot.view.handle.generation > latest->view.handle.generation))
                latest = &slot;
        }
        if (!latest) { closed_ = true; return MemoryStatus::Ok; }
        const auto result = cleanup(*latest);
        if (result != MemoryStatus::Ok) return result;
    }
    closed_ = true;
    return MemoryStatus::Ok;
}
uint32_t MemoryOwner::allocations() const {
    uint32_t count = 0;
    for (const auto &slot : slots_) if (slot.view.state != MemoryState::Empty) ++count;
    return count;
}

} // namespace MellowNative
