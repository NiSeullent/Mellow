// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
#include "XeExecutionIOKit.hpp"

namespace XeContext {
namespace {
bool same(const LiveContext &a, const LiveContext &b) {
    return a.owner == b.owner && a.epoch == b.epoch && a.allocation == b.allocation &&
        a.id == b.id && a.ggttContext == b.ggttContext && a.ggttRing == b.ggttRing &&
        a.ringBytes == b.ringBytes && a.ringCpu == b.ringCpu && a.lrcTailCpu == b.lrcTailCpu &&
        a.descriptor == b.descriptor && a.depthStallWorkaround == b.depthStallWorkaround;
}
bool span(uintptr_t p, uint64_t n) { return p && n && n <= UINTPTR_MAX - p; }
bool overlap(uintptr_t a, uint64_t n, uintptr_t b, uint64_t m) {
    return a < b + m && b < a + n; // Both exclusive-end sums already checked.
}
void swap(uint64_t &a, uint64_t &b) { const auto t = a; a = b; b = t; }
void sift(uint64_t *p, size_t root, size_t count) {
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && p[child] < p[child + 1]) ++child;
        if (p[root] >= p[child]) break;
        swap(p[root], p[child]); root = child;
    }
}
void sort(uint64_t *p, size_t count) {
    for (size_t i = count / 2; i; --i) sift(p, i - 1, count);
    for (size_t i = count; i > 1; --i) { swap(p[0], p[i - 1]); sift(p, 0, i - 1); }
}
}
bool IOKitExecutionStaging::configured() const {
    return physical_.opaque && physical_.admitted && physical_.freshStopped &&
        physical_.retainContext && physical_.releaseContext && physical_.synchronizeContext &&
        physical_.quiesced;
}
ExecutionBackend IOKitExecutionStaging::backend() {
    if (!configured()) return {};
    return {this, admitted, freshStopped, retainContext, releaseContext, stageHeaps,
        synchronizeContext, quiesced};
}
bool IOKitExecutionStaging::exact(const LiveContext &c) const { return held_ && same(c, context_); }
bool IOKitExecutionStaging::allowed(const LiveContext &c) const {
    // policy_ is only the original argument, not cached admission authority.
    return configured() && exact(c) && policyObserved_ &&
        physical_.admitted(physical_.opaque, c, policy_);
}
bool IOKitExecutionStaging::admitted(void *p, const LiveContext &c, const XeDispatch::Policy &policy) {
    if (!p) return false;
    auto &s = *static_cast<IOKitExecutionStaging *>(p);
    if (!s.configured() || (s.held_ && !s.exact(c)) ||
        !s.physical_.admitted(s.physical_.opaque, c, policy)) return false;
    s.policy_ = policy; s.policyObserved_ = true; return true;
}
bool IOKitExecutionStaging::freshStopped(void *p, const LiveContext &c) {
    if (!p) return false;
    auto &s = *static_cast<IOKitExecutionStaging *>(p);
    return s.configured() && (!s.held_ || s.exact(c)) && s.physical_.freshStopped(s.physical_.opaque, c);
}
bool IOKitExecutionStaging::retainContext(void *p, const LiveContext &c) {
    if (!p) return false;
    auto &s = *static_cast<IOKitExecutionStaging *>(p);
    if (s.held_ || !s.configured() || !s.policyObserved_ ||
        !s.physical_.admitted(s.physical_.opaque, c, s.policy_) ||
        !s.physical_.freshStopped(s.physical_.opaque, c) ||
        !s.physical_.retainContext(s.physical_.opaque, c)) return false;
    s.context_ = c; s.held_ = true; s.report_ = {}; return true;
}
bool IOKitExecutionStaging::releaseContext(void *p, const LiveContext &c) {
    if (!p) return false;
    auto &s = *static_cast<IOKitExecutionStaging *>(p);
    // The actual physical owner decides whether an inverse may run even after
    // admission was lost. A failed release preserves the exact retained hold.
    if (!s.exact(c) || !s.physical_.releaseContext(s.physical_.opaque, c)) return false;
    s.held_ = false; s.context_ = {}; s.policyObserved_ = false; return true;
}
bool IOKitExecutionStaging::stageHeaps(void *p, const LiveContext &c,
        const XeMemory::Handle (&h)[6], const XeDispatch::Prepared &prepared) {
    return p && static_cast<IOKitExecutionStaging *>(p)->stage(c, h, prepared);
}
bool IOKitExecutionStaging::synchronizeContext(void *p, const LiveContext &c) {
    if (!p) return false;
    auto &s = *static_cast<IOKitExecutionStaging *>(p);
    return s.allowed(c) && s.report_.status == IOKitStagingStatus::Staged &&
        s.physical_.synchronizeContext(s.physical_.opaque, c);
}
bool IOKitExecutionStaging::quiesced(void *p, const LiveContext &c) {
    if (!p) return false;
    auto &s = *static_cast<IOKitExecutionStaging *>(p);
    return s.exact(c) && s.physical_.quiesced(s.physical_.opaque, c);
}
bool IOKitExecutionStaging::stage(const LiveContext &c, const XeMemory::Handle (&h)[6],
                               const XeDispatch::Prepared &prepared) {
    const auto source = reinterpret_cast<uintptr_t>(&prepared);
    const auto handles = reinterpret_cast<uintptr_t>(&h);
    const auto self = reinterpret_cast<uintptr_t>(this);
    const auto vm = reinterpret_cast<uintptr_t>(&vm_);
    const auto pins = reinterpret_cast<uintptr_t>(&pins_);
    // These immutable inputs may occupy the report or scratch itself in a bad
    // trusted-kernel call. Do not mutate even report_ until that is excluded.
    if (!span(source, sizeof(prepared)) || !span(handles, sizeof(h)) ||
        !span(self, sizeof(*this)) || !span(vm, sizeof(vm_)) || !span(pins, sizeof(pins_)) ||
        overlap(source, sizeof(prepared), self, sizeof(*this)) ||
        overlap(handles, sizeof(h), self, sizeof(*this)) ||
        overlap(vm, sizeof(vm_), self, sizeof(*this)) ||
        overlap(pins, sizeof(pins_), self, sizeof(*this))) return false;
    const XeMemory::Allocation *a[6] {};
    for (size_t i = 0; i < 6; ++i) {
        a[i] = vm_.inspect(c.owner, h[i]);
        // VM inspect performs its own owner/generation lookup. Validate its
        // returned storage before this adapter reads the record, even before
        // resetting report_: bad records could occupy the report itself.
        const auto metadata = reinterpret_cast<uintptr_t>(a[i]);
        if (!span(metadata, sizeof(*a[i])) || overlap(metadata, sizeof(*a[i]), self, sizeof(*this)))
            return false;
        const auto array = reinterpret_cast<uintptr_t>(a[i]->pin.dmaPages);
        if (!a[i]->pin.pageCount || a[i]->pin.pageCount > MaxDataPages ||
            !span(array, a[i]->pin.pageCount * sizeof(uint64_t)) ||
            overlap(array, a[i]->pin.pageCount * sizeof(uint64_t), self, sizeof(*this))) return false;
    }
    report_ = {};
    if (!allowed(c) || !physical_.freshStopped(physical_.opaque, c)) {
        report_.status = IOKitStagingStatus::Unavailable; return false;
    }
    report_.status = IOKitStagingStatus::Invalid;
    if (!prepared.count || prepared.groups != (uint64_t(prepared.count) + 31) / 32 ||
        !prepared.isaBytes || prepared.isaBytes > XeDispatch::HeapBytes ||
        prepared.indirectBytes != 224 || !prepared.batchDwords ||
        prepared.batchDwords > XeDispatch::BatchDwords ||
        prepared.walkerOffset >= prepared.batchDwords ||
        XeDispatch::WalkerDwords >= prepared.batchDwords - prepared.walkerOffset ||
        prepared.batch[prepared.batchDwords - 1] != 0x05000000U) return false;
    uint8_t *cpu[6] {};
    const uint64_t layout[] = {prepared.layout.instruction, prepared.layout.indirect,
        prepared.layout.surface, prepared.layout.batch};
    size_t count = 0;
    for (size_t i = 0; i < 6; ++i) {
        if (a[i]->state != XeMemory::State::Bound || a[i]->activeUses != 1 ||
            !a[i]->address || (a[i]->address & (XeMemory::PageSize - 1)) ||
            a[i]->address >= XeMemory::VaLimit || !a[i]->bytes ||
            a[i]->bytes > XeMemory::VaLimit - a[i]->address ||
            (a[i]->bytes & (XeMemory::PageSize - 1)) ||
            a[i]->bytes > MaxDataPages * XeMemory::PageSize ||
            a[i]->pin.pageCount != a[i]->bytes / XeMemory::PageSize ||
            (reinterpret_cast<uintptr_t>(a[i]->pin.dmaPages) & (alignof(uint64_t) - 1))) return false;
        if (i < 4 && (a[i]->address != layout[i] || a[i]->bytes != XeDispatch::HeapBytes)) return false;
        if (i >= 4 && uint64_t(prepared.count) * 4 > a[i]->bytes) return false;
        cpu[i] = static_cast<uint8_t *>(XeMemory::resolvePinnedBuffer(pins_, c.owner, a[i]->bytes, a[i]->pin));
        if (!span(reinterpret_cast<uintptr_t>(cpu[i]), a[i]->bytes) ||
            (reinterpret_cast<uintptr_t>(cpu[i]) & (XeMemory::PageSize - 1))) return false;
        for (size_t j = 0; j < i; ++j)
            if ((h[i].slot == h[j].slot && h[i].generation == h[j].generation) ||
                a[i]->pin.cookie == a[j]->pin.cookie ||
                overlap(a[i]->address, a[i]->bytes, a[j]->address, a[j]->bytes) ||
                overlap(reinterpret_cast<uintptr_t>(cpu[i]), a[i]->bytes,
                    reinterpret_cast<uintptr_t>(cpu[j]), a[j]->bytes)) return false;
        if (a[i]->pin.pageCount > MaxPages - count) return false;
        count += a[i]->pin.pageCount;
    }
    for (size_t i = 0; i < 6; ++i) {
        const auto destination = reinterpret_cast<uintptr_t>(cpu[i]);
        if (overlap(destination, a[i]->bytes, source, sizeof(prepared)) ||
            overlap(destination, a[i]->bytes, handles, sizeof(h)) ||
            overlap(destination, a[i]->bytes, self, sizeof(*this))) return false;
        for (size_t j = 0; j < 6; ++j) {
            const auto metadata = reinterpret_cast<uintptr_t>(a[j]);
            const auto pages = reinterpret_cast<uintptr_t>(a[j]->pin.dmaPages);
            const auto bytes = a[j]->pin.pageCount * sizeof(uint64_t);
            if (!span(metadata, sizeof(*a[j])) || overlap(destination, a[i]->bytes, metadata, sizeof(*a[j])) ||
                overlap(destination, a[i]->bytes, pages, bytes)) return false;
            if (j < i && overlap(pages, bytes, reinterpret_cast<uintptr_t>(a[i]->pin.dmaPages),
                    a[i]->pin.pageCount * sizeof(uint64_t))) return false;
        }
    }
    count = 0;
    for (size_t i = 0; i < 6; ++i) for (size_t j = 0; j < a[i]->pin.pageCount; ++j) {
        const auto page = a[i]->pin.dmaPages[j];
        if ((page & (XeMemory::PageSize - 1)) || page > XeMemory::DmaLimit - XeMemory::PageSize) return false;
        pages_[count++] = page;
    }
    sort(pages_, count);
    for (size_t i = 1; i < count; ++i) if (pages_[i] == pages_[i - 1]) return false;
    // Verify the two actual buffer surfaces against the exact held allocations,
    // including complete byte extents and the original admitted MOCS argument.
    for (size_t i = 0; i < 2; ++i) {
        uint32_t expected[16] {};
        if (XeDispatch::encodeBufferSurface(a[i + 4]->address, a[i + 4]->bytes,
                policy_.mocsIndex, expected) != XeDispatch::Error::None) return false;
        for (size_t j = 0; j < 16; ++j) if (prepared.surface[i * 16 + j] != expected[j]) return false;
    }
    if (!allowed(c) || !physical_.freshStopped(physical_.opaque, c)) {
        report_.status = IOKitStagingStatus::Unavailable; return false;
    }
    const uint8_t *sources[] = {prepared.isa, prepared.indirect,
        reinterpret_cast<const uint8_t *>(prepared.surface), reinterpret_cast<const uint8_t *>(prepared.batch)};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < XeDispatch::HeapBytes; ++j) cpu[i][j] = sources[i][j];
        const volatile uint8_t *readback = cpu[i];
        for (size_t j = 0; j < XeDispatch::HeapBytes; ++j)
            if (readback[j] != sources[i][j]) { report_.status = IOKitStagingStatus::CopyMismatch; return false; }
        ++report_.copiedHeaps; report_.copiedBytes += XeDispatch::HeapBytes;
    }
    // Prepared contains no input/output initialization. Preserve their bytes;
    // synchronize all six descriptors, including caller-initialized input and
    // output. CPU-to-device DMA synchronization is not a GPU completion proof.
    __sync_synchronize();
    for (size_t i = 0; i < 6; ++i) {
        if (XeMemory::synchronizeForDevice(a[i]->pin) != XeMemory::Status::Ok) {
            report_.status = IOKitStagingStatus::DmaFailure; return false;
        }
        ++report_.synchronizedAllocations;
    }
    if (!allowed(c) || !physical_.freshStopped(physical_.opaque, c)) {
        report_.status = IOKitStagingStatus::Unavailable; return false;
    }
    report_.status = IOKitStagingStatus::Staged; return true;
}
}
