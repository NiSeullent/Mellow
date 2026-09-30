// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Synthetic CPU-backed DMA/controller callbacks ONLY. No acquired firmware,
// IOKit allocation, physical device authentication, boot or Metal pass implied.
#include "Drivers/PortedNvidiaGsp/FirmwareOwner.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace Mellow::PortedNvidiaGsp;
using namespace MellowNative;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL [%u]: %s\n", checks, message); exit(1); }
}
void put(uint8_t *bytes, size_t offset, uint64_t value, unsigned count = 8) {
    for (unsigned i = 0; i < count; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t get(const uint8_t *bytes, size_t offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
bool same(DeviceIdentity a, DeviceIdentity b) {
    return a.vendor == b.vendor && a.device == b.device && a.registryId == b.registryId && a.epoch == b.epoch;
}
FirmwareText text(const char *bytes) { return {bytes, strlen(bytes)}; }
struct Fixture {
    static constexpr size_t ImageOffset = 640, ImageBytes = 7001;
    static constexpr size_t SignatureOffset = 7648, SignatureBytes = 4201;
    std::vector<uint8_t> elf = std::vector<uint8_t>(11872, 0);
    std::vector<uint8_t> binary = std::vector<uint8_t>(4113);
    FirmwareSelection selection {text("610.57.04"), text(".fwsignature_tu10x")};
    BootBinary boot;
    Fixture() {
        elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[6] = 1;
        put(elf.data(), 16, 1, 2); put(elf.data(), 20, 1, 4);
        put(elf.data(), 40, 64); put(elf.data(), 52, 64, 2);
        put(elf.data(), 58, 64, 2); put(elf.data(), 60, 6, 2); put(elf.data(), 62, 1, 2);
        const char names[] = "\0.shstrtab\0.fwversion\0.fwimage\0.fwsignature_tu10x\0.note.gnu.build-id\0";
        memcpy(elf.data() + 448, names, sizeof(names));
        size_t position = 1;
        for (unsigned i = 1; i < 6; ++i) {
            put(elf.data(), 64 + i * 64, position, 4);
            put(elf.data(), 64 + i * 64 + 4, i == 1 ? 3 : i == 5 ? 7 : 1, 4);
            put(elf.data(), 64 + i * 64 + 48, 1);
            position += strlen(names + position) + 1;
        }
        section(1, 448, sizeof(names)); section(2, 560, 10);
        memcpy(elf.data() + 560, "610.57.04", 10);
        section(3, ImageOffset, ImageBytes); section(4, SignatureOffset, SignatureBytes);
        section(5, 11852, 20);
        for (size_t i = 0; i < ImageBytes; ++i) elf[ImageOffset + i] = static_cast<uint8_t>(i * 29 + 3);
        for (size_t i = 0; i < SignatureBytes; ++i) elf[SignatureOffset + i] = static_cast<uint8_t>(i * 17 + 11);
        for (size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<uint8_t>(i * 13 + 19);
        put(elf.data(), 11852, 4, 4); put(elf.data(), 11856, 4, 4); put(elf.data(), 11860, 3, 4);
        memcpy(elf.data() + 11864, "GNU", 4); put(elf.data(), 11868, 0x88776655, 4);
        boot = {{binary.data(), binary.size()}, 17, 71, 203};
    }
    void section(unsigned i, uint64_t offset, uint64_t bytes) {
        put(elf.data(), 64 + i * 64 + 24, offset); put(elf.data(), 64 + i * 64 + 32, bytes);
    }
    FirmwareBytes bytes() const { return {elf.data(), elf.size()}; }
};
struct Fake {
    struct Resource {
        uint8_t *cpu {};
        std::vector<uint64_t> pages;
        uint64_t bytes {};
        MemoryHandle handle;
        bool held {};
        ~Resource() { free(cpu); } // Host fixture cleanup only, not production retirement.
    } resources[4];
    Fixture fixture;
    DeviceIdentity device {0x10DE, 0x1E04, 0x31415926, 7};
    uint64_t owner = 0x1234;
    FirmwareOwner firmware;
    std::vector<uint64_t> scratch = std::vector<uint64_t>(64);
    std::vector<unsigned> events;
    unsigned pinCalls {}, unpinCalls {}, syncDevice {}, syncCpu {}, mapCalls {}, unmapCalls {};
    unsigned stopCalls {}, retainCalls {}, releaseCalls {}, selectCalls {}, wprCalls {};
    unsigned bootCalls {}, authCalls {}, initCalls {}, activePins {};
    unsigned failPin {}, failSync {}, failUnpin {}, noncontiguous {}, aliasPages {}, badPin {};
    unsigned damageWpr {}, revokeAt {};
    bool live = true, authorityLive = true, quiet = true, stopped = false, held = false;
    bool retainOk = true, releaseOk = true, selectOk = true, wprOk = true;
    bool bootOk = true, authOk = true, initOk = true, uncertainUnpin = false;
    bool cleanPinFailure = false;
    static Fake &self(void *p) { return *static_cast<Fake *>(p); }
    static bool admitted(void *p, const DeviceIdentity &device) {
        auto &f = self(p); return f.live && same(f.device, device);
    }
    static MemoryStatus pin(void *p, const DeviceIdentity &device, uint64_t owner,
            const MemoryHandle &handle, uint64_t bytes, DmaPin &out) {
        auto &f = self(p); ++f.pinCalls; f.events.push_back(10 + f.pinCalls);
        check(same(f.device, device) && owner == f.owner && !f.stopped && f.held, "pin exact admitted held authority");
        check(handle.slot < 4 && f.pinCalls <= 4, "private owner allocates at most four resources");
        if (f.failPin == f.pinCalls && f.cleanPinFailure) return MemoryStatus::Capacity;
        auto &r = f.resources[handle.slot];
        check(posix_memalign(reinterpret_cast<void **>(&r.cpu), 4096, static_cast<size_t>(bytes)) == 0,
              "fixture gets actual aligned host backing");
        r.bytes = bytes; r.handle = handle; r.held = true; ++f.activePins;
        memset(r.cpu, 0xA5, static_cast<size_t>(bytes));
        r.pages.resize(static_cast<size_t>(bytes / 4096));
        const uint64_t base = 0x10000000ULL + uint64_t(handle.slot) * 0x1000000;
        for (size_t i = 0; i < r.pages.size(); ++i)
            r.pages[i] = base + i * (handle.slot == 0 ? 8192 : 4096);
        if (f.noncontiguous == f.pinCalls && r.pages.size() > 1) r.pages[1] += 4096;
        if (f.aliasPages == f.pinCalls) r.pages[0] = f.resources[0].pages[0];
        if (f.badPin == f.pinCalls) r.pages.back() = 1ULL << 46;
        out = {&r, r.cpu, r.pages.data(), r.pages.size(), bytes, device.registryId};
        return f.failPin == f.pinCalls ? MemoryStatus::IoFailure : MemoryStatus::Ok;
    }
    static MemoryStatus unpin(void *p, const DeviceIdentity &, uint64_t,
            const MemoryHandle &handle, DmaPin &out) {
        auto &f = self(p); ++f.unpinCalls; f.events.push_back(50 + handle.slot);
        check(f.stopped && f.held, "every raw-DMA unpin follows authoritative epoch stop while controller held");
        if (f.uncertainUnpin || f.failUnpin == f.unpinCalls) {
            f.uncertainUnpin = true; return MemoryStatus::IoFailure;
        }
        auto &r = f.resources[handle.slot];
        check(r.held && out.cookie == &r && out.cpu == r.cpu, "unpin exact acquired descriptor");
        r.held = false; --f.activePins; free(r.cpu); r.cpu = nullptr; out = {};
        return MemoryStatus::Ok;
    }
    static MemoryStatus sync(void *p, const DeviceIdentity &, uint64_t,
            const MemoryHandle &handle, const DmaPin &pin, CacheDirection direction) {
        auto &f = self(p);
        check(pin.cookie == &f.resources[handle.slot] && f.resources[handle.slot].held && f.held,
              "synchronize the exact live held resource");
        if (direction == CacheDirection::ForCpu) { ++f.syncCpu; return MemoryStatus::Ok; }
        ++f.syncDevice; f.events.push_back(30 + handle.slot);
        check(f.pinCalls == 4 && f.wprCalls == 1, "DMA visibility occurs after every allocation and WPR copy");
        check(pin.cpu[0] != 0xA5 || handle.slot == 0, "DMA sync observes copied staging bytes");
        return f.failSync == f.syncDevice ? MemoryStatus::IoFailure : MemoryStatus::Ok;
    }
    static MemoryStatus map(void *p, const DeviceIdentity &, uint64_t, const MemoryHandle &,
            const DmaPin &, bool, GpuMapping &) { ++self(p).mapCalls; return MemoryStatus::Unavailable; }
    static MemoryStatus unmap(void *p, const DeviceIdentity &, uint64_t, const MemoryHandle &,
            const DmaPin &, GpuMapping &) { ++self(p).unmapCalls; return MemoryStatus::Unavailable; }
    static bool retired(void *, const MemoryView &) { return false; }
    static bool quiesce(void *p, const DeviceIdentity &device) {
        auto &f = self(p); ++f.stopCalls; f.events.push_back(40);
        check(same(f.device, device), "stop addresses exact held epoch");
        if (!f.quiet) return false;
        f.stopped = true; return true;
    }
    MemoryBackend backend() { return {this, admitted, pin, unpin, sync, map, unmap, retired, quiesce}; }
    static bool authorityAdmitted(void *p, const DeviceIdentity &device, const MemoryBackend &memory) {
        auto &f = self(p); return f.authorityLive && f.live && same(f.device, device) && memory.context == p;
    }
    static bool retain(void *p, const DeviceIdentity &device, uint64_t owner, uint64_t generation) {
        auto &f = self(p); ++f.retainCalls;
        check(same(f.device, device) && owner == f.owner && generation == 1 && !f.held,
              "controller retain scoped to exact owner epoch/generation");
        if (!f.retainOk) return false;
        f.held = true; return true;
    }
    static bool release(void *p, const DeviceIdentity &device, uint64_t owner, uint64_t generation) {
        auto &f = self(p); ++f.releaseCalls; f.events.push_back(60);
        check(same(f.device, device) && owner == f.owner && generation == 1 && f.held &&
              f.activePins == 0 && f.stopped, "controller release only after stop and all DMA retirements");
        if (!f.releaseOk) return false;
        f.held = false; return true;
    }
    static bool select(void *p, const DeviceIdentity &device, uint64_t owner, uint64_t generation,
                       FirmwareSelection &selection, BootBinary &binary) {
        auto &f = self(p); ++f.selectCalls;
        check(same(f.device, device) && owner == f.owner && generation == 1 && f.held,
              "matching-release signature and boot-binary selection has actual owner hold");
        if (!f.selectOk) return false;
        selection = f.fixture.selection; binary = f.fixture.boot; return true;
    }
    static bool wpr(void *p, const FirmwareDmaImage &dma, uint8_t *bytes, size_t size) {
        auto &f = self(p); ++f.wprCalls; f.events.push_back(20);
        check(size == 256 && bytes == f.resources[3].cpu && dma.owner == f.owner &&
              dma.generation == 1 && same(dma.device, f.device) && f.held,
              "WPR callback receives exact held raw-DMA owner and canonical 256-byte span");
        check(get(bytes, 16) == f.resources[0].pages[0] && get(bytes, 32) == f.resources[2].pages[0] &&
              get(bytes, 72) == f.resources[1].pages[0], "WPR uses mapper DMA addresses, without GPU VA");
        // Synthetic HAL-owned FB fields, never guessed for any physical chip.
        put(bytes, 88, 0xC0000000); put(bytes, 112, 0xD0000000); put(bytes, 168, 0xE0000000);
        if (f.damageWpr == 1) put(bytes, 16, 0);
        if (f.damageWpr == 2) put(bytes, 248, 0xA0A0A0A0A0A0A0A0ULL);
        if (f.damageWpr == 3) put(bytes, 200, 1);
        if (f.damageWpr == 4) bytes[242] = 1;
        if (f.revokeAt == 1) f.authorityLive = false;
        return f.wprOk;
    }
    static bool boot(void *p, const FirmwareDmaImage &dma) {
        auto &f = self(p); ++f.bootCalls; f.events.push_back(70);
        check(f.held && f.activePins == 4 && f.syncDevice == 4 && dma.generation == 1,
              "synthetic bootstrap only sees fully synchronized retained backing");
        if (f.revokeAt == 2) f.authorityLive = false;
        return f.bootOk;
    }
    static bool authenticate(void *p, const FirmwareDmaImage &) {
        auto &f = self(p); ++f.authCalls;
        if (f.revokeAt == 3) f.authorityLive = false;
        return f.authOk;
    }
    static bool initDone(void *p, const FirmwareDmaImage &) {
        auto &f = self(p); ++f.initCalls;
        if (f.revokeAt == 4) f.authorityLive = false;
        return f.initOk;
    }
    FirmwareBootAuthority authority(unsigned callbacks = 0) {
        return {this, authorityAdmitted, retain, release, select, wpr,
                callbacks & 1 ? boot : nullptr, callbacks & 2 ? authenticate : nullptr,
                callbacks & 4 ? initDone : nullptr};
    }
    OwnerStatus initialize(unsigned callbacks = 0, MemoryLimits limits = {}) {
        return firmware.initialize(device, owner, limits, backend(), authority(callbacks));
    }
    OwnerStatus prepare() { return firmware.prepare(fixture.bytes(), {16, 256}, scratch.data(), scratch.size()); }
    FirmwareOwnerView view() { FirmwareOwnerView out; firmware.inspect(out); return out; }
    bool untouched() const {
        for (const auto &r : resources) if (r.held)
            for (size_t i = 0; i < r.bytes; ++i) if (r.cpu[i] != 0xA5) return false;
        return true;
    }
    void cleanClose() {
        quiet = true; check(firmware.close() == OwnerStatus::Ok, "explicit close succeeds after real callback stop fixture");
        check(activePins == 0 && !held && view().state == FirmwareOwnerState::Closed && !view().authorityHeld,
              "close consumes resources and controller hold exactly once");
    }
};
void golden() {
    Fake f; check(f.initialize() == OwnerStatus::Ok && f.view().authorityHeld, "initialization holds controller lifetime");
    const auto original = f.fixture.elf;
    check(f.prepare() == OwnerStatus::Ok, "actual production owner stages all four CPU-backed DMA fixtures");
    auto v = f.view();
    check(v.state == FirmwareOwnerState::Staged && v.dmaStaged && v.ownedResources == 4 &&
          !v.bootstrapAttempted && !v.authenticated && !v.rmInitDone, "staged is not authenticated/RM-ready");
    check(f.fixture.elf == original && f.syncDevice == 4 && !f.mapCalls && !f.unmapCalls,
          "source immutable, every allocation synced, no fabricated GPU VA mapping");
    check(v.chargedBytes == 40960 && v.dma.radixAllocationBytes == 20480 && v.dma.imageBytes == 7001 &&
          v.dma.signatureBytes == 4352 && v.dma.bootBinaryBytes == 4113, "exact raw payload versus rounded DMA extents");
    const auto *radix = v.dma.radixAllocation.data;
    // Independent fixed 3-level/2-payload-page topology. Synthetic image DMA is scattered.
    check(get(radix, 0) == 0x10002000 && get(radix, 4096) == 0x10004000 &&
          get(radix, 8192) == 0x10006000 && get(radix, 8200) == 0x10008000,
          "exact little-endian raw-DMA radix pointers across scattered pages");
    for (size_t page = 0; page < 3; ++page)
        for (size_t i = page == 2 ? 16 : 8; i < 4096; ++i)
            check(radix[page * 4096 + i] == 0, "all unused table bytes zero");
    check(memcmp(radix + 12288, original.data() + Fixture::ImageOffset, Fixture::ImageBytes) == 0,
          "actual selected image copied into retained DMA allocation");
    for (size_t i = 12288 + Fixture::ImageBytes; i < 20480; ++i) check(radix[i] == 0, "image last-page tail zero");
    check(memcmp(v.dma.signatureAllocation.data, original.data() + Fixture::SignatureOffset, Fixture::SignatureBytes) == 0,
          "actual selected signature copied, not authenticated");
    for (size_t i = Fixture::SignatureBytes; i < v.dma.signatureAllocation.size; ++i)
        check(v.dma.signatureAllocation.data[i] == 0, "signature Booter extent and allocation tail zero");
    check(memcmp(v.dma.bootBinaryAllocation.data, f.fixture.binary.data(), f.fixture.binary.size()) == 0,
          "HAL-selected boot binary copied independently of ELF image");
    for (size_t i = f.fixture.binary.size(); i < v.dma.bootBinaryAllocation.size; ++i)
        check(v.dma.bootBinaryAllocation.data[i] == 0, "boot binary allocation padding zero");
    const auto *wpr = v.dma.wprMetadataAllocation.data;
    check(get(wpr, 0) == 0xDC3AAE21371A60B3ULL && get(wpr, 8) == 1 && get(wpr, 16) == 0x10000000 &&
          get(wpr, 24) == 7001 && get(wpr, 32) == 0x12000000 && get(wpr, 40) == 4113 &&
          get(wpr, 48) == 17 && get(wpr, 56) == 71 && get(wpr, 64) == 203 &&
          get(wpr, 72) == 0x11000000 && get(wpr, 80) == 4352 && get(wpr, 200) == 0 && get(wpr, 248) == 0,
          "fixed independent WPR ABI vector has exact DMA addresses/descriptor and no CPU verified sentinel");
    check(get(wpr, 88) == 0xC0000000 && get(wpr, 112) == 0xD0000000 && get(wpr, 168) == 0xE0000000,
          "HAL owns real layout values rather than owner inventing reservations");
    for (size_t i = 256; i < 4096; ++i) check(wpr[i] == 0, "WPR backing beyond exact 256-byte ABI zero");
    check(f.firmware.boot() == OwnerStatus::Unavailable && !f.view().bootstrapAttempted && f.activePins == 4 && f.held,
          "missing physical boot authority preserves staged DMA/context and fails unavailable");
    check(f.firmware.prepare(f.fixture.bytes(), {16, 256}, f.scratch.data(), f.scratch.size()) == OwnerStatus::Busy,
          "single-use owner cannot replace staged image");
    f.cleanClose(); check(f.firmware.close() == OwnerStatus::Ok && f.releaseCalls == 1, "closed owner release idempotent");
    check(f.firmware.initialize(f.device, f.owner, {}, f.backend(), f.authority()) == OwnerStatus::Busy,
          "closed owner never reuses epoch/generation");
    const std::vector<unsigned> expected {11, 12, 13, 14, 20, 30, 31, 32, 33, 40, 53, 52, 51, 50, 60};
    check(f.events == expected, "allocation-copy-sync-stop-reverse-retirement-context-release ordering");
}
void admission() {
    { Fake f; check(f.firmware.prepare(f.fixture.bytes(), {16, 256}, f.scratch.data(), f.scratch.size()) == OwnerStatus::Unavailable &&
          f.firmware.boot() == OwnerStatus::Unavailable && f.firmware.close() == OwnerStatus::Unavailable, "uninitialized owner unavailable"); }
    for (unsigned field = 0; field < 6; ++field) {
        Fake f; auto a = f.authority();
        if (field == 0) a.context = nullptr;
        if (field == 1) a.admitted = nullptr;
        if (field == 2) a.retain = nullptr;
        if (field == 3) a.release = nullptr;
        if (field == 4) a.select = nullptr;
        if (field == 5) a.populateWpr = nullptr;
        check(f.firmware.initialize(f.device, f.owner, {}, f.backend(), a) == OwnerStatus::Unavailable && !f.held && !f.pinCalls,
              "required physical authority cannot be omitted");
    }
    { Fake f; auto d = f.device; d.vendor = 0x8086; check(f.firmware.initialize(d, f.owner, {}, f.backend(), f.authority()) == OwnerStatus::Invalid,
          "NVIDIA boot owner rejects other vendor rather than claiming all GPUs"); }
    { Fake f; auto d = f.device; ++d.epoch; check(f.firmware.initialize(d, f.owner, {}, f.backend(), f.authority()) == OwnerStatus::Ownership,
          "measured exact live epoch authority mandatory"); }
    { Fake f; f.authorityLive = false; check(f.initialize() == OwnerStatus::Ownership && !f.retainCalls, "rejected authority never held"); }
    { Fake f; f.retainOk = false; check(f.initialize() == OwnerStatus::Unavailable && !f.held && !f.pinCalls,
          "clean retain rejection acquires no context/resource"); f.cleanClose(); }
    { Fake f; check(f.initialize() == OwnerStatus::Ok, "admission setup"); f.live = false;
      check(f.prepare() == OwnerStatus::Ownership && !f.pinCalls && f.held, "epoch revocation prevents acquisition but keeps hold"); f.cleanClose(); }
}
void preMutationFailures() {
    { Fake f; check(f.initialize() == OwnerStatus::Ok, "ELF failure setup"); f.fixture.elf[0] = 0;
      check(f.prepare() == OwnerStatus::FirmwareInvalid && f.view().lastFirmware == FirmwareStatus::InvalidElf && !f.pinCalls,
            "malformed firmware cannot allocate/publish"); f.cleanClose(); }
    { Fake f; f.fixture.selection.expectedRelease = text("610.57.05"); check(f.initialize() == OwnerStatus::Ok, "release failure setup");
      check(f.prepare() == OwnerStatus::FirmwareInvalid && !f.pinCalls, "matching-release source required before allocation"); f.cleanClose(); }
    { Fake f; f.selectOk = false; check(f.initialize() == OwnerStatus::Ok, "selection failure setup");
      check(f.prepare() == OwnerStatus::Ownership && !f.pinCalls, "HAL signature/boot-binary selection cannot be fabricated"); f.cleanClose(); }
    for (unsigned offset = 0; offset < 4; ++offset) {
        Fake f; if (offset == 0) f.fixture.boot.bytes = {};
        if (offset == 1) f.fixture.boot.codeOffset = f.fixture.binary.size();
        if (offset == 2) f.fixture.boot.dataOffset = UINT64_MAX;
        if (offset == 3) f.fixture.boot.manifestOffset = f.fixture.binary.size();
        check(f.initialize() == OwnerStatus::Ok, "boot descriptor failure setup");
        check(f.prepare() == OwnerStatus::Invalid && !f.pinCalls, "boot descriptor offsets must be inside actual signed boot binary"); f.cleanClose();
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        Fake f; check(f.initialize() == OwnerStatus::Ok, "scratch failure setup");
        auto *scratch = fault == 0 ? nullptr : fault == 1 ? reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(f.scratch.data()) + 1) : f.scratch.data();
        const size_t words = fault == 2 ? 9 : fault == 3 ? 0 : 64;
        check(f.firmware.prepare(f.fixture.bytes(), {16, 256}, scratch, words) == OwnerStatus::Capacity && !f.pinCalls,
              "null/unaligned/short scratch rejected before allocation"); f.cleanClose();
    }
    for (unsigned fault = 0; fault < 2; ++fault) {
        Fake f; MemoryLimits limits; limits.maxAllocationBytes = fault ? 20480 : 16384; limits.maxTotalBytes = fault ? 36864 : 65536;
        check(f.initialize(0, limits) == OwnerStatus::Ok, "capacity failure setup");
        check(f.prepare() == OwnerStatus::Capacity && !f.pinCalls, "actual per-allocation and total capacity enforced before pin"); f.cleanClose();
    }
    for (unsigned resource : {2U, 3U}) {
        Fake f; f.noncontiguous = resource; check(f.initialize() == OwnerStatus::Ok, "scatter boot range setup");
        check(f.prepare() == OwnerStatus::Unavailable && f.activePins == 4 && f.untouched() && !f.syncDevice,
              "raw Booter range cannot misrepresent scattered DMA as contiguous"); f.cleanClose();
    }
    for (unsigned resource : {2U, 3U, 4U}) {
        Fake f; f.aliasPages = resource; check(f.initialize() == OwnerStatus::Ok, "cross resource DMA alias setup");
        check(f.prepare() == OwnerStatus::AliasedStorage && f.activePins == 4 && f.untouched() && !f.syncDevice,
              "global raw-DMA numerical alias rejected before any DMA buffer modification"); f.cleanClose();
    }
    { Fake f; f.fixture.boot.bytes = {f.fixture.elf.data() + 640, 4096}; check(f.initialize() == OwnerStatus::Ok, "source overlap setup");
      check(f.prepare() == OwnerStatus::AliasedStorage && f.untouched() && !f.syncDevice,
            "firmware/boot binary CPU active source overlap rejected before DMA writes"); f.cleanClose(); }
    for (unsigned target = 0; target < 3; ++target) {
        Fake f; check(f.initialize() == OwnerStatus::Ok, "scratch CPU alias setup");
        auto *scratch = target == 0 ? reinterpret_cast<uint64_t *>(f.fixture.elf.data()) :
                        target == 1 ? reinterpret_cast<uint64_t *>(f.fixture.binary.data()) :
                        reinterpret_cast<uint64_t *>(&f.firmware);
        const auto before = f.fixture.elf;
        check(f.firmware.prepare(f.fixture.bytes(), {16, 256}, scratch, 64) == OwnerStatus::AliasedStorage &&
              f.untouched() && f.fixture.elf == before && !f.syncDevice,
              "scratch overlaps container, boot source or private owner rejected before scratch/DMA writes"); f.cleanClose();
    }
}
void uncertainAcquisitionAndSync() {
    for (unsigned resource = 1; resource <= 4; ++resource) {
        Fake f; f.failPin = resource; check(f.initialize() == OwnerStatus::Ok, "uncertain pin failure setup");
        check(f.prepare() == OwnerStatus::Quarantined && f.activePins == resource && f.view().ownedResources == resource &&
              f.untouched() && !f.syncDevice && f.held, "uncertain pin including cleanup token retained without writes");
        f.quiet = false;
        check(f.firmware.close() == OwnerStatus::Quarantined && !f.unpinCalls && f.activePins == resource && f.held,
              "failed physical stop cannot retire partial acquisition"); f.cleanClose();
    }
    { Fake f; f.failPin = 3; f.cleanPinFailure = true; check(f.initialize() == OwnerStatus::Ok, "clean pin failure setup");
      check(f.prepare() == OwnerStatus::Capacity && f.activePins == 2 && f.view().ownedResources == 2 && f.untouched(),
            "clean rejection keeps earlier allocations without inventing third ownership"); f.cleanClose(); }
    for (unsigned resource = 1; resource <= 4; ++resource) {
        Fake f; f.badPin = resource; check(f.initialize() == OwnerStatus::Ok, "invalid DMA interval setup");
        check(f.prepare() == OwnerStatus::Quarantined && f.activePins == resource && f.untouched(),
              "complete DMA page must fit actual device/mapper width before mutation"); f.cleanClose();
    }
    for (unsigned resource = 1; resource <= 4; ++resource) {
        Fake f; f.failSync = resource; check(f.initialize() == OwnerStatus::Ok, "DMA visibility failure setup");
        check(f.prepare() == OwnerStatus::Quarantined && f.activePins == 4 && !f.view().dmaStaged && f.held,
              "actual DMA synchronization failure retains all partially visible firmware resources");
        check(f.firmware.boot() == OwnerStatus::Busy && !f.bootCalls, "failed visibility cannot bootstrap"); f.cleanClose();
    }
    for (unsigned fault = 0; fault <= 4; ++fault) {
        Fake f; f.damageWpr = fault; f.wprOk = fault != 0; check(f.initialize() == OwnerStatus::Ok, "WPR failure setup");
        check(f.prepare() == OwnerStatus::Invalid && f.activePins == 4 && !f.syncDevice && !f.view().dmaStaged,
              "HAL rejection or modified seeded WPR/verified/bootcount/padding fails while retaining DMA"); f.cleanClose();
    }
    { Fake f; f.revokeAt = 1; check(f.initialize() == OwnerStatus::Ok, "WPR epoch revoke setup");
      check(f.prepare() == OwnerStatus::Ownership && f.activePins == 4 && !f.syncDevice && f.held,
            "physical epoch revoked during population never publishes/boots"); f.cleanClose(); }
}
void bootAndCloseBoundaries() {
    for (unsigned callbacks = 0; callbacks < 7; ++callbacks) {
        Fake f; check(f.initialize(callbacks) == OwnerStatus::Ok && f.prepare() == OwnerStatus::Ok, "missing boot callback setup");
        check(f.firmware.boot() == OwnerStatus::Unavailable && !f.bootCalls && !f.authCalls && !f.initCalls &&
              !f.view().bootstrapAttempted && f.activePins == 4 && f.held, "all physical boot proofs required BEFORE any hardware operation");
        f.cleanClose();
    }
    for (unsigned fault = 0; fault < 7; ++fault) {
        Fake f; check(f.initialize(7) == OwnerStatus::Ok && f.prepare() == OwnerStatus::Ok, "boot result boundary setup");
        if (fault == 0) f.bootOk = false;
        if (fault == 1) f.authOk = false;
        if (fault == 2) f.initOk = false;
        if (fault >= 3) f.revokeAt = fault - 1;
        const auto expected = fault == 0 ? OwnerStatus::IoFailure : fault == 1 || fault == 3 ? OwnerStatus::AuthenticationFailed :
                              fault == 2 || fault == 4 ? OwnerStatus::InitDoneFailed : fault == 5 ? OwnerStatus::Ownership : OwnerStatus::Ok;
        check(f.firmware.boot() == expected, "boot, device authentication and GSP_INIT_DONE failures classified precisely");
        auto v = f.view();
        check(v.bootstrapAttempted && f.bootCalls == 1 && f.activePins == 4 && f.held,
              "attempted boot/failure never releases DMA or physical controller");
        check(v.authenticated == (fault == 2 || fault >= 4) && v.rmInitDone == (fault == 6),
              "actual proof stages independently recorded; synthetic success is not a physical pass");
        check(f.firmware.boot() == OwnerStatus::Busy && f.bootCalls == 1, "single-use cannot blindly retry uncertain boot");
        f.quiet = false;
        check(f.firmware.close() == OwnerStatus::Quarantined && !f.unpinCalls && f.activePins == 4 && f.held,
              "boot uncertainty requires physical all-consumer stop, never timeout-based unpin");
        f.cleanClose();
    }
    { Fake f; check(f.initialize() == OwnerStatus::Ok && f.prepare() == OwnerStatus::Ok, "metadata tamper setup");
      f.resources[3].cpu[248] = 1;
      check(f.firmware.boot() == OwnerStatus::Ownership && !f.bootCalls && f.activePins == 4,
            "CPU verified sentinel tamper cannot become authentication"); f.cleanClose(); }
    { Fake f; check(f.initialize() == OwnerStatus::Ok && f.prepare() == OwnerStatus::Ok, "unpin uncertainty setup");
      f.failUnpin = 2;
      check(f.firmware.close() == OwnerStatus::Quarantined && f.activePins == 3 && f.held && !f.releaseCalls,
            "uncertain inverse DMA completion permanently holds remaining resources/context");
      auto v = f.view(); check(!v.dmaStaged && !v.dma.radixAllocation.data && v.ownedResources == 3,
            "partly retired view cannot expose freed staged CPU pointers");
      check(f.firmware.close() == OwnerStatus::Quarantined && f.activePins == 3 && f.held && !f.releaseCalls,
            "latched uncertain adapter is never repaired by retry or epoch stop");
    }
    { Fake f; check(f.initialize() == OwnerStatus::Ok && f.prepare() == OwnerStatus::Ok, "context release uncertainty setup");
      f.releaseOk = false;
      check(f.firmware.close() == OwnerStatus::Quarantined && f.activePins == 0 && f.held && f.view().authorityHeld,
            "actual controller/context hold remains until its release proves success");
      f.releaseOk = true; f.cleanClose(); check(f.releaseCalls == 2, "release retry only after already completed DMA retirement");
    }
}
} // namespace
int main() {
    golden(); admission(); preMutationFailures(); uncertainAcquisitionAndSync(); bootAndCloseBoundaries();
    printf("PASS: %u synthetic GSP firmware-owner checks; physical GPU/authentication/Metal NOT_RUN\n", checks);
    return 0;
}
