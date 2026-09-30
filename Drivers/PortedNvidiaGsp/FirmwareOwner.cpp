// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and docs/NVIDIA-GSP-FIRMWARE-OWNER.md.
#include "FirmwareOwner.hpp"
#ifdef KERNEL
#include "../../Mellow/NativeMemoryIOKit.hpp"
#endif

namespace Mellow { namespace PortedNvidiaGsp {
namespace {
using namespace MellowNative;
struct CpuSpan { uintptr_t begin, end; };
bool span(const void *data, uint64_t bytes, CpuSpan &out) {
    const auto begin = reinterpret_cast<uintptr_t>(data);
    if (!data || bytes > UINTPTR_MAX - begin) return false;
    out = {begin, begin + static_cast<uintptr_t>(bytes)}; return true;
}
bool overlaps(CpuSpan a, CpuSpan b) { return a.begin < b.end && b.begin < a.end; }
bool same(DeviceIdentity a, DeviceIdentity b) {
    return a.vendor == b.vendor && a.device == b.device && a.registryId == b.registryId && a.epoch == b.epoch;
}
bool same(DmaPin a, DmaPin b) {
    return a.cookie == b.cookie && a.cpu == b.cpu && a.pages == b.pages &&
        a.pageCount == b.pageCount && a.bytes == b.bytes && a.deviceRegistryId == b.deviceRegistryId;
}
uint64_t read(const uint8_t *bytes, unsigned offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
void write(uint8_t *bytes, unsigned offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void swap(uint64_t &a, uint64_t &b) { const auto value = a; a = b; b = value; }
void sift(uint64_t *words, size_t root, size_t count) {
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && words[child] < words[child + 1]) ++child;
        if (words[root] >= words[child]) return;
        swap(words[root], words[child]); root = child;
    }
}
void sort(uint64_t *words, size_t count) {
    for (size_t i = count / 2; i; --i) sift(words, i - 1, count);
    for (size_t n = count; n > 1; --n) { swap(words[0], words[n - 1]); sift(words, 0, n - 1); }
}
bool rounded(uint64_t bytes, uint64_t alignment, uint64_t &out) {
    if (!bytes || bytes > UINT64_MAX - (alignment - 1)) return false;
    out = (bytes + alignment - 1) & ~(alignment - 1); return true;
}
} // namespace

bool FirmwareOwner::admitted() const {
    return initialized_ && authorityHeld_ && view_.state != FirmwareOwnerState::Closed &&
        memoryBackend_.admitted && memoryBackend_.admitted(memoryBackend_.context, device_) &&
        authority_.admitted && authority_.admitted(authority_.context, device_, memoryBackend_);
}
OwnerStatus FirmwareOwner::memoryStatus(MellowNative::MemoryStatus status) {
    view_.lastMemory = status;
    switch (status) {
    case MemoryStatus::Ok: return OwnerStatus::Ok;
    case MemoryStatus::Invalid: return OwnerStatus::Invalid;
    case MemoryStatus::Unavailable: return OwnerStatus::Unavailable;
    case MemoryStatus::Ownership: case MemoryStatus::StaleEpoch: return OwnerStatus::Ownership;
    case MemoryStatus::Capacity: return OwnerStatus::Capacity;
    case MemoryStatus::Busy: return OwnerStatus::Busy;
    case MemoryStatus::Quarantined: return OwnerStatus::Quarantined;
    default: return OwnerStatus::IoFailure;
    }
}
OwnerStatus FirmwareOwner::fail(OwnerStatus status) {
    view_.state = memory_.draining() ? FirmwareOwnerState::Quarantined : FirmwareOwnerState::Failed;
    return status;
}
OwnerStatus FirmwareOwner::initialize(MellowNative::DeviceIdentity device, uint64_t owner,
        MellowNative::MemoryLimits limits, MellowNative::MemoryBackend backend,
        FirmwareBootAuthority authority) {
    if (initialized_) return OwnerStatus::Busy;
    if (device.vendor != 0x10DE || !owner) return OwnerStatus::Invalid;
    if (!backend.context || !authority.context || !authority.admitted || !authority.retain ||
        !authority.release || !authority.select ||
        !authority.populateWpr) return OwnerStatus::Unavailable;
    if (!authority.admitted(authority.context, device, backend)) return OwnerStatus::Ownership;
    const auto result = memoryStatus(memory_.initialize(device, limits, backend));
    if (result != OwnerStatus::Ok) return result;
    device_ = device; owner_ = owner; limits_ = limits; memoryBackend_ = backend;
    authority_ = authority; initialized_ = true; view_.state = FirmwareOwnerState::Initialized;
    if (!authority_.retain(authority_.context, device_, owner_, generation_))
        return fail(OwnerStatus::Unavailable);
    authorityHeld_ = view_.authorityHeld = true;
    return OwnerStatus::Ok;
}
#ifdef KERNEL
OwnerStatus FirmwareOwner::initializeNative(MellowNative::DeviceIdentity device, uint64_t owner,
        MellowNative::MemoryLimits limits, MellowNative::NativeMemoryIOKit &adapter,
        FirmwareBootAuthority authority) {
    return initialize(device, owner, limits, adapter.backend(), authority);
}
#endif
bool FirmwareOwner::currentPins() const {
    if (!complete_ || !admitted()) return false;
    for (size_t i = 0; i < ResourceCount; ++i) {
        MemoryView current;
        if (memory_.inspect(owner_, handles_[i], current) != MemoryStatus::Ok ||
            !same(current.device, device_) || current.state != MemoryState::Pinned ||
            current.jobHolds || !same(current.pin, pins_[i].pin) ||
            current.bytes != pins_[i].bytes || current.mapping.cookie ||
            current.mapping.address || current.mapping.bytes || current.mapping.writable)
            return false;
    }
    return true;
}
OwnerStatus FirmwareOwner::prepare(FirmwareBytes container, FirmwareParseLimits parseLimits,
                                   uint64_t *scratch, size_t scratchWords) {
    if (!initialized_) return OwnerStatus::Unavailable;
    if (view_.state != FirmwareOwnerState::Initialized) return OwnerStatus::Busy;
    if (!admitted()) return OwnerStatus::Ownership;
    view_.state = FirmwareOwnerState::Preparing;
    FirmwareSelection selection;
    BootBinary binary;
    if (!authority_.select(authority_.context, device_, owner_, generation_, selection, binary))
        return fail(OwnerStatus::Ownership);
    FirmwareImageView image;
    view_.lastFirmware = extractFirmwareImage(container, selection, parseLimits, image);
    if (view_.lastFirmware != FirmwareStatus::Ok) return fail(OwnerStatus::FirmwareInvalid);
    CpuSpan source;
    if (!binary.bytes.data || !binary.bytes.size || !span(binary.bytes.data, binary.bytes.size, source) ||
        binary.codeOffset >= binary.bytes.size || binary.dataOffset >= binary.bytes.size ||
        binary.manifestOffset >= binary.bytes.size) return fail(OwnerStatus::Invalid);
    Radix3Layout radix;
    view_.lastRadix = planRadix3(image.image.size, radix);
    if (view_.lastRadix != Status::Ok) return fail(OwnerStatus::FirmwareInvalid);
    uint64_t signatureBytes = 0, signatureAllocation = 0, binaryAllocation = 0;
    if (!rounded(image.signature.size, 256, signatureBytes) ||
        !rounded(signatureBytes, MemoryOwner::PageBytes, signatureAllocation) ||
        !rounded(binary.bytes.size, MemoryOwner::PageBytes, binaryAllocation)) return fail(OwnerStatus::Capacity);
    const uint64_t allocationBytes[ResourceCount] = {
        radix.allocationBytes, signatureAllocation, binaryAllocation, MemoryOwner::PageBytes};
    uint64_t totalBytes = 0;
    size_t totalPages = 0;
    for (uint64_t bytes : allocationBytes) {
        if (bytes > limits_.maxAllocationBytes || bytes > SIZE_MAX ||
            totalBytes > UINT64_MAX - bytes) return fail(OwnerStatus::Capacity);
        totalBytes += bytes;
        const uint64_t pages = bytes / MemoryOwner::PageBytes;
        if (pages > SIZE_MAX - totalPages) return fail(OwnerStatus::Capacity);
        totalPages += static_cast<size_t>(pages);
    }
    if (totalBytes > limits_.maxTotalBytes || !scratch || scratchWords < totalPages ||
        totalPages > SIZE_MAX / sizeof(uint64_t) ||
        (reinterpret_cast<uintptr_t>(scratch) % alignof(uint64_t))) return fail(OwnerStatus::Capacity);
    // Allocate every resource before modifying any DMA buffer. Partial failures
    // are owned by the private MemoryOwner and never automatically unwound.
    for (size_t i = 0; i < ResourceCount; ++i) {
        const auto result = memoryStatus(memory_.allocate(owner_, allocationBytes[i], handles_[i]));
        if (result != OwnerStatus::Ok) return fail(result);
        const auto inspected = memoryStatus(memory_.inspect(owner_, handles_[i], pins_[i]));
        if (inspected != OwnerStatus::Ok) return fail(inspected);
    }
    CpuSpan spans[12];
    if (!span(container.data, container.size, spans[0]) ||
        !span(binary.bytes.data, binary.bytes.size, spans[1]) ||
        !span(scratch, totalPages * sizeof(uint64_t), spans[2]) ||
        !span(this, sizeof(*this), spans[3])) return fail(OwnerStatus::AliasedStorage);
    for (size_t i = 0; i < ResourceCount; ++i) {
        const auto &pin = pins_[i].pin;
        if (!span(pin.cpu, allocationBytes[i], spans[4 + i]) ||
            !span(pin.pages, pin.pageCount * sizeof(uint64_t), spans[8 + i]))
            return fail(OwnerStatus::AliasedStorage);
    }
    for (unsigned i = 0; i < 12; ++i)
        for (unsigned j = i + 1; j < 12; ++j)
            if (overlaps(spans[i], spans[j])) return fail(OwnerStatus::AliasedStorage);
    // Same actual mapper: reject numerical aliases across ALL retained tables,
    // payload, signature, boot image and WPR pages before any DMA-buffer mutation.
    // Scratch is temporary and may change on failure.
    // Physical alias exclusivity beyond IOVM values remains the adapter's proof.
    size_t position = 0;
    for (const auto &resource : pins_)
        for (size_t i = 0; i < resource.pin.pageCount; ++i) scratch[position++] = resource.pin.pages[i];
    sort(scratch, totalPages);
    for (size_t i = 1; i < totalPages; ++i)
        if (scratch[i - 1] == scratch[i]) return fail(OwnerStatus::AliasedStorage);
    // Booter fields name contiguous DMA byte ranges, not scatter/GPU-VA ranges.
    for (size_t resource = 1; resource < ResourceCount; ++resource)
        for (size_t i = 1; i < pins_[resource].pin.pageCount; ++i)
            if (pins_[resource].pin.pages[i] != pins_[resource].pin.pages[i - 1] + MemoryOwner::PageBytes)
                return fail(OwnerStatus::Unavailable);
    const auto &radixPin = pins_[0].pin;
    const size_t tablePages = static_cast<size_t>(radix.tablePageCount);
    view_.lastRadix = serializeRadix3(radix, {radixPin.pages, tablePages},
        {radixPin.pages + tablePages, static_cast<size_t>(radix.pageCount[3])}, limits_.dmaAddressBits,
        image.image.data, image.image.size, radixPin.cpu, static_cast<size_t>(radixPin.bytes),
        scratch, scratchWords);
    if (view_.lastRadix != Status::Ok) return fail(OwnerStatus::FirmwareInvalid);
    for (size_t resource = 1; resource < ResourceCount; ++resource)
        for (size_t i = 0; i < allocationBytes[resource]; ++i) pins_[resource].pin.cpu[i] = 0;
    for (size_t i = 0; i < image.signature.size; ++i) pins_[1].pin.cpu[i] = image.signature.data[i];
    for (size_t i = 0; i < binary.bytes.size; ++i) pins_[2].pin.cpu[i] = binary.bytes.data[i];
    view_.dma = {device_, owner_, generation_, radixPin.pages[0], image.image.size, radix.allocationBytes,
        pins_[1].pin.pages[0], signatureBytes, pins_[2].pin.pages[0], binary.bytes.size,
        binary.codeOffset, binary.dataOffset, binary.manifestOffset, pins_[3].pin.pages[0],
        {radixPin.cpu, static_cast<size_t>(radixPin.bytes)},
        {pins_[1].pin.cpu, static_cast<size_t>(pins_[1].pin.bytes)},
        {pins_[2].pin.cpu, static_cast<size_t>(pins_[2].pin.bytes)},
        {pins_[3].pin.cpu, static_cast<size_t>(pins_[3].pin.bytes)}};
    auto *wpr = pins_[3].pin.cpu;
    // Pinned GspFwWprMeta offsets; CPU never writes the Booter verified sentinel.
    write(wpr, 0, 0xDC3AAE21371A60B3ULL); write(wpr, 8, 1);
    write(wpr, 16, view_.dma.radixRoot); write(wpr, 24, view_.dma.imageBytes);
    write(wpr, 32, view_.dma.bootBinaryAddress); write(wpr, 40, view_.dma.bootBinaryBytes);
    write(wpr, 48, binary.codeOffset); write(wpr, 56, binary.dataOffset); write(wpr, 64, binary.manifestOffset);
    write(wpr, 72, view_.dma.signatureAddress); write(wpr, 80, signatureBytes);
    if (!authority_.populateWpr(authority_.context, view_.dma, wpr, 256) || !wprExact())
        return fail(OwnerStatus::Invalid);
    if (!admitted()) return fail(OwnerStatus::Ownership);
    for (const auto &handle : handles_) {
        const auto result = memoryStatus(memory_.sync(owner_, handle, CacheDirection::ForDevice));
        if (result != OwnerStatus::Ok) return fail(result);
    }
    complete_ = true;
    if (!currentPins()) return fail(OwnerStatus::Ownership);
    view_.dmaStaged = true; view_.state = FirmwareOwnerState::Staged;
    return OwnerStatus::Ok;
}
bool FirmwareOwner::wprExact() const {
    const auto *bytes = pins_[3].pin.cpu;
    const auto &dma = view_.dma;
    return bytes && read(bytes, 0) == 0xDC3AAE21371A60B3ULL && read(bytes, 8) == 1 &&
        read(bytes, 16) == dma.radixRoot && read(bytes, 24) == dma.imageBytes &&
        read(bytes, 32) == dma.bootBinaryAddress && read(bytes, 40) == dma.bootBinaryBytes &&
        read(bytes, 48) == dma.codeOffset && read(bytes, 56) == dma.dataOffset &&
        read(bytes, 64) == dma.manifestOffset && read(bytes, 72) == dma.signatureAddress &&
        read(bytes, 80) == dma.signatureBytes && !read(bytes, 200) && !read(bytes, 248) &&
        !bytes[242] && !bytes[243];
}
OwnerStatus FirmwareOwner::boot() {
    if (!initialized_) return OwnerStatus::Unavailable;
    if (view_.state != FirmwareOwnerState::Staged) return OwnerStatus::Busy;
    if (!currentPins() || !wprExact()) return fail(OwnerStatus::Ownership);
    if (!authority_.bootstrap || !authority_.authenticated || !authority_.waitInitDone)
        return OwnerStatus::Unavailable;
    view_.bootstrapAttempted = true; view_.state = FirmwareOwnerState::BootAttempted;
    if (!authority_.bootstrap(authority_.context, view_.dma)) return fail(OwnerStatus::IoFailure);
    if (!admitted() || !authority_.authenticated(authority_.context, view_.dma))
        return fail(OwnerStatus::AuthenticationFailed);
    view_.authenticated = true;
    if (!admitted() || !authority_.waitInitDone(authority_.context, view_.dma))
        return fail(OwnerStatus::InitDoneFailed);
    if (!admitted()) return fail(OwnerStatus::Ownership);
    view_.rmInitDone = true; view_.state = FirmwareOwnerState::Running;
    return OwnerStatus::Ok;
}
void FirmwareOwner::inspect(FirmwareOwnerView &out) const {
    out = view_; out.ownedResources = memory_.allocations(); out.chargedBytes = memory_.chargedBytes();
    if (!currentPins()) { out.dmaStaged = false; out.dma = {}; }
}
OwnerStatus FirmwareOwner::close() {
    if (!initialized_) return OwnerStatus::Unavailable;
    if (view_.state == FirmwareOwnerState::Closed) return OwnerStatus::Ok;
    // Includes failed/partial preparation and uncertain hardware boot effects.
    // No raw-pin retirement occurs until the backend proves epoch-wide stop.
    const auto result = memoryStatus(memory_.close());
    if (result != OwnerStatus::Ok) { view_.state = FirmwareOwnerState::Quarantined; return result; }
    complete_ = false; view_.dmaStaged = false; view_.dma = {};
    if (authorityHeld_) {
        if (!authority_.release(authority_.context, device_, owner_, generation_)) {
            view_.state = FirmwareOwnerState::Quarantined; return OwnerStatus::Quarantined;
        }
        authorityHeld_ = view_.authorityHeld = false;
    }
    view_.state = FirmwareOwnerState::Closed; return OwnerStatus::Ok;
}
} } // namespace Mellow::PortedNvidiaGsp
