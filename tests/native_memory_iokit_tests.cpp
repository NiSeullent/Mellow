// Unchanged NativeMemoryIOKit.cpp + MemoryOwner.cpp, explicit fake OS boundary.
// Host allocation cleanup at the end is not production quarantine recovery.
#include "native_memory_iokit_shim.hpp"
#include "../Mellow/NativeMemoryIOKit.hpp"
#include <cstdio>
#include <cstdlib>
using namespace MellowNative;
namespace Mock = NativeMemoryShim;
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (0)
struct Vm {
    IOPCIDevice *device {};
    IOService *owner {};
    DeviceIdentity identity {};
    bool admitted {true}, coherent {true}, retired {true}, quiesced {true};
    MemoryStatus mapStatus {MemoryStatus::Ok}, unmapStatus {MemoryStatus::Ok};
    bool partialMap {};
    unsigned maps {}, unmaps {}, stops {}, coherenceChecks {};
    static bool admission(void *p, IOPCIDevice *device, IOService *owner, const DeviceIdentity &id) {
        auto &v = *static_cast<Vm *>(p);
        return v.admitted && device == v.device && owner == v.owner &&
            id.vendor == v.identity.vendor && id.device == v.identity.device &&
            id.registryId == v.identity.registryId && id.epoch == v.identity.epoch;
    }
    static bool system(void *p, const DeviceIdentity &, const DmaPin &pin) {
        auto &v = *static_cast<Vm *>(p); ++v.coherenceChecks;
        return v.coherent && pin.cookie && pin.cpu && pin.pages && pin.pageCount;
    }
    static MemoryStatus map(void *p, const DeviceIdentity &, uint64_t owner,
            const MemoryHandle &handle, const DmaPin &pin, bool writable, GpuMapping &out) {
        auto &v = *static_cast<Vm *>(p); ++v.maps;
        CHECK(owner == 42 && pin.bytes != 0);
        if (v.mapStatus == MemoryStatus::Ok || v.partialMap)
            out = {reinterpret_cast<void *>(uintptr_t(handle.slot + 1)),
                0x1000000 + uint64_t(handle.slot) * 0x10000, pin.bytes, writable};
        return v.mapStatus;
    }
    static MemoryStatus unmap(void *p, const DeviceIdentity &, uint64_t owner,
            const MemoryHandle &, const DmaPin &, GpuMapping &) {
        auto &v = *static_cast<Vm *>(p); ++v.unmaps; CHECK(owner == 42); return v.unmapStatus;
    }
    static bool retirement(void *p, const MemoryView &) { return static_cast<Vm *>(p)->retired; }
    static bool stop(void *p, const DeviceIdentity &) { auto &v = *static_cast<Vm *>(p); ++v.stops; return v.quiesced; }
    PhysicalVmOps ops() { return {this, admission, system, map, unmap, retirement, stop}; }
};
struct Fixture {
    IOPCIDevice *device {new IOPCIDevice};
    IOService *owner {new IOService};
    IOMapper *mapper {new IOMapper};
    DeviceIdentity identity {0x8086, 0x7d41, 123, 7};
    MemoryLimits limits {46, 48, 8192, 16384};
    Vm vm;
    NativeMemoryIOKit adapter;
    MemoryBackend backend {};
    bool quarantine {};
    Fixture() {
        Mock::reset(); owner->provider = device; device->openOwner = owner;
        device->fixtureProperty("iommu-parent", mapper);
        vm.device = device; vm.owner = owner; vm.identity = identity;
    }
    ~Fixture() {
        const auto status = adapter.detach();
        CHECK(status == (quarantine ? MemoryStatus::Busy : MemoryStatus::Ok));
        CHECK(Mock::unsafeClearCalls == 0 && Mock::forbiddenMapperLookups == 0 && Mock::prematureDestroy == 0);
        device->release(); owner->release(); mapper->release();
    }
    void attach() {
        CHECK(adapter.attach(device, owner, identity, limits, vm.ops()) == MemoryStatus::Ok);
        CHECK(device->refs == 2 && owner->refs == 2 && mapper->refs == 3);
        backend = adapter.backend();
    }
    MemoryHandle handle(uint32_t slot = 0) const { return {slot, 77 + slot, 123, 7}; }
    MemoryStatus pin(DmaPin &out, uint32_t slot = 0, uint64_t bytes = 8192) {
        return backend.pin(backend.context, identity, 42, handle(slot), bytes, out);
    }
    MemoryStatus unpin(DmaPin &pin, uint32_t slot = 0) {
        return backend.unpin(backend.context, identity, 42, handle(slot), pin);
    }
};
static void cleanPinFailure(Fixture &f) {
    f.attach(); DmaPin pin {};
    CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie != nullptr);
    CHECK(f.adapter.pinnedBytes() == 8192 && f.adapter.detach() == MemoryStatus::Busy);
    CHECK(f.unpin(pin) == MemoryStatus::Ok && pin.cookie == nullptr && f.adapter.pinnedBytes() == 0);
    CHECK(f.device->refs == 2 && f.owner->refs == 2 && f.mapper->refs == 3);
}
static void retainUnknown(Fixture &f, DmaPin &pin, bool descriptorFailure = false) {
    const auto cookie = pin.cookie;
    CHECK(f.unpin(pin) == MemoryStatus::IoFailure);
    CHECK(pin.cookie == cookie && f.adapter.pinnedBytes() == 8192);
    const auto commandCalls = Mock::commandCompleteCalls, descriptorCalls = Mock::descriptorCompleteCalls;
    Mock::faults.commandComplete = false; Mock::faults.descriptorComplete = false;
    CHECK(f.unpin(pin) == MemoryStatus::Quarantined);
    CHECK(pin.cookie == cookie && f.adapter.pinnedBytes() == 8192);
    CHECK(Mock::commandCompleteCalls == commandCalls && Mock::descriptorCompleteCalls == descriptorCalls);
    const auto syncCalls = Mock::syncCalls;
    CHECK(f.backend.synchronize(f.backend.context, f.identity, 42, f.handle(), pin, CacheDirection::ForCpu) == MemoryStatus::Ownership);
    GpuMapping mapping {};
    CHECK(f.backend.map(f.backend.context, f.identity, 42, f.handle(), pin, true, mapping) == MemoryStatus::Ownership);
    CHECK(Mock::syncCalls == syncCalls && f.vm.maps == 0 && mapping.cookie == nullptr);
    CHECK(f.adapter.detach() == MemoryStatus::Busy);
    if (!descriptorFailure) CHECK(Mock::lastCommand && Mock::lastBuffer);
    else CHECK(!Mock::lastCommand && Mock::lastBuffer);
    f.quarantine = true;
}
int main() {
    { Fixture f; f.attach(); DmaPin pin {};
      CHECK(f.pin(pin) == MemoryStatus::Ok && pin.pageCount == 2 && pin.pages[0] == 0x800000 && pin.pages[1] == 0x801000);
      CHECK(pin.deviceRegistryId == 123 && pin.bytes == 8192 && f.vm.coherenceChecks == 1);
      for (size_t i = 0; i < pin.bytes; ++i) CHECK(pin.cpu[i] == 0);
      CHECK(f.adapter.detach() == MemoryStatus::Busy);
      CHECK(f.backend.synchronize(f.backend.context, f.identity, 42, f.handle(), pin, CacheDirection::ForDevice) == MemoryStatus::Ok);
      CHECK(f.backend.synchronize(f.backend.context, f.identity, 42, f.handle(), pin, CacheDirection::ForCpu) == MemoryStatus::Ok);
      CHECK(Mock::ordered("sync.out", "sync.in")); CHECK(f.unpin(pin) == MemoryStatus::Ok);
      CHECK(Mock::ordered("dma.complete", "dma.clear") && Mock::ordered("dma.clear", "dma.destroy") &&
          Mock::ordered("dma.destroy", "memory.complete") && Mock::ordered("memory.complete", "memory.destroy")); }
    for (unsigned missing = 0; missing < 7; ++missing) {
        Fixture f; auto ops = f.vm.ops();
        switch (missing) {
            case 0: ops.context = nullptr; break; case 1: ops.admitted = nullptr; break;
            case 2: ops.coherentSystem = nullptr; break; case 3: ops.map = nullptr; break;
            case 4: ops.unmap = nullptr; break; case 5: ops.retired = nullptr; break;
            case 6: ops.quiesce = nullptr; break;
        }
        CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, ops) == MemoryStatus::Unavailable);
        CHECK(!f.adapter.attached() && f.device->refs == 1 && f.owner->refs == 1 && f.mapper->refs == 2);
    }
    { Fixture f; f.vm.admitted = false;
      CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Ownership);
      CHECK(f.device->configReads == 0 && f.device->refs == 1 && f.owner->refs == 1 && f.mapper->refs == 2); }
    { Fixture f; f.device->fixtureProperty("iommu-parent", nullptr); CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Unavailable); }
    { Fixture f; auto *numeric = new OSObject; f.device->fixtureProperty("iommu-parent", numeric); numeric->release(); CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Unavailable); }
    { Fixture f; f.mapper->inactive = true; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Unavailable); }
    { Fixture f; f.owner->provider = nullptr; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Ownership); }
    { Fixture f; f.device->openOwner = nullptr; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Ownership); }
    { Fixture f; f.device->physicalId = 0xffffffff; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Ownership); }
    { Fixture f; ++f.device->registryId; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Ownership); }
    { Fixture f; f.limits.dmaAddressBits = 38; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops()) == MemoryStatus::Invalid); }
    { Fixture f; f.attach(); f.vm.admitted = false; DmaPin pin {};
      const auto reads = f.device->configReads; CHECK(f.pin(pin) == MemoryStatus::Ownership && !pin.cookie); CHECK(f.device->configReads == reads); }
    { Fixture f; f.attach(); auto *different = new IOMapper; f.device->fixtureProperty("iommu-parent", different); different->release(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ownership && !pin.cookie); }
    { Fixture f; f.attach(); DmaPin pin {}; Mock::faults.mallocFailureCall = 1; CHECK(f.pin(pin) == MemoryStatus::Capacity && !pin.cookie && f.adapter.pinnedBytes() == 0); }
    { Fixture f; Mock::faults.mallocFailureCall = 2; cleanPinFailure(f); }
    { Fixture f; Mock::faults.bufferFactory = true; cleanPinFailure(f); }
    { Fixture f; Mock::faults.noCpu = true; cleanPinFailure(f); }
    { Fixture f; Mock::faults.descriptorPrepare = true; cleanPinFailure(f); }
    { Fixture f; Mock::faults.commandFactory = true; cleanPinFailure(f); }
    { Fixture f; Mock::faults.setDescriptor = true; cleanPinFailure(f); }
    { Fixture f; Mock::faults.setDescriptor = true; Mock::faults.setDescriptorAcquires = false; cleanPinFailure(f); }
    { Fixture f; Mock::faults.commandPrepare = true; cleanPinFailure(f); }
    for (unsigned fault = 0; fault < 9; ++fault) {
        Fixture f;
        switch (fault) {
            case 0: Mock::faults.segmentError = 1; break; case 1: Mock::faults.segmentCount = 1; break;
            case 2: Mock::faults.segmentLength = 1; break; case 3: Mock::faults.segmentOffset = 1; break;
            case 4: Mock::faults.segmentAlignment = 1; break; case 5: Mock::faults.segmentWidth = 1; break;
            case 6: Mock::faults.physicalShort = 1; break; case 7: Mock::faults.translatedBounce = 1; break;
            case 8: Mock::faults.copyDescriptor = true; break;
        }
        cleanPinFailure(f);
    }
    { Fixture f; f.vm.coherent = false; cleanPinFailure(f); }
    { Fixture f; f.attach(); DmaPin pin {}, second {}, third {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      CHECK(f.pin(second) == MemoryStatus::Busy && !second.cookie);
      CHECK(f.pin(second, 31) == MemoryStatus::Ok && f.adapter.pinnedBytes() == 16384);
      CHECK(f.pin(third, 1) == MemoryStatus::Capacity && !third.cookie);
      CHECK(f.pin(third, 32) == MemoryStatus::Invalid);
      CHECK(f.unpin(second, 31) == MemoryStatus::Ok && f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      auto bad = f.handle(); ++bad.generation;
      CHECK(f.backend.unpin(f.backend.context, f.identity, 42, bad, pin) == MemoryStatus::Ownership);
      CHECK(f.backend.unpin(f.backend.context, f.identity, 43, f.handle(), pin) == MemoryStatus::Ownership);
      DmaPin forged = pin; forged.cookie = reinterpret_cast<void *>(uintptr_t(1));
      CHECK(f.backend.unpin(f.backend.context, f.identity, 42, f.handle(), forged) == MemoryStatus::Ownership);
      forged = pin; ++forged.pageCount;
      CHECK(f.backend.synchronize(f.backend.context, f.identity, 42, f.handle(), forged, CacheDirection::ForCpu) == MemoryStatus::Ownership);
      CHECK(f.backend.synchronize(f.backend.context, f.identity, 42, f.handle(), pin, static_cast<CacheDirection>(99)) == MemoryStatus::Invalid);
      Mock::faults.synchronize = true;
      CHECK(f.backend.synchronize(f.backend.context, f.identity, 42, f.handle(), pin, CacheDirection::ForCpu) == MemoryStatus::IoFailure);
      Mock::faults.synchronize = false; CHECK(f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f; f.attach(); MemoryOwner memory; MemoryHandle handle {};
      CHECK(memory.initialize(f.identity, f.limits, f.backend) == MemoryStatus::Ok);
      CHECK(memory.allocate(42, 8192, handle) == MemoryStatus::Ok && memory.map(42, handle, true) == MemoryStatus::Ok);
      CHECK(memory.retain(42, handle) == MemoryStatus::Ok && memory.synchronizeRetained(42, handle, CacheDirection::ForCpu) == MemoryStatus::Ok);
      CHECK(memory.retire(42, handle) == MemoryStatus::Busy && memory.close() == MemoryStatus::Busy);
      CHECK(f.vm.stops == 1 && f.adapter.pinnedBytes() == 8192);
      CHECK(memory.release(42, handle) == MemoryStatus::Ok && memory.close() == MemoryStatus::Ok);
      CHECK(f.vm.unmaps == 1 && f.adapter.pinnedBytes() == 0 && memory.allocations() == 0); }
    { Fixture f; f.attach(); MemoryOwner memory; MemoryHandle handle {};
      CHECK(memory.initialize(f.identity, f.limits, f.backend) == MemoryStatus::Ok);
      CHECK(memory.allocate(42, 8192, handle) == MemoryStatus::Ok);
      Mock::faults.syncContext = &f.vm;
      Mock::faults.afterSync = [](void *context, IOOptionBits direction) {
          if (direction == kIODirectionOut) static_cast<Vm *>(context)->admitted = false;
      };
      // Physical admission disappears after device sync but before native map.
      // The portable owner must quarantine until actual quiescence; the native
      // adapter has recorded that this allocation was never published to VM.
      CHECK(memory.map(42, handle, true) == MemoryStatus::Quarantined);
      MemoryView view {};
      CHECK(memory.inspect(42, handle, view) == MemoryStatus::Ok && view.state == MemoryState::Quarantined);
      CHECK(f.vm.maps == 0 && f.vm.unmaps == 0 && f.adapter.pinnedBytes() == 8192 && memory.allocations() == 1);
      CHECK(memory.retire(42, handle) == MemoryStatus::Quarantined && f.adapter.pinnedBytes() == 8192);
      CHECK(memory.close() == MemoryStatus::Ok);
      CHECK(f.vm.stops == 1 && f.vm.maps == 0 && f.vm.unmaps == 0);
      CHECK(f.adapter.pinnedBytes() == 0 && memory.allocations() == 0);
      CHECK(Mock::ordered("sync.out", "sync.in") && Mock::ordered("sync.in", "dma.complete") &&
          Mock::ordered("dma.complete", "dma.clear") && Mock::ordered("dma.clear", "memory.complete")); }
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      GpuMapping forged {reinterpret_cast<void *>(uintptr_t(1)), 0x1000000, 8192, true};
      CHECK(f.backend.unmap(f.backend.context, f.identity, 42, f.handle(), pin, forged) == MemoryStatus::Ownership);
      CHECK(f.vm.unmaps == 0 && forged.cookie != nullptr && f.adapter.pinnedBytes() == 8192);
      GpuMapping empty {};
      CHECK(f.backend.unmap(f.backend.context, f.identity, 42, f.handle(), pin, empty) == MemoryStatus::Ok);
      CHECK(f.vm.unmaps == 0 && f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok); GpuMapping mapping {};
      f.vm.mapStatus = MemoryStatus::Unavailable;
      CHECK(f.backend.map(f.backend.context, f.identity, 42, f.handle(), pin, true, mapping) == MemoryStatus::Unavailable);
      CHECK(f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok); GpuMapping mapping {};
      f.vm.mapStatus = MemoryStatus::IoFailure; f.vm.partialMap = true;
      CHECK(f.backend.map(f.backend.context, f.identity, 42, f.handle(), pin, true, mapping) == MemoryStatus::IoFailure);
      CHECK(f.unpin(pin) == MemoryStatus::Busy && f.adapter.pinnedBytes() == 8192);
      f.vm.unmapStatus = MemoryStatus::IoFailure;
      CHECK(f.backend.unmap(f.backend.context, f.identity, 42, f.handle(), pin, mapping) == MemoryStatus::IoFailure);
      CHECK(f.unpin(pin) == MemoryStatus::Busy);
      f.vm.unmapStatus = MemoryStatus::Ok;
      CHECK(f.backend.unmap(f.backend.context, f.identity, 42, f.handle(), pin, mapping) == MemoryStatus::Ok && !mapping.cookie);
      CHECK(f.unpin(pin) == MemoryStatus::Ok); }
    CHECK(Mock::objects.empty() && Mock::allocations.empty());
    // Permanent quarantine cases run after normal leak-free lifetime cases.
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      Mock::faults.commandComplete = true; retainUnknown(f, pin); }
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      Mock::faults.descriptorComplete = true; retainUnknown(f, pin, true); }
    { Fixture f; f.attach(); DmaPin pin {}; Mock::faults.commandPrepare = true; Mock::faults.commandPrepareAcquires = false;
      CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie); retainUnknown(f, pin); }
    { Fixture f; f.attach(); DmaPin pin {}; Mock::faults.commandPrepare = true;
      CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie); Mock::faults.commandComplete = true; retainUnknown(f, pin); }
    CHECK(!Mock::objects.empty() && !Mock::allocations.empty());
    // Release only simulated host allocations AFTER production objects have
    // ceased to exist; this is neither adapter recovery nor IOMMU retirement.
    Mock::disposeQuarantinedHostWorld();
    CHECK(Mock::objects.empty() && Mock::allocations.empty());
    std::printf("PASS native memory IOKit boundary: %u checks; simulated OS, no physical DMA/GPU\n", checks);
}
