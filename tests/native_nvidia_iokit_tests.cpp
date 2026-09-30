// Compiles unchanged production adapter code against explicit fake OS types.
// Does not model PCI transactions, cache behavior, reset exclusion, or a GPU.
#include "native_nvidia_iokit_shim.hpp"
#include "../Mellow/NvidiaMmioIOKit.hpp"
#include <cstdio>
#include <cstdlib>
using namespace MellowNativeNvidia;
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (0)

struct Fixture {
    IOPCIDevice *device {new IOPCIDevice};
    IOService owner;
    OSArray *array {new OSArray};
    IOMemoryDescriptor *descriptor {new IOMemoryDescriptor};
    IOKitMmio adapter;
    PhysicalExpectation expected {{0x10de, 0x2204, 123, 7}, {1, 0, 0}, 0x172};
    Fixture() {
        owner.provider = device; device->openOwner = &owner;
        device->fixtureDescriptor = descriptor;
        device->fixture32(0, 0x220410de);
        device->fixture16(4, 2); device->fixture16(6, 0x10);
        device->fixture32(8, 0x030000a1); device->fixture8(0x0e, 0);
        device->fixture32(0x10, 0xa0000000); device->fixture32(0x14, 0);
        pm(0x60); array->add(descriptor); device->fixtureProperty(gIODeviceMemoryKey, array);
    }
    ~Fixture() { adapter.detach(); device->release(); array->release(); descriptor->release(); }
    void pm(uint8_t offset) {
        device->fixture8(0x34, offset); device->fixture32(offset, 0x00030001);
        if (offset <= 0xf8) device->fixture16(offset + 4, 0);
    }
    void bar64(uint64_t physical) {
        device->fixture32(0x10, static_cast<uint32_t>(physical) | 4U);
        device->fixture32(0x14, static_cast<uint32_t>(physical >> 32));
        descriptor->physical = physical;
        descriptor->tag = (kIOPCI64BitMemorySpace << 24) | (1U << 16) | 0x10;
    }
};
struct References {
    unsigned device, owner, array, descriptor, objects, maps;
    explicit References(const Fixture &f) : device(f.device->refs), owner(f.owner.refs),
        array(f.array->refs), descriptor(f.descriptor->refs),
        objects(NativeNvidiaShim::aliveObjects), maps(NativeNvidiaShim::aliveMaps) {}
    void unchanged(const Fixture &f) const {
        CHECK(f.device->refs == device && f.owner.refs == owner &&
            f.array->refs == array && f.descriptor->refs == descriptor);
        CHECK(NativeNvidiaShim::aliveObjects == objects && NativeNvidiaShim::aliveMaps == maps);
        CHECK(f.device->forbiddenCalls == 0);
    }
};
static void empty(const PhysicalProbe &probe) {
    CHECK(probe.boot0 == 0 && probe.boot1 == 0 && probe.physical.device.registryId == 0 &&
        probe.physical.device.epoch == 0 && probe.architecture == Architecture::Unknown);
}
static void failure(Fixture &f, ProbeStatus expected) {
    const References refs(f);
    CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == expected);
    CHECK(f.adapter.mappedLength() == 0);
    refs.unchanged(f);
    PhysicalProbe out {}; out.boot0 = 99; out.physical.device.epoch = 99;
    CHECK(f.adapter.sample(out) == ProbeStatus::Unavailable); empty(out);
    f.adapter.detach(); refs.unchanged(f);
}
static void success(Fixture &f) {
    const References refs(f);
    CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok);
    CHECK(f.adapter.mappedLength() == 16 && NativeNvidiaShim::aliveMaps == refs.maps + 1);
    CHECK(f.device->refs == refs.device + 1 && f.descriptor->refs == refs.descriptor + 2);
    CHECK(f.descriptor->lastMapOptions == kIOMapInhibitCache && f.device->forbiddenCalls == 0);
    PhysicalProbe out {};
    CHECK(f.adapter.sample(out) == ProbeStatus::Ok);
    CHECK(out.boot0 == 0x172000a1 && out.pci.pmcsr == 0 && out.physical.device.registryId == 123);
    CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ownership);
    f.adapter.detach(); CHECK(f.adapter.mappedLength() == 0); refs.unchanged(f);
    f.adapter.detach(); refs.unchanged(f);
}
static void liveFailure(Fixture &f, ProbeStatus expected = ProbeStatus::IoFailure) {
    PhysicalProbe out {}; out.boot0 = 99; out.physical.device.epoch = 99;
    CHECK(f.adapter.sample(out) == expected); empty(out);
    CHECK(NativeNvidiaShim::aliveMaps == 1); // Failure retains the map until explicit detach.
    f.adapter.detach();
    CHECK(f.device->refs == 1 && f.descriptor->refs == 2 && NativeNvidiaShim::aliveMaps == 0);
}

int main() {
    { Fixture f; success(f); }
    { Fixture f; f.bar64(0x100000000ULL); success(f); }
    { Fixture f; f.pm(0xf8); success(f); }
    { Fixture f; f.device->fixture8(0x34, 0x80); f.device->fixture32(0x80, 0x00006005); success(f); }
    { Fixture f; f.owner.provider = nullptr; failure(f, ProbeStatus::Ownership); CHECK(f.descriptor->mapCalls == 0); }
    { Fixture f; f.device->openOwner = nullptr; failure(f, ProbeStatus::Ownership); CHECK(f.descriptor->mapCalls == 0); }
    { Fixture f; f.owner.inactive = true; failure(f, ProbeStatus::Ownership); }
    { Fixture f; f.device->inactive = true; failure(f, ProbeStatus::Ownership); }
    { Fixture f; f.device->powerState = 1; failure(f, ProbeStatus::Unavailable); CHECK(f.descriptor->mapCalls == 0); }
    { Fixture f; auto *tunnel = new OSObject; f.device->fixtureProperty(kIOPCITunnelledKey, tunnel); tunnel->release(); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.device->fixture32(0, 0xffffffff); failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; ++f.device->registryId; failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; ++f.device->bus; failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; f.device->fixture16(4, 4); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.device->fixture8(0x0e, 1); failure(f, ProbeStatus::Invalid); }
    { Fixture f; f.device->fixture32(8, 0x040300a1); failure(f, ProbeStatus::Invalid); }
    for (uint32_t bar : {0U, 0xffffffffU, 0xa0000001U, 0xa0000002U, 0xa0000006U}) {
        Fixture f; f.device->fixture32(0x10, bar); failure(f, ProbeStatus::Unavailable); CHECK(f.descriptor->mapCalls == 0);
    }
    { Fixture f; f.device->fixtureProperty(gIODeviceMemoryKey, nullptr); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; auto *wrong = new OSObject; f.device->fixtureProperty(gIODeviceMemoryKey, wrong); wrong->release(); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.descriptor->tag = (2U << 24) | (1U << 16) | 0x14; failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.descriptor->tag ^= 1U << 16; failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.descriptor->tag ^= 1U << 24; failure(f, ProbeStatus::Unavailable); }
    { Fixture f; auto *duplicate = new IOMemoryDescriptor; f.array->add(duplicate); duplicate->release(); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; auto *irrelevant = new OSObject; f.array->add(irrelevant); irrelevant->release(); success(f); }
    { Fixture f; f.descriptor->physical += 0x1000; failure(f, ProbeStatus::Unavailable); CHECK(f.descriptor->mapCalls == 0); }
    { Fixture f; f.descriptor->length = 4; failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.descriptor->contiguous = 8; failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.descriptor->length = UINT64_MAX; failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.descriptor->failMap = true; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.descriptor->overrideVirtual = true; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.descriptor->overrideVirtual = true; f.descriptor->mapVirtual = reinterpret_cast<uintptr_t>(f.descriptor->registers) + 1; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.descriptor->overrideLength = true; f.descriptor->mapLength = 4; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.descriptor->overrideLength = true; f.descriptor->mapLength = 8; failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; f.descriptor->overridePhysical = true; f.descriptor->mapPhysical = 0xa0001000; failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; f.descriptor->registers[0] = 0; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.descriptor->registers[0] = 0xffffffff; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.descriptor->registers[0] = 0x174000a1; failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; f.descriptor->registers[1] = 0x01000001; failure(f, ProbeStatus::UnsupportedEndian); }
    { Fixture f; f.descriptor->registers[1] = 0x00010000; failure(f, ProbeStatus::VirtualDevice); }
    { Fixture f; f.device->fixture16(6, 0); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.device->fixture16(6, 0xffff); failure(f, ProbeStatus::Unavailable); }
    for (uint8_t start : {uint8_t(0), uint8_t(0x3c), uint8_t(0x61), uint8_t(0xff)}) {
        Fixture f; f.device->fixture8(0x34, start); failure(f, ProbeStatus::Unavailable);
    }
    { Fixture f; f.device->fixture32(0x60, 0xffffffff); failure(f, ProbeStatus::Unavailable); }
    { Fixture f; f.device->fixture32(0x60, 0x00006005); failure(f, ProbeStatus::Unavailable); CHECK(f.device->configReads < 100); }
    { Fixture f; f.device->fixture32(0x60, 0x00008005); f.device->fixture32(0x80, 0x00006005); failure(f, ProbeStatus::Unavailable); CHECK(f.device->configReads < 100); }
    { Fixture f; f.pm(0xfc); failure(f, ProbeStatus::Unavailable); }
    for (uint16_t pmcsr : {uint16_t(1), uint16_t(2), uint16_t(3), uint16_t(0xffff)}) {
        Fixture f; f.device->fixture16(0x64, pmcsr); failure(f, ProbeStatus::Unavailable);
    }
    { Fixture f; f.device->beforeIdRead = [](IOPCIDevice &d, unsigned n) { if (n == 3) d.powerState = 1; }; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.device->beforeIdRead = [](IOPCIDevice &d, unsigned n) { if (n == 4) d.powerState = 1; }; failure(f, ProbeStatus::IoFailure); }
    { Fixture f; f.device->beforeIdRead = [](IOPCIDevice &d, unsigned n) { if (n == 5) ++d.registryId; }; failure(f, ProbeStatus::IdentityChanged); }
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.device->fixture32(0x10, 0xa1000000); liveFailure(f); }
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.descriptor->lastMap->physical += 0x1000; liveFailure(f); }
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.descriptor->lastMap->length = 8; liveFailure(f); }
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.descriptor->physical += 0x1000; liveFailure(f); }
    // Regression: retaining a descriptor is not a promise that its in-place
    // length/tag stayed unchanged; reject a stale map before either BAR read.
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.descriptor->length = 4; liveFailure(f); }
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.descriptor->contiguous = 4; liveFailure(f); }
    { Fixture f; CHECK(f.adapter.attach(f.device, &f.owner, f.expected) == ProbeStatus::Ok); f.descriptor->tag ^= 1U << 16; liveFailure(f); }
    CHECK(NativeNvidiaShim::aliveObjects == 0 && NativeNvidiaShim::aliveMaps == 0);
    std::printf("PASS native NVIDIA IOKit boundary: %u checks; simulated OS only, no GPU execution\n", checks);
}
