// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
#include "XeGuCRegionOwner.hpp"
#ifdef KERNEL
#include "XeMemoryIOKit.hpp"
#endif

namespace XeGuCRegions {
namespace {
bool empty(const XeMemory::Pin &p) { return !p.cookie && !p.dmaPages && !p.pageCount; }
bool same(const XeGgtt::Backing &a, const XeGgtt::Backing &b) {
    return a.cookie == b.cookie && a.cpu == b.cpu && a.dmaPages == b.dmaPages &&
        a.pageCount == b.pageCount && a.identity == b.identity;
}
bool same(const XeGuCFirmware::Region &a, const XeGuCFirmware::Region &b) {
    return a.owner == b.owner && a.generation == b.generation && a.ggtt == b.ggtt &&
        a.bytes == b.bytes && a.cpu == b.cpu && a.dmaPages == b.dmaPages &&
        a.pageCount == b.pageCount && a.pinCookie == b.pinCookie;
}
bool overlaps(uintptr_t a, uint64_t n, uintptr_t b, uint64_t m) {
    // Callers first prove both exclusive-end sums representable.
    return a < b + m && b < a + n;
}
Status pinStatus(XeMemory::Status s) {
    switch (s) {
    case XeMemory::Status::Invalid: return Status::Invalid;
    case XeMemory::Status::NoSpace: return Status::NoSpace;
    case XeMemory::Status::Unavailable: return Status::Unavailable;
    case XeMemory::Status::Busy: return Status::Busy;
    default: return Status::Io;
    }
}
}
bool Owner::admitted() const {
    return started_ && !closed_ && hardware_.admitted(hardware_.opaque, owner_, epoch_);
}
bool Owner::quiet() const {
    return admitted() && hardware_.consumersQuiesced(hardware_.opaque, owner_, epoch_);
}
Status Owner::initialize(uint64_t owner, uint64_t epoch, const XeGgtt::Range *ranges,
                         size_t count, Pins pins, Hardware hardware) {
    if (started_) return Status::Busy;
    if (!owner || !epoch || !ranges || !count || count > XeGgtt::Manager::MaxRanges)
        return Status::Invalid;
    if (!pins.backend.context || !pins.backend.pin || !pins.backend.unpin ||
        !pins.opaque || !pins.cpu || !pins.synchronize || !hardware.opaque ||
        !hardware.admitted || !hardware.acquireSpace || !hardware.releaseSpace ||
        !hardware.resetAllowed || !hardware.consumersQuiesced || !hardware.readPat3 ||
        !hardware.readPte || !hardware.writePte || !hardware.invalidate) return Status::Unavailable;
    for (size_t i = 0; i < count; ++i) {
        if (ranges[i].address < 4ULL * 1024 * 1024 || !ranges[i].bytes ||
            (ranges[i].address & 4095) || (ranges[i].bytes & 4095) ||
            ranges[i].bytes > XeGuCFirmware::gucGgttTop ||
            ranges[i].address > XeGuCFirmware::gucGgttTop - ranges[i].bytes)
            return Status::Invalid;
    }
    owner_ = owner; epoch_ = epoch; pins_ = pins; hardware_ = hardware;
    // Manager callbacks use this object's canonical pin table. Mark the object
    // started for admission only; failed initialize acquires no space lease.
    started_ = true;
    const auto status = ggtt_.initialize(epoch_, ranges, count, ggttBackend());
    if (status != XeGgtt::Status::Ok) {
        started_ = false; owner_ = epoch_ = 0; pins_ = {}; hardware_ = {};
        return status == XeGgtt::Status::Invalid ? Status::Invalid : Status::Unavailable;
    }
    return Status::Ok;
}
Status Owner::lookup(Handle h, size_t &index) const {
    if (!started_ || closed_) return Status::Unavailable;
    if (h.owner != owner_) return Status::WrongOwner;
    if (h.epoch != epoch_) return Status::WrongEpoch;
    if (h.slot >= MaxRegions || !h.generation) return Status::NotFound;
    const auto &s = slots_[h.slot];
    if (s.view.state == State::Free || s.view.handle.generation != h.generation)
        return Status::NotFound;
    index = h.slot; return Status::Ok;
}
Owner::Slot *Owner::find(const XeGuCFirmware::Region &r) {
    if (!started_ || closed_ || r.owner != owner_ || !r.generation) return nullptr;
    // Compare pointer values to private metadata BEFORE any backend inspection.
    for (auto &s : slots_)
        if (s.view.state != State::Free && s.reserved && same(s.view.region, r)) return &s;
    return nullptr;
}
Owner::Slot *Owner::find(const XeGgtt::Backing &b) {
    for (auto &s : slots_)
        if (s.view.state != State::Free && s.pinOwned && same(s.backing, b)) return &s;
    return nullptr;
}
bool Owner::canonical(const Slot &s, uint8_t *&cpu) const {
    cpu = nullptr;
    if (!s.pinOwned || !s.pin.cookie || !s.pin.dmaPages || !s.bytes ||
        s.bytes > MaxBytes || s.bytes > SIZE_MAX || (s.bytes & 4095) ||
        s.pin.pageCount != s.bytes / XeGgtt::PageSize ||
        s.pin.pageCount > XeGgtt::Manager::MaxPages ||
        (reinterpret_cast<uintptr_t>(s.pin.dmaPages) & (alignof(uint64_t) - 1)) ||
        reinterpret_cast<uintptr_t>(s.pin.dmaPages) >
            UINTPTR_MAX - s.pin.pageCount * sizeof(uint64_t)) return false;
    if (!pins_.cpu(pins_.opaque, owner_, s.pin, s.bytes, cpu)) return false;
    const auto address = reinterpret_cast<uintptr_t>(cpu);
    if (!address || (address & 4095) || address > UINTPTR_MAX - s.bytes) return false;
    return !s.view.region.cpu || s.view.region.cpu == cpu;
}
bool Owner::alias(const Slot &candidate) const {
    const auto address = reinterpret_cast<uintptr_t>(candidate.view.region.cpu);
    const auto array = reinterpret_cast<uintptr_t>(candidate.pin.dmaPages);
    const uint64_t arrayBytes = candidate.pin.pageCount * sizeof(uint64_t);
    // Metadata must not occupy CPU bytes we may write, including its own array.
    if (overlaps(address, candidate.bytes, array, arrayBytes)) return true;
    for (const auto &s : slots_) {
        if (&s == &candidate || s.view.state == State::Free) continue;
        if (s.pin.cookie && s.pin.cookie == candidate.pin.cookie) return true;
        if (s.view.region.cpu && overlaps(address, candidate.bytes,
                reinterpret_cast<uintptr_t>(s.view.region.cpu), s.bytes)) return true;
        if (s.pin.dmaPages && s.pin.pageCount && s.pin.pageCount <= XeGgtt::Manager::MaxPages) {
            const auto other = reinterpret_cast<uintptr_t>(s.pin.dmaPages);
            const uint64_t bytes = s.pin.pageCount * sizeof(uint64_t);
            if (other > UINTPTR_MAX - bytes || overlaps(address, candidate.bytes, other, bytes) ||
                overlaps(array, arrayBytes, other, bytes)) return true;
        }
        if (s.view.region.cpu && overlaps(array, arrayBytes,
                reinterpret_cast<uintptr_t>(s.view.region.cpu), s.bytes)) return true;
    }
    return false;
}
Status Owner::quarantine(Slot &s) {
    s.view.state = State::Quarantined; draining_ = true; return Status::Quarantined;
}
bool Owner::fresh(Slot &s) {
    uint8_t *cpu = nullptr;
    if (s.reserved && admitted() && canonical(s, cpu) &&
        ggtt_.published(owner_, epoch_, s.view.ggtt, s.backing) == XeGgtt::Status::Ok) return true;
    quarantine(s); return false;
}
Status Owner::cleanup(Slot &s) {
    if (s.view.firmwareReferences) return Status::Busy;
    // Includes failed/partially published mappings. Caller cannot turn an empty
    // mapping token or elapsed time into permission to complete/unpin backing.
    if (!quiet()) return quarantine(s);
    if (s.reserved) {
        const auto status = ggtt_.retire(owner_, epoch_, s.view.ggtt);
        if (status != XeGgtt::Status::Ok) return quarantine(s);
        s.reserved = false;
    }
    if (s.view.ggttBackingHeld) return quarantine(s);
    // A DMA/descriptor completion error may consume the backend's active count
    // without reclaiming its IOMMU mapping. A later retry/NotReady/GPU reset is
    // not an independent inverse proof. This owner has no recovery authority.
    if (s.pinCleanupUncertain) return quarantine(s);
    if (s.pinOwned) {
        const auto status = pins_.backend.unpin(pins_.backend.context, s.pin);
        // Ok is not enough if the exact backend still reports retained state.
        if (status != XeMemory::Status::Ok || !empty(s.pin)) {
            s.pinCleanupUncertain = true; return quarantine(s);
        }
        s.pinOwned = false;
    } else if (!empty(s.pin)) return quarantine(s);
    charged_ -= s.bytes; s = {}; return Status::Ok;
}
Status Owner::allocate(uint64_t owner, uint64_t epoch, uint64_t bytes, Handle &out) {
    out = {};
    if (!started_ || closed_) return Status::Unavailable;
    if (owner != owner_) return Status::WrongOwner;
    if (epoch != epoch_) return Status::WrongEpoch;
    if (!bytes || (bytes & 4095) || bytes > MaxBytes || bytes > SIZE_MAX) return Status::Invalid;
    if (draining_ || !admitted()) return Status::Unavailable;
    if (!quiet()) return Status::Busy;
    if (!serial_ || charged_ > MaxTotalBytes || bytes > MaxTotalBytes - charged_)
        return Status::NoSpace;
    size_t index = 0;
    while (index < MaxRegions && slots_[index].view.state != State::Free) ++index;
    if (index == MaxRegions) return Status::NoSpace;
    auto &s = slots_[index]; s = {};
    s.bytes = bytes; s.view.handle = {index, owner_, epoch_, serial_++};
    s.view.state = State::Pinning; charged_ += bytes; out = s.view.handle;
    const auto pinned = pins_.backend.pin(pins_.backend.context, owner_, bytes, s.pin);
    s.pinOwned = !empty(s.pin);
    if (pinned != XeMemory::Status::Ok) {
        if (cleanup(s) != Status::Ok) return Status::Quarantined;
        out = {}; return pinStatus(pinned);
    }
    // A backend reporting success without a cleanup token violated its ABI.
    if (!s.pinOwned) { s.pinCleanupUncertain = true; return quarantine(s); }
    uint8_t *cpu = nullptr;
    if (!canonical(s, cpu)) {
        const auto cleanupStatus = cleanup(s);
        if (cleanupStatus == Status::Ok) { out = {}; return Status::Invalid; }
        return cleanupStatus;
    }
    s.view.region = {owner_, s.view.handle.generation, 0, bytes, cpu,
        s.pin.dmaPages, s.pin.pageCount, s.pin.cookie};
    bool pagesValid = !alias(s);
    for (size_t i = 0; pagesValid && i < s.pin.pageCount; ++i)
        pagesValid = !(s.pin.dmaPages[i] & 4095) &&
            s.pin.dmaPages[i] <= XeMemory::DmaLimit - XeGgtt::PageSize;
    if (!pagesValid) {
        const auto cleanupStatus = cleanup(s);
        if (cleanupStatus == Status::Ok) { out = {}; return Status::Invalid; }
        return cleanupStatus;
    }
    s.backing = {s.pin.cookie, cpu, s.pin.dmaPages, s.pin.pageCount, s.view.handle.generation};
    const auto reserved = ggtt_.reserve(owner_, epoch_, bytes, XeGgtt::PageSize, s.view.ggtt);
    if (reserved != XeGgtt::Status::Ok) {
        const auto cleanupStatus = cleanup(s);
        if (cleanupStatus == Status::Ok) { out = {}; return Status::NoSpace; }
        return cleanupStatus;
    }
    s.reserved = true; s.view.state = State::Reserved;
    XeGgtt::Mapping mapping {};
    if (ggtt_.inspect(owner_, epoch_, s.view.ggtt, mapping) != XeGgtt::Status::Ok)
        return quarantine(s);
    s.view.region.ggtt = mapping.range.address;
    const auto published = ggtt_.publish(owner_, epoch_, s.view.ggtt, s.backing, 3);
    if (published != XeGgtt::Status::Ok) {
        // Only these documented results can include Manager's successful inverse.
        // Mere NotFound/zero PTE/absence never authorizes freeing the Pin.
        if ((published == XeGgtt::Status::Io || published == XeGgtt::Status::Invalid) &&
            !s.view.ggttBackingHeld &&
            ggtt_.inspect(owner_, epoch_, s.view.ggtt, mapping) == XeGgtt::Status::NotFound) {
            s.reserved = false;
        }
        const auto cleanupStatus = cleanup(s);
        if (cleanupStatus == Status::Ok) { out = {}; return Status::Io; }
        return cleanupStatus;
    }
    if (!fresh(s)) return quarantine(s);
    s.view.state = State::Published; return Status::Ok;
}
Status Owner::write(Handle h, uint64_t offset, const uint8_t *source, size_t bytes) {
    size_t index = 0; const auto status = lookup(h, index);
    if (status != Status::Ok) return status;
    auto &s = slots_[index];
    if (!source || !bytes || offset > s.bytes || bytes > s.bytes - offset ||
        reinterpret_cast<uintptr_t>(source) > UINTPTR_MAX - bytes) return Status::Invalid;
    if (draining_ || s.view.state != State::Published) return Status::Unavailable;
    if (s.view.firmwareReferences || !quiet()) return Status::Busy;
    if (!fresh(s)) return quarantine(s);
    const auto destination = reinterpret_cast<uintptr_t>(s.view.region.cpu) + offset;
    if (overlaps(destination, bytes, reinterpret_cast<uintptr_t>(source), bytes)) return Status::Invalid;
    auto *out = s.view.region.cpu + static_cast<size_t>(offset);
    for (size_t i = 0; i < bytes; ++i) out[i] = source[i];
    // Visibility must be established again by Loader's retained synchronize.
    return Status::Ok;
}
Status Owner::region(Handle h, XeGuCFirmware::Region &out) {
    out = {}; size_t index = 0; const auto status = lookup(h, index);
    if (status != Status::Ok) return status;
    auto &s = slots_[index];
    if (draining_ || s.view.state != State::Published) return Status::Unavailable;
    if (!fresh(s)) return quarantine(s);
    out = s.view.region; return Status::Ok;
}
Status Owner::inspect(Handle h, View &out) const {
    out = {}; size_t index = 0; const auto status = lookup(h, index);
    if (status == Status::Ok) out = slots_[index].view;
    return status;
}
Status Owner::retire(Handle h) {
    size_t index = 0; const auto status = lookup(h, index);
    return status == Status::Ok ? cleanup(slots_[index]) : status;
}
size_t Owner::allocations() const {
    size_t count = 0; for (const auto &s : slots_) count += s.view.state != State::Free; return count;
}
Status Owner::close() {
    if (!started_) return Status::Unavailable;
    if (closed_) return Status::Ok;
    draining_ = true;
    // Reverse acquisition order, independent of reused slot indices.
    for (size_t count = 0; count < MaxRegions; ++count) {
        Slot *latest = nullptr;
        for (auto &s : slots_) if (s.view.state != State::Free &&
            (!latest || s.view.handle.generation > latest->view.handle.generation)) latest = &s;
        if (!latest) break;
        const auto status = cleanup(*latest);
        if (status != Status::Ok) return status;
    }
    const auto status = ggtt_.close();
    if (status != XeGgtt::Status::Ok) return Status::Quarantined;
    closed_ = true; return Status::Ok;
}
XeGgtt::Backend Owner::ggttBackend() {
    return {this, ggttAdmitted, acquireSpace, releaseSpace, retainBacking, ownsBacking,
        releaseBacking, patReady, readPte, writePte, invalidate, retired};
}
bool Owner::ggttAdmitted(void *p, uint64_t epoch) {
    auto &s = *static_cast<Owner *>(p); return epoch == s.epoch_ && s.admitted();
}
bool Owner::acquireSpace(void *p, uint64_t epoch, const XeGgtt::Range *ranges, size_t count) {
    auto &s = *static_cast<Owner *>(p);
    return epoch == s.epoch_ && s.admitted() &&
        s.hardware_.acquireSpace(s.hardware_.opaque, s.owner_, epoch, ranges, count);
}
bool Owner::releaseSpace(void *p, uint64_t epoch) {
    auto &s = *static_cast<Owner *>(p);
    return epoch == s.epoch_ && s.quiet() &&
        s.hardware_.releaseSpace(s.hardware_.opaque, s.owner_, epoch);
}
bool Owner::retainBacking(void *p, uint64_t owner, uint64_t epoch, const XeGgtt::Backing &b) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(b); uint8_t *cpu = nullptr;
    if (owner != s.owner_ || epoch != s.epoch_ || !record || record->view.ggttBackingHeld ||
        !s.admitted() || !s.canonical(*record, cpu)) return false;
    record->view.ggttBackingHeld = true; return true;
}
bool Owner::ownsBacking(void *p, uint64_t owner, uint64_t epoch, const XeGgtt::Backing &b) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(b); uint8_t *cpu = nullptr;
    return owner == s.owner_ && epoch == s.epoch_ && record && record->view.ggttBackingHeld &&
        s.admitted() && s.canonical(*record, cpu);
}
bool Owner::releaseBacking(void *p, uint64_t owner, uint64_t epoch, const XeGgtt::Backing &b) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(b);
    if (owner != s.owner_ || epoch != s.epoch_ || !record || !record->view.ggttBackingHeld ||
        record->view.firmwareReferences || !s.quiet()) return false;
    record->view.ggttBackingHeld = false; return true;
}
bool Owner::patReady(void *p, uint64_t epoch, uint8_t pat) {
    auto &s = *static_cast<Owner *>(p); uint32_t value = 0;
    return epoch == s.epoch_ && pat == 3 && s.admitted() &&
        s.hardware_.readPat3(s.hardware_.opaque, s.owner_, epoch, value) && value == 2;
}
bool Owner::readPte(void *p, uint64_t epoch, uint64_t va, uint64_t &value) {
    auto &s = *static_cast<Owner *>(p);
    return epoch == s.epoch_ && s.admitted() &&
        s.hardware_.readPte(s.hardware_.opaque, s.owner_, epoch, va, value);
}
bool Owner::writePte(void *p, uint64_t epoch, uint64_t va, uint64_t value) {
    auto &s = *static_cast<Owner *>(p);
    return epoch == s.epoch_ && s.admitted() &&
        s.hardware_.writePte(s.hardware_.opaque, s.owner_, epoch, va, value);
}
bool Owner::invalidate(void *p, uint64_t epoch) {
    auto &s = *static_cast<Owner *>(p);
    return epoch == s.epoch_ && s.admitted() &&
        s.hardware_.invalidate(s.hardware_.opaque, s.owner_, epoch);
}
bool Owner::retired(void *p, uint64_t owner, uint64_t epoch, XeGgtt::Handle) {
    auto &s = *static_cast<Owner *>(p);
    return owner == s.owner_ && epoch == s.epoch_ && s.quiet();
}
bool Owner::loaderAdmitted(void *p, uint64_t owner, uint64_t epoch) {
    auto &s = *static_cast<Owner *>(p);
    // Draining blocks new allocations/writers, not Loader's checked reset path.
    return owner == s.owner_ && epoch == s.epoch_ && s.admitted();
}
bool Owner::resetAllowed(void *p, uint64_t owner, uint64_t epoch) {
    auto &s = *static_cast<Owner *>(p);
    return loaderAdmitted(p, owner, epoch) &&
        s.hardware_.resetAllowed(s.hardware_.opaque, owner, epoch);
}
bool Owner::retainRegion(void *p, const XeGuCFirmware::Region &r, bool) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(r);
    if (!record || s.draining_ || record->view.state != State::Published ||
        record->view.firmwareReferences || !s.fresh(*record)) return false;
    // Loader's retain ABI has no borrower identity. A second borrower could
    // overwrite ADS/log through its writable CPU pointer. Every region hold is
    // exclusive, including read-only firmware, until the exact hold is released.
    if (s.ggtt_.retain(s.owner_, s.epoch_, record->view.ggtt, record->backing) != XeGgtt::Status::Ok)
        return false;
    ++record->view.firmwareReferences; return true;
}
bool Owner::releaseRegion(void *p, const XeGuCFirmware::Region &r) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(r);
    if (!record || !record->view.firmwareReferences || !s.quiet()) return false;
    if (s.ggtt_.release(s.owner_, s.epoch_, record->view.ggtt) != XeGgtt::Status::Ok) return false;
    --record->view.firmwareReferences; return true;
}
bool Owner::synchronizeRegion(void *p, const XeGuCFirmware::Region &r) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(r);
    if (!record || !record->view.firmwareReferences || !s.fresh(*record)) return false;
    if (s.pins_.synchronize(s.pins_.opaque, record->pin, true) != XeMemory::Status::Ok) {
        s.quarantine(*record); return false;
    }
    __sync_synchronize();
    // Synchronization may block or expose a changed DMA association. Re-resolve
    // the exact pin and published PTEs before admitting firmware consumption.
    return s.fresh(*record);
}
bool Owner::readPat3(void *p, uint32_t &value) {
    auto &s = *static_cast<Owner *>(p);
    return s.admitted() && s.hardware_.readPat3(s.hardware_.opaque, s.owner_, s.epoch_, value);
}
bool Owner::publishedRegion(void *p, const XeGuCFirmware::Region &r, uint64_t epoch) {
    auto &s = *static_cast<Owner *>(p); auto *record = s.find(r);
    return epoch == s.epoch_ && record && record->view.firmwareReferences && s.fresh(*record);
}
XeGuCFirmware::Backend Owner::firmwareBackend(MellowXe::MmioAccess io, uint8_t revision) {
    XeGuCFirmware::Backend b {}; b.io = io; b.physicalRevision = revision; b.opaque = this;
    b.admitted = loaderAdmitted; b.quiesced = resetAllowed; b.retain = retainRegion;
    b.release = releaseRegion; b.synchronize = synchronizeRegion; b.readPat3 = readPat3;
    b.mappingPublished = publishedRegion; return b;
}
#ifdef KERNEL
namespace {
bool iokitCpu(void *opaque, uint64_t owner, const XeMemory::Pin &pin, uint64_t bytes, uint8_t *&out) {
    out = opaque ? static_cast<uint8_t *>(XeMemory::resolveDirectPinnedBuffer(
        *static_cast<XeMemory::IOKitContext *>(opaque), owner, bytes, pin)) : nullptr;
    return out != nullptr;
}
XeMemory::Status iokitSynchronize(void *, const XeMemory::Pin &pin, bool device) {
    return device ? XeMemory::synchronizeForDevice(pin) : XeMemory::synchronizeForCpu(pin);
}
}
Pins makeIOKitPins(XeMemory::IOKitContext &context) {
    return {XeMemory::makeIOKitPinBackend(context), &context, iokitCpu, iokitSynchronize};
}
#endif
} // namespace XeGuCRegions
