// Local 7D41 research implementation, 2026. See LICENSE and NOTICE.
#include "XeMemory.hpp"

namespace XeMemory {
static bool powerOfTwo(uint64_t value) { return value && !(value & (value - 1)); }
static bool span(uint64_t address, uint64_t bytes, uint64_t limit) {
    return bytes && address < limit && bytes <= limit - address;
}
static bool dmaPage(uint64_t address) {
    return !(address & (PageSize - 1)) && span(address, PageSize, DmaLimit);
}
static bool alignUp(uint64_t value, uint64_t alignment, uint64_t &out) {
    if (!powerOfTwo(alignment) || value > UINT64_MAX - (alignment - 1)) return false;
    out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}
static uint64_t patBits(uint8_t pat) {
    // Intel xe_gtt_defs.h: PAT0=3, PAT1=4, leaf PAT2=7, Xe-LPG PAT3=62.
    return ((pat & 1U) ? (1ULL << 3) : 0) |
           ((pat & 2U) ? (1ULL << 4) : 0) |
           ((pat & 4U) ? (1ULL << 7) : 0) |
           ((pat & 8U) ? (1ULL << 62) : 0);
}
Status encodeSystemPte4K(uint64_t address, uint8_t pat, bool writable, uint64_t &entry) {
    if (!dmaPage(address) || pat > 15) return Status::Invalid;
    entry = address | 1ULL | (writable ? 2ULL : 0) | patBits(pat);
    return Status::Ok;
}
Status encodeSystemPde(uint64_t address, uint8_t pat, uint64_t &entry) {
    // Non-leaf tables expose only PAT0/PAT1 in Intel's xelp_pde_encode_bo.
    if (!dmaPage(address) || pat > 3) return Status::Invalid;
    entry = address | 3ULL | patBits(pat);
    return Status::Ok;
}
Status pageTableIndices(uint64_t address, uint16_t (&indices)[4]) {
    if (address >= VaLimit) return Status::Invalid;
    for (unsigned level = 0; level < 4; ++level)
        indices[level] = static_cast<uint16_t>((address >> (12 + 9 * level)) & 511U);
    return Status::Ok;
}
Status VirtualMemory::initialize(Allocation *slots, size_t capacity, uint64_t first,
                                  uint64_t limit, Backend backend) {
    if (slots_) return Status::Busy;
    if (!slots || !capacity || first >= limit || limit > VaLimit ||
        (first & (PageSize - 1)) || (limit & (PageSize - 1))) return Status::Invalid;
    slots_ = slots;
    capacity_ = capacity;
    first_ = first;
    limit_ = limit;
    backend_ = backend;
    for (size_t i = 0; i < capacity_; ++i) slots_[i] = Allocation {};
    return Status::Ok;
}
bool VirtualMemory::available(uint64_t address, uint64_t bytes) const {
    if (!slots_ || address < first_ || !span(address, bytes, limit_) ||
        (address & (PageSize - 1)) || (bytes & (PageSize - 1))) return false;
    for (size_t i = 0; i < capacity_; ++i) {
        const auto &a = slots_[i];
        if (a.state != State::Free && address < a.address + a.bytes && a.address < address + bytes)
            return false;
    }
    return true;
}
Status VirtualMemory::insert(uint64_t owner, uint64_t address, uint64_t bytes,
                             State state, Handle *out) {
    if (!available(address, bytes)) return Status::Invalid;
    if (!nextGeneration_ || nextGeneration_ == UINT64_MAX) return Status::NoSpace;
    for (size_t i = 0; i < capacity_; ++i) {
        if (slots_[i].state != State::Free) continue;
        Allocation allocation {};
        allocation.state = state;
        allocation.owner = owner;
        allocation.address = address;
        allocation.bytes = bytes;
        allocation.generation = nextGeneration_++;
        slots_[i] = allocation;
        if (out) *out = Handle {i, allocation.generation};
        return Status::Ok;
    }
    return Status::NoSpace;
}
Status VirtualMemory::exclude(uint64_t address, uint64_t bytes) {
    return insert(0, address, bytes, State::Excluded, nullptr);
}
Status VirtualMemory::reserveAt(uint64_t owner, uint64_t address, uint64_t bytes, Handle &out) {
    if (!owner) return Status::Invalid;
    return insert(owner, address, bytes, State::Reserved, &out);
}
Status VirtualMemory::reserve(uint64_t owner, uint64_t bytes, uint64_t alignment, Handle &out) {
    if (!slots_ || !owner || !bytes || (bytes & (PageSize - 1)) ||
        alignment < PageSize || !powerOfTwo(alignment)) return Status::Invalid;
    uint64_t candidate;
    if (!alignUp(first_, alignment, candidate)) return Status::NoSpace;
    for (;;) {
        if (!span(candidate, bytes, limit_)) return Status::NoSpace;
        uint64_t next = candidate;
        for (size_t i = 0; i < capacity_; ++i) {
            const auto &a = slots_[i];
            if (a.state != State::Free && candidate < a.address + a.bytes && a.address < candidate + bytes) {
                if (a.address + a.bytes > next) next = a.address + a.bytes;
            }
        }
        if (next == candidate) return insert(owner, candidate, bytes, State::Reserved, &out);
        if (!alignUp(next, alignment, candidate)) return Status::NoSpace;
    }
}
Status VirtualMemory::lookup(uint64_t owner, Handle handle, Allocation *&a) {
    if (!slots_ || handle.slot >= capacity_ || !handle.generation) return Status::NotFound;
    a = &slots_[handle.slot];
    if (a->state == State::Free || a->state == State::Excluded || a->generation != handle.generation)
        return Status::NotFound;
    if (!owner || a->owner != owner) return Status::WrongOwner;
    if (a->state == State::Quarantined) return Status::Quarantined;
    return Status::Ok;
}
const Allocation *VirtualMemory::inspect(uint64_t owner, Handle handle) const {
    if (!slots_ || handle.slot >= capacity_ || !handle.generation || !owner) return nullptr;
    const auto &a = slots_[handle.slot];
    return a.state != State::Free && a.state != State::Excluded &&
           a.generation == handle.generation && a.owner == owner ? &a : nullptr;
}
size_t VirtualMemory::occupied() const {
    size_t result = 0;
    for (size_t i = 0; i < capacity_; ++i) if (slots_[i].state != State::Free) ++result;
    return result;
}
Status VirtualMemory::pin(uint64_t owner, Handle handle) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state != State::Reserved) return Status::Busy;
    if (!backend_.pin || !backend_.unpin) return Status::Unavailable;
    Pin pinned {};
    const Status result = backend_.pin(backend_.context, owner, a->bytes, pinned);
    if (result != Status::Ok) {
        if (pinned.cookie || pinned.dmaPages || pinned.pageCount) {
            a->pin = pinned;
            a->state = State::Quarantined;
            return Status::Quarantined;
        }
        return result;
    }
    bool valid = pinned.cookie && pinned.dmaPages && pinned.pageCount == a->bytes / PageSize;
    if (valid) for (size_t i = 0; i < pinned.pageCount; ++i) valid &= dmaPage(pinned.dmaPages[i]);
    if (!valid) {
        if (backend_.unpin(backend_.context, pinned) != Status::Ok) {
            a->pin = pinned;
            a->state = State::Quarantined;
            return Status::Quarantined;
        }
        return Status::BackendFailure;
    }
    a->pin = pinned;
    a->state = State::Pinned;
    return Status::Ok;
}
Status VirtualMemory::bind(uint64_t owner, Handle handle, uint8_t pat, bool writable) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state != State::Pinned) return Status::Busy;
    if (pat > 15) return Status::Invalid;
    if (!backend_.bind || !backend_.unbind || !(backend_.verifiedPatIndices & (1U << pat)))
        return Status::Unavailable;
    const Status result = backend_.bind(backend_.context, a->address, a->pin, pat, writable);
    if (result == Status::Ok) a->state = State::Bound;
    else if (result != Status::Unavailable) {
        a->state = State::Quarantined;
        return Status::Quarantined;
    }
    return result;
}
Status VirtualMemory::retainUse(uint64_t owner, Handle handle) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state != State::Bound) return Status::Busy;
    if (a->activeUses == UINT32_MAX) return Status::NoSpace;
    ++a->activeUses;
    return Status::Ok;
}
Status VirtualMemory::releaseUse(uint64_t owner, Handle handle) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state != State::Bound && a->state != State::Retiring) return Status::Busy;
    if (!a->activeUses) return Status::Invalid;
    --a->activeUses;
    return Status::Ok;
}
Status VirtualMemory::recordUse(uint64_t owner, Handle handle, Fence fence) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state != State::Bound) return Status::Busy;
    if (!fence.timeline || !fence.value ||
        (a->lastUse.timeline && a->lastUse.timeline != fence.timeline) ||
        fence.value <= a->lastUse.value) return Status::Invalid;
    a->lastUse = fence;
    return Status::Ok;
}
Status VirtualMemory::retire(uint64_t owner, Handle handle) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state == State::Retiring) return Status::Ok;
    if (a->state != State::Bound) return Status::Busy;
    a->state = State::Retiring;
    return Status::Ok;
}
Status VirtualMemory::reclaim(uint64_t owner, Handle handle) {
    Allocation *a;
    const Status found = lookup(owner, handle, a);
    if (found != Status::Ok) return found;
    if (a->state == State::Bound) return Status::Busy;
    if (a->state == State::Retiring) {
        if (a->activeUses) return Status::Busy;
        if (a->lastUse.value) {
            if (!backend_.fenceComplete) return Status::Unavailable;
            if (!backend_.fenceComplete(backend_.context, a->lastUse)) return Status::Busy;
        }
        if (!backend_.unbind) return Status::Unavailable;
        const Status result = backend_.unbind(backend_.context, a->address, a->bytes);
        if (result == Status::Unavailable) return result;
        if (result != Status::Ok) {
            a->state = State::Quarantined;
            return Status::Quarantined;
        }
        a->state = State::Pinned;
    }
    if (a->state == State::Pinned) {
        if (!backend_.unpin || backend_.unpin(backend_.context, a->pin) != Status::Ok) {
            a->state = State::Quarantined;
            return Status::Quarantined;
        }
    }
    *a = Allocation {};
    return Status::Ok;
}
}
