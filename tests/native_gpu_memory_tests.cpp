// Host tests of the production owner using explicit modeled callbacks.
// No IOKit adapter, physical GPU, firmware or acceleration success is implied.
#include "../Drivers/NativeGpu/MemoryOwner.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace MellowNative;
namespace {
unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL native memory line %d: %s\n", __LINE__, #value); \
    std::abort(); } } while (false)
enum class Event { Pin, DeviceSync, CpuSync, Map, Retired, Unmap, Unpin, Stop };
enum class PinMode { Normal, CleanUnavailable, Partial, Lost, UnknownStatus, Device, Alignment, Width, Cpu, Alias };
enum class MapMode { Normal, CleanUnavailable, Unknown, Alignment, Width, Zero, Size, Access, Alias };
struct Block {
    alignas(4096) uint8_t cpu[8192] {};
    uint64_t pages[2] {};
    bool live {}, mapped {};
};
struct Fixture {
    Block blocks[MemoryOwner::MaxAllocations] {};
    Event events[512] {};
    uint32_t eventSlots[512] {};
    size_t count {};
    DeviceIdentity identity {0x8086, 0x7D41, 0x1234, 11};
    bool admitted {true}, retired {true}, stopped {true};
    PinMode pinMode {PinMode::Normal};
    MapMode mapMode {MapMode::Normal};
    unsigned failDeviceSync {}, failCpuSync {}, failUnmap {}, failUnpin {};
    void record(Event event, uint32_t slot = UINT32_MAX) {
        CHECK(count < 512); events[count] = event; eventSlots[count++] = slot;
    }
    size_t occurrences(Event event) const {
        size_t result = 0;
        for (size_t i = 0; i < count; ++i) if (events[i] == event) ++result;
        return result;
    }
    static Fixture &get(void *p) { return *static_cast<Fixture *>(p); }
    static bool admission(void *p, const DeviceIdentity &d) {
        auto &f = get(p);
        return f.admitted && d.vendor == f.identity.vendor && d.device == f.identity.device &&
            d.registryId == f.identity.registryId && d.epoch == f.identity.epoch;
    }
    static MemoryStatus pin(void *p, const DeviceIdentity &d, uint64_t,
                            const MemoryHandle &h, uint64_t bytes, DmaPin &out) {
        auto &f = get(p); f.record(Event::Pin, h.slot);
        if (f.pinMode == PinMode::CleanUnavailable) return MemoryStatus::Unavailable;
        auto &b = f.blocks[h.slot]; CHECK(!b.live); CHECK(bytes <= sizeof(b.cpu));
        b.live = true;
        if (f.pinMode == PinMode::Lost) return MemoryStatus::IoFailure;
        if (f.pinMode == PinMode::UnknownStatus) return static_cast<MemoryStatus>(255);
        b.pages[0] = 0x100000 + static_cast<uint64_t>(h.slot) * 8192;
        b.pages[1] = b.pages[0] + 4096;
        out = {&b, b.cpu, b.pages, static_cast<size_t>(bytes / 4096), bytes, d.registryId};
        if (f.pinMode == PinMode::Partial) return MemoryStatus::IoFailure;
        if (f.pinMode == PinMode::Device) ++out.deviceRegistryId;
        if (f.pinMode == PinMode::Alignment) ++b.pages[0];
        if (f.pinMode == PinMode::Width) b.pages[0] = 1ULL << 46;
        if (f.pinMode == PinMode::Cpu) out.cpu = nullptr;
        if (f.pinMode == PinMode::Alias) out.cpu = f.blocks[0].cpu;
        return MemoryStatus::Ok;
    }
    static MemoryStatus unpin(void *p, const DeviceIdentity &, uint64_t,
                              const MemoryHandle &h, DmaPin &pin) {
        auto &f = get(p); f.record(Event::Unpin, h.slot);
        auto &b = f.blocks[h.slot]; CHECK(b.live && !b.mapped);
        if (f.failUnpin) { --f.failUnpin; pin.cpu = nullptr; return MemoryStatus::IoFailure; }
        b.live = false; pin = {}; return MemoryStatus::Ok;
    }
    static MemoryStatus synchronize(void *p, const DeviceIdentity &, uint64_t,
                                    const MemoryHandle &h, const DmaPin &, CacheDirection d) {
        auto &f = get(p); CHECK(f.blocks[h.slot].live);
        f.record(d == CacheDirection::ForDevice ? Event::DeviceSync : Event::CpuSync, h.slot);
        auto &failure = d == CacheDirection::ForDevice ? f.failDeviceSync : f.failCpuSync;
        if (failure) { --failure; return MemoryStatus::IoFailure; }
        return MemoryStatus::Ok;
    }
    static MemoryStatus map(void *p, const DeviceIdentity &, uint64_t,
                            const MemoryHandle &h, const DmaPin &pin, bool writable, GpuMapping &out) {
        auto &f = get(p); f.record(Event::Map, h.slot);
        auto &b = f.blocks[h.slot]; CHECK(b.live && !b.mapped);
        if (f.mapMode == MapMode::CleanUnavailable) return MemoryStatus::Unavailable;
        b.mapped = true;
        if (f.mapMode == MapMode::Unknown) return MemoryStatus::IoFailure;
        out = {&b, 0x100000000ULL + static_cast<uint64_t>(h.slot) * 0x10000, pin.bytes, writable};
        if (f.mapMode == MapMode::Alignment) ++out.address;
        if (f.mapMode == MapMode::Width) out.address = (1ULL << 48) - 4096;
        if (f.mapMode == MapMode::Zero) out.address = 0;
        if (f.mapMode == MapMode::Size) out.bytes += 4096;
        if (f.mapMode == MapMode::Access) out.writable = !writable;
        if (f.mapMode == MapMode::Alias) out.address = 0x100000000ULL;
        return MemoryStatus::Ok;
    }
    static MemoryStatus unmap(void *p, const DeviceIdentity &, uint64_t,
                              const MemoryHandle &h, const DmaPin &, GpuMapping &mapping) {
        auto &f = get(p); f.record(Event::Unmap, h.slot);
        auto &b = f.blocks[h.slot]; CHECK(b.live && b.mapped);
        if (f.failUnmap) { --f.failUnmap; return MemoryStatus::IoFailure; }
        // Model completed GPU unpublication/TLB invalidation, then VA release.
        b.mapped = false; mapping = {}; return MemoryStatus::Ok;
    }
    static bool retirement(void *p, const MemoryView &v) {
        auto &f = get(p); f.record(Event::Retired, v.handle.slot); return f.retired;
    }
    static bool stop(void *p, const DeviceIdentity &) {
        auto &f = get(p); f.record(Event::Stop);
        if (f.stopped) f.admitted = false;
        return f.stopped;
    }
    MemoryBackend backend() {
        return {this, admission, pin, unpin, synchronize, map, unmap, retirement, stop};
    }
};
constexpr uint64_t Owner = 73;
MemoryLimits limits() { return {46, 48, 8192, 8192 * MemoryOwner::MaxAllocations}; }
void initialize(Fixture &f, MemoryOwner &m) {
    CHECK(m.initialize(f.identity, limits(), f.backend()) == MemoryStatus::Ok);
}
MemoryHandle allocate(MemoryOwner &m, uint64_t bytes = 4096) {
    MemoryHandle handle;
    CHECK(m.allocate(Owner, bytes, handle) == MemoryStatus::Ok); return handle;
}
MemoryHandle mapped(MemoryOwner &m, uint64_t bytes = 4096) {
    auto h = allocate(m, bytes); CHECK(m.map(Owner, h, true) == MemoryStatus::Ok); return h;
}

void admissionAndBounds() {
    Fixture f; MemoryOwner m;
    auto b = f.backend(); b.map = nullptr;
    CHECK(m.initialize(f.identity, limits(), b) == MemoryStatus::Unavailable);
    auto id = f.identity; id.registryId = 0;
    CHECK(m.initialize(id, limits(), f.backend()) == MemoryStatus::Invalid);
    id = f.identity; id.epoch = 0;
    CHECK(m.initialize(id, limits(), f.backend()) == MemoryStatus::Invalid);
    auto l = limits(); l.dmaAddressBits = 38;
    CHECK(m.initialize(f.identity, l, f.backend()) == MemoryStatus::Invalid);
    l = limits(); l.gpuAddressBits = 49;
    CHECK(m.initialize(f.identity, l, f.backend()) == MemoryStatus::Invalid);
    f.admitted = false;
    CHECK(m.initialize(f.identity, limits(), f.backend()) == MemoryStatus::Ownership);
    f.admitted = true; initialize(f, m);
    CHECK(m.initialize(f.identity, limits(), f.backend()) == MemoryStatus::Busy);
    MemoryHandle h;
    for (uint64_t size : {0ULL, 4095ULL, 12288ULL, ~0ULL})
        CHECK(m.allocate(Owner, size, h) == MemoryStatus::Invalid);
    CHECK(m.allocate(0, 4096, h) == MemoryStatus::Invalid);
    CHECK(f.count == 0 && !m.chargedBytes());
    CHECK(m.close() == MemoryStatus::Ok);
}
void ownershipAndHolds() {
    Fixture f; MemoryOwner m; initialize(f, m); auto h = mapped(m, 8192);
    MemoryView v;
    CHECK(m.inspect(Owner, h, v) == MemoryStatus::Ok);
    CHECK(v.pin.pages[1] == v.pin.pages[0] + 4096 && v.mapping.address != v.pin.pages[0]);
    CHECK(v.device.registryId == f.identity.registryId && v.state == MemoryState::Mapped);
    CHECK(m.retain(Owner + 1, h) == MemoryStatus::Ownership);
    auto wrong = h; ++wrong.registryId;
    CHECK(m.retain(Owner, wrong) == MemoryStatus::Ownership);
    wrong = h; ++wrong.epoch;
    CHECK(m.retain(Owner, wrong) == MemoryStatus::StaleEpoch);
    wrong = h; ++wrong.generation;
    CHECK(m.retain(Owner, wrong) == MemoryStatus::Ownership);
    CHECK(m.retain(Owner, h) == MemoryStatus::Ok);
    CHECK(m.retain(Owner, h) == MemoryStatus::Ok);
    const auto count = f.count;
    CHECK(m.retire(Owner, h) == MemoryStatus::Busy);
    CHECK(m.sync(Owner, h, CacheDirection::ForCpu) == MemoryStatus::Busy);
    CHECK(f.count == count);
    CHECK(m.release(Owner, h) == MemoryStatus::Ok && m.release(Owner, h) == MemoryStatus::Ok);
    CHECK(m.release(Owner, h) == MemoryStatus::Ownership);
    f.retired = false;
    CHECK(m.sync(Owner, h, CacheDirection::ForCpu) == MemoryStatus::Busy);
    CHECK(m.retire(Owner, h) == MemoryStatus::Busy);
    CHECK(m.retain(Owner, h) == MemoryStatus::Busy);
    f.retired = true; const auto start = f.count;
    CHECK(m.retire(Owner, h) == MemoryStatus::Ok);
    CHECK(f.events[start] == Event::Retired && f.events[start + 1] == Event::CpuSync &&
        f.events[start + 2] == Event::Unmap && f.events[start + 3] == Event::Unpin);
    auto next = allocate(m); CHECK(next.slot == h.slot && next.generation != h.generation);
    CHECK(m.inspect(Owner, h, v) == MemoryStatus::Ownership);
    CHECK(!v.pin.cookie && !v.mapping.address && v.state == MemoryState::Empty);
    CHECK(m.close() == MemoryStatus::Ok);
}
void retainedSynchronization() {
    Fixture f; MemoryOwner m; initialize(f, m); auto h = mapped(m);
    CHECK(m.synchronizeRetained(Owner, h, CacheDirection::ForCpu) == MemoryStatus::Busy);
    CHECK(m.retain(Owner, h) == MemoryStatus::Ok); f.retired = false;
    const auto retiredCount = f.occurrences(Event::Retired);
    CHECK(m.synchronizeRetained(Owner, h, CacheDirection::ForDevice) == MemoryStatus::Ok);
    CHECK(m.synchronizeRetained(Owner, h, CacheDirection::ForCpu) == MemoryStatus::Ok);
    CHECK(f.occurrences(Event::Retired) == retiredCount);
    MemoryView v; CHECK(m.inspect(Owner, h, v) == MemoryStatus::Ok && v.jobHolds == 1);
    CHECK(m.retire(Owner, h) == MemoryStatus::Busy);
    CHECK(!f.occurrences(Event::Unmap) && !f.occurrences(Event::Unpin));
    CHECK(m.synchronizeRetained(Owner, h, static_cast<CacheDirection>(9)) == MemoryStatus::Invalid);
    CHECK(m.release(Owner, h) == MemoryStatus::Ok); f.retired = true;
    const auto cpuSyncs = f.occurrences(Event::CpuSync);
    CHECK(m.retire(Owner, h) == MemoryStatus::Ok);
    CHECK(f.occurrences(Event::CpuSync) == cpuSyncs + 1);
    CHECK(m.close() == MemoryStatus::Ok);

    Fixture failed; MemoryOwner failOwner; initialize(failed, failOwner); h = mapped(failOwner);
    CHECK(failOwner.retain(Owner, h) == MemoryStatus::Ok); failed.failCpuSync = 1;
    CHECK(failOwner.synchronizeRetained(Owner, h, CacheDirection::ForCpu) == MemoryStatus::Quarantined);
    CHECK(failOwner.inspect(Owner, h, v) == MemoryStatus::Ok && v.jobHolds == 1);
    CHECK(failOwner.chargedBytes() == 4096 && !failed.occurrences(Event::Unmap));
    CHECK(failOwner.close() == MemoryStatus::Busy);
    CHECK(failOwner.release(Owner, h) == MemoryStatus::Ok && failOwner.close() == MemoryStatus::Ok);
}
void capacityAndCleanupOrder() {
    Fixture f; MemoryOwner m; initialize(f, m);
    MemoryHandle handles[MemoryOwner::MaxAllocations];
    for (auto &h : handles) h = allocate(m, 8192);
    MemoryHandle out;
    CHECK(m.allocate(Owner, 4096, out) == MemoryStatus::Capacity);
    CHECK(m.allocations() == MemoryOwner::MaxAllocations && m.chargedBytes() == limits().maxTotalBytes);
    CHECK(m.retire(Owner, handles[0]) == MemoryStatus::Ok);
    out = allocate(m, 8192); CHECK(out.slot == 0 && out.generation > handles[31].generation);
    const auto start = f.count; CHECK(m.close() == MemoryStatus::Ok);
    CHECK(f.events[start] == Event::Stop && f.events[start + 1] == Event::Unpin &&
        f.eventSlots[start + 1] == 0 && f.eventSlots[start + 2] == 31);
    CHECK(!m.allocations() && !m.chargedBytes() && m.close() == MemoryStatus::Ok);
    CHECK(m.allocate(Owner, 4096, out) == MemoryStatus::Quarantined);
}
void pinFailures() {
    Fixture clean; MemoryOwner cm; initialize(clean, cm); clean.pinMode = PinMode::CleanUnavailable;
    MemoryHandle h; CHECK(cm.allocate(Owner, 4096, h) == MemoryStatus::Unavailable);
    CHECK(h.slot == UINT32_MAX && !cm.chargedBytes() && !cm.allocations());
    CHECK(cm.close() == MemoryStatus::Ok);
    for (auto mode : {PinMode::Partial, PinMode::Lost, PinMode::UnknownStatus, PinMode::Device, PinMode::Alignment,
                       PinMode::Width, PinMode::Cpu}) {
        Fixture f; MemoryOwner m; initialize(f, m); f.pinMode = mode;
        CHECK(m.allocate(Owner, 4096, h) == MemoryStatus::Quarantined);
        CHECK(h.slot != UINT32_MAX && m.chargedBytes() == 4096 && m.draining());
        CHECK(!f.occurrences(Event::Unpin) && !f.occurrences(Event::Map));
        CHECK(m.retire(Owner, h) == MemoryStatus::Ok);
        CHECK(!m.chargedBytes() && m.close() == MemoryStatus::Ok);
    }
    Fixture alias; MemoryOwner am; initialize(alias, am); allocate(am);
    alias.pinMode = PinMode::Alias;
    CHECK(am.allocate(Owner, 4096, h) == MemoryStatus::Quarantined);
    CHECK(am.close() == MemoryStatus::Ok);
}
void mappingFailures() {
    Fixture clean; MemoryOwner cm; initialize(clean, cm); auto h = allocate(cm);
    clean.mapMode = MapMode::CleanUnavailable;
    CHECK(cm.map(Owner, h, true) == MemoryStatus::Unavailable);
    CHECK(cm.retire(Owner, h) == MemoryStatus::Ok && !clean.occurrences(Event::Unmap));
    CHECK(cm.close() == MemoryStatus::Ok);
    for (auto mode : {MapMode::Unknown, MapMode::Alignment, MapMode::Width, MapMode::Zero,
                       MapMode::Size, MapMode::Access}) {
        Fixture f; MemoryOwner m; initialize(f, m); h = allocate(m, 8192); f.mapMode = mode;
        CHECK(m.map(Owner, h, true) == MemoryStatus::Quarantined);
        CHECK(m.retire(Owner, h) == MemoryStatus::Quarantined);
        CHECK(m.chargedBytes() == 8192 && !f.occurrences(Event::Unmap) && !f.occurrences(Event::Unpin));
        CHECK(m.close() == MemoryStatus::Ok);
        CHECK(f.occurrences(Event::Unmap) == 1 && f.occurrences(Event::Unpin) == 1);
    }
    Fixture alias; MemoryOwner am; initialize(alias, am); mapped(am); h = allocate(am);
    alias.mapMode = MapMode::Alias;
    CHECK(am.map(Owner, h, true) == MemoryStatus::Quarantined && am.close() == MemoryStatus::Ok);
}
void cleanupFailures() {
    Fixture sync; MemoryOwner sm; initialize(sync, sm); auto h = mapped(sm); sync.failCpuSync = 1;
    CHECK(sm.retire(Owner, h) == MemoryStatus::Quarantined);
    CHECK(sm.chargedBytes() == 4096 && !sync.occurrences(Event::Unmap));
    CHECK(sm.close() == MemoryStatus::Ok && sync.occurrences(Event::CpuSync) == 2);
    Fixture unmap; MemoryOwner um; initialize(unmap, um); h = mapped(um); unmap.failUnmap = 1;
    CHECK(um.retire(Owner, h) == MemoryStatus::Quarantined);
    CHECK(!unmap.occurrences(Event::Unpin) && um.chargedBytes() == 4096);
    CHECK(um.retire(Owner, h) == MemoryStatus::Quarantined);
    CHECK(um.close() == MemoryStatus::Ok && unmap.occurrences(Event::Unmap) == 2 &&
        unmap.occurrences(Event::CpuSync) == 2);
    Fixture unpin; MemoryOwner pm; initialize(unpin, pm); h = mapped(pm); unpin.failUnpin = 1;
    CHECK(pm.retire(Owner, h) == MemoryStatus::Quarantined && pm.chargedBytes() == 4096);
    CHECK(pm.retire(Owner, h) == MemoryStatus::Ok && !pm.chargedBytes());
    CHECK(unpin.occurrences(Event::Unmap) == 1 && unpin.occurrences(Event::Unpin) == 2);
    CHECK(pm.close() == MemoryStatus::Ok);
    Fixture device; MemoryOwner dm; initialize(device, dm); h = allocate(dm); device.failDeviceSync = 1;
    CHECK(dm.map(Owner, h, true) == MemoryStatus::Quarantined);
    CHECK(!device.occurrences(Event::Map) && dm.close() == MemoryStatus::Ok);
}
void failedStopAndExplicitReferences() {
    Fixture f; MemoryOwner m; initialize(f, m); auto h = mapped(m);
    CHECK(m.retain(Owner, h) == MemoryStatus::Ok); f.stopped = false;
    CHECK(m.quiesce() == MemoryStatus::Quarantined && m.close() == MemoryStatus::Quarantined);
    CHECK(!f.occurrences(Event::Unmap) && !f.occurrences(Event::Unpin));
    f.stopped = true;
    CHECK(m.quiesce() == MemoryStatus::Ok && m.quiesced());
    CHECK(m.close() == MemoryStatus::Busy && m.chargedBytes() == 4096);
    CHECK(m.retain(Owner, h) == MemoryStatus::Quarantined);
    CHECK(m.synchronizeRetained(Owner, h, CacheDirection::ForCpu) == MemoryStatus::Quarantined);
    MemoryView v; CHECK(m.inspect(Owner, h, v) == MemoryStatus::Ok && v.jobHolds == 1);
    CHECK(m.release(Owner, h) == MemoryStatus::Ok && m.close() == MemoryStatus::Ok);
    CHECK(!m.chargedBytes());
}
}
int main() {
    admissionAndBounds(); ownershipAndHolds(); retainedSynchronization(); capacityAndCleanupOrder();
    pinFailures(); mappingFailures(); cleanupFailures(); failedStopAndExplicitReferences();
    std::printf("PASS native memory owner (%u checks; modeled callbacks, no physical GPU)\n", checks);
}
