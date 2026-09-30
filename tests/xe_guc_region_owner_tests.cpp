// SPDX-License-Identifier: MIT
// Production Owner, XeGgtt::Manager and Loader; ONLY physical/DMA callbacks are
// simulated. These tests establish no native IOKit/firmware/GPU execution.
#include "../Mellow/XeGuCRegionOwner.hpp"
#ifdef XE_REGION_OWNER_IOKIT_SHIM_TEST
#ifndef KERNEL
#error IOKit factory regression requires the actual KERNEL factory branch
#endif
#include "native_memory_iokit_shim.hpp"
#include "../Mellow/XeMemoryIOKit.hpp"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace XeGuCRegions;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (false)
struct Device {
    static constexpr uint64_t OwnerId = 11, Epoch = 7, Base = 8ULL * 1024 * 1024;
    struct Resource {
        uint64_t owner {}, bytes {}, number {};
        uint8_t *cpu {}, *reportedCpu {};
        std::vector<uint64_t> pages;
        const uint64_t *reportedPages {};
        size_t reportedCount {};
        bool live {true};
    };
    std::vector<std::unique_ptr<Resource>> resources;
    std::vector<uint64_t> ptes = std::vector<uint64_t>(4096);
    std::vector<uint64_t> freed;
    std::unique_ptr<Owner> regions = std::make_unique<Owner>();
    bool live {true}, grant {true}, quiet {true}, resetPermission {true}, spaceRelease {true};
    bool invalidateWorks {true}, pinFail {}, partialPinFail {}, emptyPinSuccess {}, syncWorks {true};
    bool unpinWorks {true}, unpinLeavesToken {}, wrongCpu {}, overflowCpu {}, overflowPages {};
    bool metadataInCpu {}, aliasCpu {}, badPage {}, corruptWrites {}, cpuAccessible {true};
    bool revokeOnSync {}, corruptPteOnSync {}, revokeCpuOnSync {};
    uint32_t pat {2};
    unsigned pins {}, unpins {}, cpuChecks {}, syncs {}, reads {}, writes {}, invalidations {}, releases {}, mmioWrites {};
    unsigned failWrite {}, failRead {};
    uint64_t totalPinned {}, number {};
    ~Device() {
        // Simulated fixtures may intentionally remain quarantined. Their teardown
        // frees fixture storage only; production Owner has no cleanup destructor.
        for (auto &r : resources) if (r->cpu) std::free(r->cpu);
    }
    static Device &d(void *p) { return *static_cast<Device *>(p); }
    Resource *resolve(const XeMemory::Pin &pin) {
        for (auto &r : resources) if (r.get() == pin.cookie && r->live) return r.get();
        return nullptr;
    }
    static XeMemory::Status pin(void *p, uint64_t owner, uint64_t bytes, XeMemory::Pin &out) {
        auto &s = d(p); ++s.pins; out = {};
        if (s.pinFail) return XeMemory::Status::BackendFailure;
        if (s.emptyPinSuccess) return XeMemory::Status::Ok;
        auto r = std::make_unique<Resource>(); r->owner = owner; r->bytes = bytes; r->number = ++s.number;
        r->cpu = static_cast<uint8_t *>(std::aligned_alloc(4096, static_cast<size_t>(bytes)));
        CHECK(r->cpu != nullptr); std::memset(r->cpu, 0, static_cast<size_t>(bytes));
        r->reportedCpu = r->cpu;
        r->pages.resize(static_cast<size_t>(bytes / 4096));
        for (size_t i = 0; i < r->pages.size(); ++i)
            r->pages[i] = 0x100000000ULL + r->number * Owner::MaxBytes + i * 4096;
        r->reportedPages = r->pages.data(); r->reportedCount = r->pages.size();
        if (s.badPage) r->pages[0] |= 1;
        if (s.wrongCpu) r->reportedCpu = r->cpu + 4;
        if (s.overflowCpu) r->reportedCpu = reinterpret_cast<uint8_t *>(UINTPTR_MAX & ~uintptr_t(4095));
        if (s.overflowPages) r->reportedPages = reinterpret_cast<uint64_t *>(UINTPTR_MAX & ~uintptr_t(7));
        if (s.metadataInCpu) {
            r->reportedPages = reinterpret_cast<uint64_t *>(r->cpu);
            reinterpret_cast<uint64_t *>(r->cpu)[0] = r->pages[0];
        }
        if (s.aliasCpu && !s.resources.empty()) r->reportedCpu = s.resources[0]->reportedCpu;
        auto *raw = r.get(); s.resources.push_back(std::move(r)); s.totalPinned += bytes;
        out = {raw, raw->reportedPages, raw->reportedCount};
        return s.partialPinFail ? XeMemory::Status::BackendFailure : XeMemory::Status::Ok;
    }
    static XeMemory::Status unpin(void *p, XeMemory::Pin &pin) {
        auto &s = d(p); ++s.unpins; auto *r = s.resolve(pin);
        if (!r) return XeMemory::Status::Invalid;
        if (!s.unpinWorks) return XeMemory::Status::BackendFailure;
        if (s.unpinLeavesToken) return XeMemory::Status::Ok;
        r->live = false; s.totalPinned -= r->bytes; s.freed.push_back(r->number);
        std::free(r->cpu); r->cpu = nullptr; pin = {}; return XeMemory::Status::Ok;
    }
    static bool cpu(void *p, uint64_t owner, const XeMemory::Pin &pin, uint64_t bytes, uint8_t *&out) {
        auto &s = d(p); ++s.cpuChecks; auto *r = s.resolve(pin); out = nullptr;
        if (!r || !s.cpuAccessible || r->owner != owner || r->bytes != bytes ||
            pin.dmaPages != r->reportedPages || pin.pageCount != r->reportedCount) return false;
        out = r->reportedCpu; return true;
    }
    static XeMemory::Status sync(void *p, const XeMemory::Pin &pin, bool device) {
        auto &s = d(p); ++s.syncs; CHECK(device); CHECK(s.resolve(pin) != nullptr);
        if (s.syncWorks) {
            if (s.revokeOnSync) s.live = false;
            if (s.corruptPteOnSync) s.ptes[0] ^= 4096;
            if (s.revokeCpuOnSync) s.cpuAccessible = false;
        }
        return s.syncWorks ? XeMemory::Status::Ok : XeMemory::Status::BackendFailure;
    }
    Pins pinOps() {
        XeMemory::Backend b {}; b.context = this; b.pin = pin; b.unpin = unpin;
        return {b, this, cpu, sync};
    }
    static bool identity(void *p, uint64_t owner, uint64_t epoch) {
        return d(p).live && owner == OwnerId && epoch == Epoch;
    }
    static bool acquire(void *p, uint64_t owner, uint64_t epoch, const XeGgtt::Range *r, size_t count) {
        auto &s = d(p); CHECK(identity(p, owner, epoch)); CHECK(r != nullptr && count == 1);
        return s.grant;
    }
    static bool release(void *p, uint64_t owner, uint64_t epoch) {
        auto &s = d(p); CHECK(identity(p, owner, epoch)); ++s.releases;
        CHECK(s.totalPinned == 0); return s.spaceRelease;
    }
    static bool reset(void *p, uint64_t owner, uint64_t epoch) {
        return identity(p, owner, epoch) && d(p).resetPermission;
    }
    static bool stopped(void *p, uint64_t owner, uint64_t epoch) {
        return identity(p, owner, epoch) && d(p).quiet;
    }
    static bool pat3(void *p, uint64_t owner, uint64_t epoch, uint32_t &value) {
        if (!identity(p, owner, epoch)) return false;
        value = d(p).pat; return true;
    }
    static size_t page(Device &s, uint64_t address) {
        CHECK(address >= Base && !(address & 4095));
        const auto i = static_cast<size_t>((address - Base) / 4096);
        CHECK(i < s.ptes.size()); return i;
    }
    static bool read(void *p, uint64_t owner, uint64_t epoch, uint64_t address, uint64_t &value) {
        auto &s = d(p); CHECK(identity(p, owner, epoch)); const auto i = page(s, address);
        if (++s.reads == s.failRead) return false;
        value = s.ptes[i]; return true;
    }
    static bool write(void *p, uint64_t owner, uint64_t epoch, uint64_t address, uint64_t value) {
        auto &s = d(p); CHECK(identity(p, owner, epoch)); const auto i = page(s, address);
        s.ptes[i] = s.corruptWrites ? 0xdead : value;
        return ++s.writes != s.failWrite; // Publication can occur before an error.
    }
    static bool invalidate(void *p, uint64_t owner, uint64_t epoch) {
        auto &s = d(p); CHECK(identity(p, owner, epoch)); ++s.invalidations; return s.invalidateWorks;
    }
    Hardware hardware() {
        return {this, identity, acquire, release, reset, stopped, pat3, read, write, invalidate};
    }
    Status initialize() {
        const XeGgtt::Range range {Base, ptes.size() * 4096ULL};
        return regions->initialize(OwnerId, Epoch, &range, 1, pinOps(), hardware());
    }
    Handle allocation(uint64_t bytes = 8192) {
        Handle h; CHECK(regions->allocate(OwnerId, Epoch, bytes, h) == Status::Ok); return h;
    }
    XeGuCFirmware::Region exported(Handle h) {
        XeGuCFirmware::Region r; CHECK(regions->region(h, r) == Status::Ok); return r;
    }
    XeGuCFirmware::Backend backend() { return regions->firmwareBackend({}, 9); }
};

static void normalOwnership() {
    Device d; CHECK(d.initialize() == Status::Ok); const auto h = d.allocation(); const auto r = d.exported(h);
    CHECK(d.ptes[0] == (r.dmaPages[0] | 1ULL | (3ULL << 52)));
    CHECK(d.regions->chargedBytes() == 8192 && d.totalPinned == 8192);
    const uint8_t bytes[] = {1, 2, 3, 4}; CHECK(d.regions->write(h, 5, bytes, 4) == Status::Ok);
    CHECK(std::memcmp(r.cpu + 5, bytes, 4) == 0);
    auto b = d.backend(); CHECK(b.preloadAdsValid == nullptr && b.goldenAdsValid == nullptr);
    CHECK(b.admitted(b.opaque, Device::OwnerId, Device::Epoch));
    CHECK(!b.admitted(b.opaque, Device::OwnerId, Device::Epoch + 1));
    uint32_t pat = 0; CHECK(b.readPat3(b.opaque, pat) && pat == 2);
    CHECK(!b.mappingPublished(b.opaque, r, Device::Epoch)); // No GuC hold yet.
    CHECK(b.retain(b.opaque, r, false));
    const auto reads = d.reads, cpuChecks = d.cpuChecks;
    CHECK(!b.retain(b.opaque, r, true) && !b.retain(b.opaque, r, false));
    CHECK(d.reads == reads && d.cpuChecks == cpuChecks);
    View view; CHECK(d.regions->inspect(h, view) == Status::Ok);
    CHECK(view.firmwareReferences == 1 && view.ggttBackingHeld);
    CHECK(b.mappingPublished(b.opaque, r, Device::Epoch));
    CHECK(b.synchronize(b.opaque, r) && d.syncs == 1);
    CHECK(d.regions->write(h, 0, bytes, 4) == Status::Busy);
    CHECK(d.regions->retire(h) == Status::Busy && d.unpins == 0);
    d.quiet = false;
    CHECK(b.quiesced(b.opaque, Device::OwnerId, Device::Epoch)); // Reset permission alone.
    CHECK(!b.release(b.opaque, r));
    CHECK(d.regions->inspect(h, view) == Status::Ok && view.firmwareReferences == 1);
    d.quiet = true; CHECK(b.release(b.opaque, r));
    CHECK(!b.release(b.opaque, r));
    CHECK(d.regions->retire(h) == Status::Ok);
    CHECK(d.ptes[0] == 0 && d.ptes[1] == 0 && d.totalPinned == 0);
    CHECK(d.regions->region(h, view.region) == Status::NotFound);
    CHECK(d.regions->close() == Status::Ok && d.regions->close() == Status::Ok);
    CHECK(d.releases == 1);
}
static void exactIdentityAndNoMutation() {
    Device d; CHECK(d.initialize() == Status::Ok); const auto h = d.allocation(); const auto r = d.exported(h);
    const unsigned reads = d.reads, writes = d.writes, cpuChecks = d.cpuChecks;
    const uint8_t bytes[] = {4, 5};
    auto wrong = h; ++wrong.owner; CHECK(d.regions->write(wrong, 0, bytes, 2) == Status::WrongOwner);
    wrong = h; ++wrong.epoch; CHECK(d.regions->retire(wrong) == Status::WrongEpoch);
    wrong = h; ++wrong.generation; CHECK(d.regions->write(wrong, 0, bytes, 2) == Status::NotFound);
    wrong = h; wrong.slot = SIZE_MAX; CHECK(d.regions->retire(wrong) == Status::NotFound);
    auto b = d.backend();
    for (unsigned field = 0; field < 8; ++field) {
        auto forged = r;
        switch (field) {
        case 0: ++forged.owner; break;
        case 1: ++forged.generation; break;
        case 2: forged.ggtt += 4096; break;
        case 3: forged.bytes += 4096; break;
        case 4: forged.cpu = reinterpret_cast<uint8_t *>(uintptr_t(1)); break;
        case 5: forged.dmaPages = reinterpret_cast<uint64_t *>(uintptr_t(1)); break;
        case 6: ++forged.pageCount; break;
        default: forged.pinCookie = reinterpret_cast<void *>(uintptr_t(1)); break;
        }
        CHECK(!b.retain(b.opaque, forged, false)); CHECK(!b.release(b.opaque, forged));
        CHECK(!b.synchronize(b.opaque, forged)); CHECK(!b.mappingPublished(b.opaque, forged, Device::Epoch));
    }
    CHECK(d.reads == reads && d.writes == writes && d.cpuChecks == cpuChecks);
    CHECK(d.regions->write(h, 8191, bytes, 2) == Status::Invalid);
    CHECK(d.regions->write(h, UINT64_MAX, bytes, 2) == Status::Invalid);
    CHECK(d.regions->write(h, 0, reinterpret_cast<uint8_t *>(UINTPTR_MAX), 2) == Status::Invalid);
    CHECK(d.regions->write(h, 0, nullptr, 1) == Status::Invalid);
    CHECK(d.regions->write(h, 0, bytes, 0) == Status::Invalid);
    CHECK(d.regions->write(h, 0, r.cpu, 2) == Status::Invalid);
    CHECK(r.cpu[0] == 0 && r.cpu[8191] == 0);
    d.quiet = false; CHECK(d.regions->write(h, 0, bytes, 2) == Status::Busy);
    d.quiet = true; CHECK(d.regions->retire(h) == Status::Ok);
    const auto newer = d.allocation(); CHECK(newer.slot == h.slot && newer.generation != h.generation);
    CHECK(!b.retain(b.opaque, r, false)); CHECK(d.regions->retire(h) == Status::NotFound);
    CHECK(d.regions->close() == Status::Ok);
}
static void inputAdmission() {
    Device d; auto pins = d.pinOps(); auto hardware = d.hardware();
    XeGgtt::Range range {Device::Base, 8192};
    CHECK(d.regions->initialize(0, Device::Epoch, &range, 1, pins, hardware) == Status::Invalid);
    CHECK(d.regions->initialize(Device::OwnerId, 0, &range, 1, pins, hardware) == Status::Invalid);
    auto missing = hardware; missing.consumersQuiesced = nullptr;
    CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &range, 1, pins, missing) == Status::Unavailable);
    auto noSync = pins; noSync.synchronize = nullptr;
    CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &range, 1, noSync, hardware) == Status::Unavailable);
    const XeGgtt::Range overflow {XeGuCFirmware::gucGgttTop - 4096, 8192};
    CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &overflow, 1, pins, hardware) == Status::Invalid);
    const XeGgtt::Range low {4096, 8192};
    CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &low, 1, pins, hardware) == Status::Invalid);
    d.grant = false; CHECK(d.initialize() == Status::Unavailable);
    d.grant = true; CHECK(d.initialize() == Status::Ok);
    Handle h;
    CHECK(d.regions->allocate(Device::OwnerId + 1, Device::Epoch, 4096, h) == Status::WrongOwner);
    CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch + 1, 4096, h) == Status::WrongEpoch);
    CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4097, h) == Status::Invalid);
    CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, Owner::MaxBytes + 4096, h) == Status::Invalid);
    d.quiet = false; CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, h) == Status::Busy);
    d.quiet = true; d.live = false;
    CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, h) == Status::Unavailable);
    CHECK(d.pins == 0 && d.writes == 0); d.live = true; CHECK(d.regions->close() == Status::Ok);
}
static void pinAndCpuFailures() {
    for (unsigned failure = 0; failure < 8; ++failure) {
        Device d; CHECK(d.initialize() == Status::Ok);
        switch (failure) {
        case 0: d.pinFail = true; break;
        case 1: d.partialPinFail = true; break;
        case 2: d.wrongCpu = true; break;
        case 3: d.overflowCpu = true; break;
        case 4: d.overflowPages = true; break;
        case 5: d.metadataInCpu = true; break;
        case 6: d.badPage = true; break;
        default: d.cpuAccessible = false; break;
        }
        Handle h; const auto status = d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, h);
        CHECK(status == (failure < 2 ? Status::Io : Status::Invalid));
        CHECK(d.regions->allocations() == 0 && d.regions->chargedBytes() == 0 && d.totalPinned == 0);
        CHECK(d.writes == 0 && d.invalidations == 0 && h.slot == SIZE_MAX);
        CHECK(d.unpins == (failure == 0 ? 0U : 1U)); CHECK(d.regions->close() == Status::Ok);
    }
    { Device d; CHECK(d.initialize() == Status::Ok); d.emptyPinSuccess = true; Handle h;
      CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, h) == Status::Quarantined);
      CHECK(d.regions->retire(h) == Status::Quarantined && d.regions->chargedBytes() == 4096);
      CHECK(d.regions->close() == Status::Quarantined && d.releases == 0); }
    { Device d; CHECK(d.initialize() == Status::Ok); const auto first = d.allocation(4096);
      const auto before = d.ptes; d.aliasCpu = true; Handle other;
      CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, other) == Status::Invalid);
      CHECK(d.ptes == before && d.regions->allocations() == 1 && d.totalPinned == 4096);
      CHECK(d.regions->retire(first) == Status::Ok && d.regions->close() == Status::Ok); }
}
static void publicationInverseAndQuarantine() {
    for (unsigned failure = 0; failure < 5; ++failure) {
        Device d; CHECK(d.initialize() == Status::Ok);
        if (failure == 0) d.pat = 1;
        if (failure == 1) d.failWrite = 1;
        if (failure == 2) d.failRead = 3; // Two pre-publication reads, then PTE readback.
        if (failure == 3) d.invalidateWorks = false;
        if (failure == 4) d.corruptWrites = true;
        Handle h; const auto status = d.regions->allocate(Device::OwnerId, Device::Epoch, 8192, h);
        if (failure < 3) {
            CHECK(status == Status::Io && d.totalPinned == 0 && d.regions->allocations() == 0);
            CHECK(d.ptes[0] == 0 && d.ptes[1] == 0 && h.slot == SIZE_MAX);
            CHECK(d.regions->close() == Status::Ok);
        } else {
            CHECK(status == Status::Quarantined && d.regions->draining());
            CHECK(d.totalPinned == 8192 && d.unpins == 0 && h.slot != SIZE_MAX);
            View view; CHECK(d.regions->inspect(h, view) == Status::Ok && view.ggttBackingHeld);
            if (failure == 3) {
                CHECK(d.ptes[0] == 0 && d.ptes[1] == 0);
                d.invalidateWorks = true; CHECK(d.regions->retire(h) == Status::Ok);
                CHECK(d.totalPinned == 0 && d.regions->close() == Status::Ok);
            } else {
                CHECK(d.regions->retire(h) == Status::Quarantined && d.totalPinned == 8192);
                CHECK(d.regions->close() == Status::Quarantined);
            }
        }
    }
    { Device d; CHECK(d.initialize() == Status::Ok); d.ptes[0] = 0x1234; Handle h;
      CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, h) == Status::Quarantined);
      CHECK(d.writes == 0 && d.ptes[0] == 0x1234 && d.totalPinned == 4096);
      CHECK(d.regions->retire(h) == Status::Quarantined && d.unpins == 0); }
}
static void freshReadbackAndEpochLoss() {
    Device d; CHECK(d.initialize() == Status::Ok); const auto h = d.allocation(); const auto r = d.exported(h);
    auto b = d.backend(); CHECK(b.retain(b.opaque, r, false));
    d.live = false; CHECK(!b.release(b.opaque, r)); CHECK(!b.synchronize(b.opaque, r));
    View view; CHECK(d.regions->inspect(h, view) == Status::Ok && view.firmwareReferences == 1);
    CHECK(d.unpins == 0 && d.regions->draining());
    d.live = true; d.ptes[0] ^= 4096;
    CHECK(!b.mappingPublished(b.opaque, r, Device::Epoch)); CHECK(!b.retain(b.opaque, r, false));
    CHECK(b.release(b.opaque, r));
    CHECK(d.regions->retire(h) == Status::Quarantined && d.unpins == 0);
    d.ptes[0] = r.dmaPages[0] | 1ULL | (3ULL << 52);
    CHECK(d.regions->retire(h) == Status::Ok && d.regions->close() == Status::Ok);
}
static void synchronizationAndUnpinUncertainty() {
    { Device d; CHECK(d.initialize() == Status::Ok); const auto h = d.allocation(); const auto r = d.exported(h);
      auto b = d.backend(); CHECK(b.retain(b.opaque, r, true)); d.syncWorks = false;
      CHECK(!b.synchronize(b.opaque, r) && d.regions->draining());
      CHECK(d.regions->retire(h) == Status::Busy && d.totalPinned == 8192);
      d.quiet = false; CHECK(!b.release(b.opaque, r)); d.quiet = true;
      CHECK(b.release(b.opaque, r)); CHECK(d.regions->retire(h) == Status::Ok);
      CHECK(d.regions->close() == Status::Ok); }
    for (bool token : {false, true}) {
        Device d; CHECK(d.initialize() == Status::Ok); const auto h = d.allocation();
        d.unpinWorks = token; d.unpinLeavesToken = token;
        CHECK(d.regions->retire(h) == Status::Quarantined);
        CHECK(d.ptes[0] == 0 && d.ptes[1] == 0 && d.totalPinned == 8192 && d.unpins == 1);
        d.unpinWorks = true; d.unpinLeavesToken = false;
        CHECK(d.regions->retire(h) == Status::Quarantined && d.unpins == 1);
        CHECK(d.regions->close() == Status::Quarantined && d.unpins == 1 && d.releases == 0);
    }
}
static void reverseCleanupAndSpaceLease() {
    Device d; CHECK(d.initialize() == Status::Ok); const auto first = d.allocation(4096);
    const auto second = d.allocation(4096); CHECK(d.regions->retire(first) == Status::Ok);
    const auto third = d.allocation(4096); CHECK(third.slot == first.slot && third.generation > second.generation);
    d.spaceRelease = false; CHECK(d.regions->close() == Status::Quarantined);
    CHECK(d.freed == std::vector<uint64_t>({1, 3, 2})); CHECK(d.regions->chargedBytes() == 0);
    CHECK(d.regions->allocations() == 0 && d.releases == 1);
    d.spaceRelease = true; CHECK(d.regions->close() == Status::Ok && d.releases == 2);
}
static void successfulSyncMustRevalidateAuthorityAndBacking() {
    for (unsigned fault = 0; fault < 3; ++fault) {
        Device d; CHECK(d.initialize() == Status::Ok); const auto h = d.allocation(); const auto r = d.exported(h);
        auto b = d.backend(); CHECK(b.retain(b.opaque, r, true));
        const auto writes = d.writes; const auto pte = d.ptes[0];
        switch (fault) {
        case 0: d.revokeOnSync = true; break;
        case 1: d.corruptPteOnSync = true; break;
        default: d.revokeCpuOnSync = true; break;
        }
        // The callback reports successful synchronization. Actual production
        // Owner must then reject the changed epoch, PTE or CPU association.
        CHECK(!b.synchronize(b.opaque, r) && d.syncs == 1 && d.regions->draining());
        View view; CHECK(d.regions->inspect(h, view) == Status::Ok);
        CHECK(view.state == State::Quarantined && view.firmwareReferences == 1 && view.ggttBackingHeld);
        CHECK(d.totalPinned == 8192 && d.regions->chargedBytes() == 8192 && d.unpins == 0 && d.writes == writes);
        CHECK(d.regions->retire(h) == Status::Busy);
        d.live = true; d.cpuAccessible = true; d.ptes[0] = pte;
        d.revokeOnSync = d.corruptPteOnSync = d.revokeCpuOnSync = false;
        d.quiet = false; CHECK(!b.release(b.opaque, r));
        CHECK(d.regions->retire(h) == Status::Busy && d.unpins == 0 && d.totalPinned == 8192);
        d.quiet = true; CHECK(b.release(b.opaque, r));
        CHECK(d.regions->retire(h) == Status::Ok && d.totalPinned == 0);
        CHECK(d.regions->close() == Status::Ok);
    }
}
static void loaderRejectsRealUnidentifiedImage() {
  for (bool consumersStopped : {true, false}) {
    Device d; CHECK(d.initialize() == Status::Ok);
    const auto firmware = d.allocation((XeGuCFirmware::firmwareFileBytes + 4095ULL) & ~4095ULL);
    const auto ads = d.allocation(XeGuCFirmware::minimalAdsPrefix + 8392704ULL);
    const auto log = d.allocation(XeGuCFirmware::logBytes);
    XeGuCFirmware::Plan plan; plan.owner = Device::OwnerId; plan.epoch = Device::Epoch; plan.pciRevision = 9;
    plan.firmware = d.exported(firmware); plan.ads = d.exported(ads); plan.log = d.exported(log);
    auto backend = d.backend();
    backend.io = {&d,
        [](void *, uint32_t, uint32_t &value) { value = 0; return true; },
        [](void *p, uint32_t, uint32_t) { ++Device::d(p).mmioWrites; return true; },
        [](void *) -> uint64_t { return 1; }, [](void *, uint32_t) {}};
    XeGuCFirmware::Loader loader(backend);
    // Actual Loader hashes its actual pinned CPU bytes and refuses this zero
    // image. Reset permission alone cannot drop its Manager/backing holds.
    d.quiet = consumersStopped;
    CHECK(backend.quiesced(backend.opaque, Device::OwnerId, Device::Epoch));
    CHECK(loader.start(plan) == (consumersStopped ? XeGuCFirmware::Error::FirmwareMismatch :
        XeGuCFirmware::Error::Quarantined));
    CHECK(loader.heldRegions() == (consumersStopped ? 0U : 3U));
    CHECK(d.mmioWrites == 0 && d.syncs == 0);
    for (const auto h : {firmware, ads, log}) {
        View view; CHECK(d.regions->inspect(h, view) == Status::Ok);
        CHECK(view.firmwareReferences == (consumersStopped ? 0U : 1U) && view.ggttBackingHeld);
    }
    if (!consumersStopped) {
        CHECK(d.regions->close() == Status::Busy && d.regions->draining());
        CHECK(d.unpins == 0 && d.releases == 0 && loader.heldRegions() == 3);
        CHECK(loader.resetAndRelease() == XeGuCFirmware::Error::Quarantined);
        CHECK(loader.heldRegions() == 3 && d.mmioWrites == 0);
        d.quiet = true;
        CHECK(loader.resetAndRelease() == XeGuCFirmware::Error::None);
        CHECK(loader.heldRegions() == 0 && d.mmioWrites == 0);
    }
    CHECK(d.regions->close() == Status::Ok);
    CHECK(d.freed == std::vector<uint64_t>({3, 2, 1}));
  }
}
static void allocationBound() {
    Device d; CHECK(d.initialize() == Status::Ok);
    for (size_t i = 0; i < Owner::MaxRegions; ++i) d.allocation(4096);
    const auto pins = d.pins; const auto writes = d.writes;
    Handle extra; CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 4096, extra) == Status::NoSpace);
    CHECK(extra.slot == SIZE_MAX && d.pins == pins && d.writes == writes);
    CHECK(d.regions->allocations() == Owner::MaxRegions && d.regions->chargedBytes() == 8 * 4096);
    CHECK(d.regions->close() == Status::Ok && d.unpins == Owner::MaxRegions);
    CHECK(d.freed == std::vector<uint64_t>({8, 7, 6, 5, 4, 3, 2, 1}));
}
#ifdef XE_REGION_OWNER_IOKIT_SHIM_TEST
static void actualIOKitFactory() {
    // Actual factory and actual XeMemoryIOKit allocation/resolver/synchronizer;
    // the XNU OS and device mapping boundary remains an explicitly fake shim.
    namespace Mock = NativeMemoryShim;
    Mock::reset(); auto *mapper = new IOMapper;
    XeMemory::IOKitContext context; context.mapper = mapper;
    const auto pins = makeIOKitPins(context); XeMemory::Pin pin; uint8_t *cpu = nullptr;
    CHECK(pins.backend.context == &context && pins.opaque == &context);
    CHECK(pins.backend.pin(pins.backend.context, Device::OwnerId, 8192, pin) == XeMemory::Status::Ok);
    CHECK(pins.cpu(pins.opaque, Device::OwnerId, pin, 8192, cpu));
    CHECK(cpu != nullptr && cpu == XeMemory::kernelBuffer(pin));
    CHECK(!pins.cpu(pins.opaque, Device::OwnerId + 1, pin, 8192, cpu) && cpu == nullptr);
    CHECK(!pins.cpu(pins.opaque, Device::OwnerId, pin, 4096, cpu) && cpu == nullptr);
    XeMemory::IOKitContext foreign; foreign.mapper = mapper;
    CHECK(!pins.cpu(&foreign, Device::OwnerId, pin, 8192, cpu) && cpu == nullptr);
    CHECK(pins.synchronize(pins.opaque, pin, true) == XeMemory::Status::Ok);
    CHECK(Mock::syncCalls == 1 && context.pinnedBytes == 8192);
    CHECK(pins.backend.unpin(pins.backend.context, pin) == XeMemory::Status::Ok);
    CHECK(!pins.cpu(pins.opaque, Device::OwnerId, pin, 8192, cpu) && cpu == nullptr);
    CHECK(context.pinnedBytes == 0 && Mock::unsafeClearCalls == 0 && Mock::prematureDestroy == 0);
    mapper->release(); CHECK(Mock::objects.empty() && Mock::allocations.empty());
}
static void actualIOKitSyncRevalidation() {
    namespace Mock = NativeMemoryShim;
    for (unsigned fault = 0; fault < 3; ++fault) {
        Mock::reset(); auto *mapper = new IOMapper;
        XeMemory::IOKitContext context; context.mapper = mapper;
        {
            Device d; const XeGgtt::Range range {Device::Base, d.ptes.size() * 4096ULL};
            CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &range, 1,
                makeIOKitPins(context), d.hardware()) == Status::Ok);
            const auto h = d.allocation(); const auto r = d.exported(h); auto b = d.backend();
            CHECK(b.retain(b.opaque, r, true));
            const auto pte = d.ptes[0]; const auto writes = d.writes;
            Mock::faults.syncContext = &d;
            switch (fault) {
            case 0: Mock::faults.afterSync = [](void *opaque, IOOptionBits) { Device::d(opaque).live = false; }; break;
            case 1: Mock::faults.afterSync = [](void *opaque, IOOptionBits) { Device::d(opaque).ptes[0] ^= 4096; }; break;
            default: Mock::faults.afterSync = [](void *, IOOptionBits) { Mock::faults.copyDescriptor = true; }; break;
            }
            // Actual production DMA synchronize returns success; only the
            // subsequent direct association/epoch/PTE check sees the fault.
            CHECK(!b.synchronize(b.opaque, r) && Mock::syncCalls == 1);
            View view; CHECK(d.regions->inspect(h, view) == Status::Ok);
            CHECK(view.state == State::Quarantined && view.firmwareReferences == 1 && view.ggttBackingHeld);
            CHECK(context.pinnedBytes == 8192 && d.regions->chargedBytes() == 8192 && d.writes == writes);
            CHECK(Mock::commandCompleteCalls == 0 && Mock::descriptorCompleteCalls == 0);
            CHECK(d.regions->retire(h) == Status::Busy);
            Mock::faults.afterSync = nullptr; Mock::faults.copyDescriptor = false;
            d.live = true; d.ptes[0] = pte; d.quiet = false;
            CHECK(!b.release(b.opaque, r) && context.pinnedBytes == 8192);
            d.quiet = true; CHECK(b.release(b.opaque, r));
            CHECK(d.regions->retire(h) == Status::Ok && context.pinnedBytes == 0);
            CHECK(d.regions->close() == Status::Ok);
        }
        CHECK(Mock::commandCompleteCalls == 1 && Mock::descriptorCompleteCalls == 1);
        CHECK(Mock::unsafeClearCalls == 0 && Mock::prematureDestroy == 0);
        mapper->release(); CHECK(Mock::objects.empty() && Mock::allocations.empty());
    }
}
static void actualIOKitInspectionMustNotOutlivePhysicalAdmission() {
    namespace Mock = NativeMemoryShim;
    Mock::reset(); auto *mapper = new IOMapper;
    XeMemory::IOKitContext context; context.mapper = mapper;
    {
        Device d; const XeGgtt::Range range {Device::Base, d.ptes.size() * 4096ULL};
        CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &range, 1,
            makeIOKitPins(context), d.hardware()) == Status::Ok);
        Mock::faults.inspectionContext = &d;
        Mock::faults.afterInspectionAddress = 0x801fff;
        Mock::faults.afterInspection = [](void *opaque) { Device::d(opaque).live = false; };
        Handle h; CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 8192, h) == Status::Quarantined);
        CHECK(Mock::inspectionCallbacks == 1 && d.writes == 0 && d.mmioWrites == 0);
        CHECK(context.pinnedBytes == 8192 && d.regions->chargedBytes() == 8192);
        CHECK(Mock::commandCompleteCalls == 0 && Mock::descriptorCompleteCalls == 0);
        CHECK(d.regions->close() == Status::Quarantined);
        CHECK(context.pinnedBytes == 8192 && d.releases == 0);
        Mock::faults.afterInspection = nullptr; d.live = true;
        CHECK(d.regions->retire(h) == Status::Ok && context.pinnedBytes == 0);
        CHECK(d.regions->close() == Status::Ok);
    }
    mapper->release(); CHECK(Mock::objects.empty() && Mock::allocations.empty());
}
static void actualIOKitBounceFailsBeforePublication() {
    namespace Mock = NativeMemoryShim;
    for (bool uncertainCleanup : {false, true}) {
        Mock::reset(); auto *mapper = new IOMapper;
        {
            XeMemory::IOKitContext context; context.mapper = mapper;
            Device d; const XeGgtt::Range range {Device::Base, d.ptes.size() * 4096ULL};
            CHECK(d.regions->initialize(Device::OwnerId, Device::Epoch, &range, 1,
                makeIOKitPins(context), d.hardware()) == Status::Ok);
            Mock::faults.copyDescriptor = true; Mock::faults.commandComplete = uncertainCleanup;
            Handle h;
            CHECK(d.regions->allocate(Device::OwnerId, Device::Epoch, 8192, h) ==
                (uncertainCleanup ? Status::Quarantined : Status::Invalid));
            CHECK(d.writes == 0 && d.mmioWrites == 0 && d.ptes[0] == 0 && d.ptes[1] == 0);
            CHECK(Mock::commandCompleteCalls == 1 && Mock::unsafeClearCalls == 0 && Mock::prematureDestroy == 0);
            if (!uncertainCleanup) {
                CHECK(h.slot == SIZE_MAX && context.pinnedBytes == 0 && d.regions->chargedBytes() == 0);
                CHECK(d.regions->allocations() == 0 && Mock::descriptorCompleteCalls == 1);
                CHECK(d.regions->close() == Status::Ok);
            } else {
                View view; CHECK(d.regions->inspect(h, view) == Status::Ok);
                CHECK(view.state == State::Quarantined && !view.ggttBackingHeld && view.firmwareReferences == 0);
                CHECK(context.pinnedBytes == 8192 && d.regions->chargedBytes() == 8192 && d.regions->allocations() == 1);
                CHECK(Mock::lastCommand && !Mock::lastCommand->active && Mock::lastCommand->getMemoryDescriptor());
                Mock::faults = {}; // A later OS success cannot recover lost cleanup authority.
                CHECK(d.regions->retire(h) == Status::Quarantined && d.regions->close() == Status::Quarantined);
                CHECK(context.pinnedBytes == 8192 && d.regions->chargedBytes() == 8192 && d.releases == 0);
                CHECK(Mock::commandCompleteCalls == 1 && Mock::descriptorCompleteCalls == 0);
            }
        }
        mapper->release();
        if (!uncertainCleanup) CHECK(Mock::objects.empty() && Mock::allocations.empty());
        else CHECK(!Mock::objects.empty() && !Mock::allocations.empty());
    }
    // Explicit host-world teardown after retained production owners/contexts
    // have left scope; this is not a production DMA retirement operation.
    Mock::disposeQuarantinedHostWorld();
    CHECK(Mock::objects.empty() && Mock::allocations.empty());
}
#endif
int main() {
    normalOwnership(); exactIdentityAndNoMutation(); inputAdmission(); pinAndCpuFailures();
    publicationInverseAndQuarantine(); freshReadbackAndEpochLoss();
    synchronizationAndUnpinUncertainty(); reverseCleanupAndSpaceLease(); loaderRejectsRealUnidentifiedImage();
    successfulSyncMustRevalidateAuthorityAndBacking();
    allocationBound();
#ifdef XE_REGION_OWNER_IOKIT_SHIM_TEST
    actualIOKitFactory();
    actualIOKitSyncRevalidation(); actualIOKitInspectionMustNotOutlivePhysicalAdmission();
    actualIOKitBounceFailsBeforePublication();
#endif
    std::printf("PASS xe_guc_region_owner_tests (%u checks; simulated hardware/DMA boundary)\n", checks);
}
