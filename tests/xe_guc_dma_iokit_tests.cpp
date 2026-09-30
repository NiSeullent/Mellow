// Production firmware/DMA binding and ForceWake, with explicit simulated OS
// and MMIO. No physical DMA, firmware authentication, GuC or GPU is executed.
#include "native_memory_iokit_shim.hpp"
#include "../Mellow/XeGuCFirmwareIOKit.hpp"
#include <cstdio>
#include <cstdlib>

namespace Mock = NativeMemoryShim;
namespace G = XeGuCFirmware;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

// BAR attachment is outside this test. The real ForceWake method bodies use
// the explicit register simulator below; no OS map or readiness is inferred.
MellowXe::MmioAccess MellowXe::IOKitMmio::access() { return {}; }
struct WakeRegisters {
    uint32_t control {}, ack {};
    uint64_t clock {100};
    static bool read(void *p, uint32_t reg, uint32_t &out) {
        auto &s = *static_cast<WakeRegisters *>(p);
        if (reg == 0xd8c) out = (12U << 22) | (70U << 14);
        else if (reg == 0xa188) out = s.control;
        else if (reg == 0xdfc) out = s.ack;
        else return false;
        return true;
    }
    static bool write(void *p, uint32_t reg, uint32_t value) {
        auto &s = *static_cast<WakeRegisters *>(p);
        CHECK(reg == 0xa188 && (value == 0x10001 || value == 0x10000));
        s.control = s.ack = value & 1U; return true;
    }
    static uint64_t now(void *p) { return static_cast<WakeRegisters *>(p)->clock; }
    static void delay(void *p, uint32_t n) { static_cast<WakeRegisters *>(p)->clock += n; }
    MellowXe::MmioAccess access() { return {this, read, write, now, delay}; }
};
struct Proofs {
    bool admitted {true}, quiet {true}, published {true}, revokeOnPublication {};
    unsigned holds {}, retainCalls {}, publicationCalls {};
    static bool owns(void *p, uint64_t owner, uint64_t epoch) {
        return owner == 42 && epoch == 11 && static_cast<Proofs *>(p)->admitted;
    }
    static bool quiesced(void *p, uint64_t owner, uint64_t epoch) {
        return owner == 42 && epoch == 11 && static_cast<Proofs *>(p)->quiet;
    }
    static bool retain(void *p, const G::Region &r, bool) {
        auto &s = *static_cast<Proofs *>(p); ++s.retainCalls;
        if (r.owner != 42 || s.holds) return false;
        ++s.holds; return true;
    }
    static bool release(void *p, const G::Region &r) {
        auto &s = *static_cast<Proofs *>(p);
        if (r.owner != 42 || !s.holds || !s.quiet) return false;
        --s.holds; return true;
    }
    static bool mapping(void *p, const G::Region &, uint64_t epoch) {
        auto &s = *static_cast<Proofs *>(p); ++s.publicationCalls;
        const bool result = epoch == 11 && s.holds && s.published;
        if (s.revokeOnPublication) s.admitted = false;
        return result;
    }
    G::IOKitProofs ops() {
        G::IOKitProofs out {}; out.opaque = this; out.ownsEpoch = owns;
        out.quiesced = quiesced; out.retainGgtt = retain; out.releaseGgtt = release;
        out.mappingPublished = mapping; return out;
    }
};
struct Fixture {
    IOPCIDevice *pci {new IOPCIDevice};
    IOMapper *mapper {new IOMapper};
    XeMemory::IOKitContext pins;
    XeMemory::Backend memory {};
    XeMemory::Pin pin {};
    G::Region region {};
    WakeRegisters registers;
    MellowXe::IOKitMmio mmio;
    Proofs proofs;
    Fixture() {
        Mock::reset(); pins.mapper = mapper; pci->fixtureProperty("iommu-parent", mapper);
        memory = XeMemory::makeIOKitPinBackend(pins);
        CHECK(memory.pin(memory.context, 42, 8192, pin) == XeMemory::Status::Ok);
        auto *cpu = static_cast<uint8_t *>(XeMemory::resolveDirectPinnedBuffer(pins, 42, 8192, pin));
        CHECK(cpu); region = {42, 99, 0x1000000, 8192, cpu, pin.dmaPages, pin.pageCount, pin.cookie};
        CHECK(mmio.forceWake().initialize(registers.access()) == MellowXe::MmioStatus::Ok);
        CHECK(mmio.forceWake().acquire(MellowXe::WakeDomain::Gt) == MellowXe::MmioStatus::Ok);
    }
    ~Fixture() {
        CHECK(proofs.holds == 0);
        CHECK(memory.unpin(memory.context, pin) == XeMemory::Status::Ok && !pin.cookie);
        CHECK(mmio.forceWake().release(MellowXe::WakeDomain::Gt) == MellowXe::MmioStatus::Ok);
        CHECK(mmio.forceWake().shutdown() == MellowXe::MmioStatus::Ok);
        CHECK(pins.pinnedBytes == 0 && Mock::unsafeClearCalls == 0 && Mock::prematureDestroy == 0);
        pci->release(); mapper->release(); CHECK(Mock::objects.empty() && Mock::allocations.empty());
    }
};
int main() {
    { Fixture f; G::IOKitBinding binding(*f.pci, f.mmio, f.pins, f.proofs.ops(), 11);
      const auto b = binding.backend();
      CHECK(b.admitted(b.opaque, 42, 11) && !b.admitted(b.opaque, 42, 12));
      // Allocation generation99 is deliberately different from physical epoch11.
      CHECK(b.retain(b.opaque, f.region, true) && f.proofs.holds == 1);
      CHECK(b.synchronize(b.opaque, f.region) && Mock::syncCalls == 1);
      CHECK(b.mappingPublished(b.opaque, f.region, 11));
      CHECK(!b.mappingPublished(b.opaque, f.region, 99));
      f.proofs.quiet = false;
      CHECK(!b.release(b.opaque, f.region) && f.proofs.holds == 1 && f.pins.pinnedBytes == 8192);
      f.proofs.quiet = true; CHECK(b.release(b.opaque, f.region)); }
    { Fixture f; XeMemory::IOKitContext foreign; foreign.mapper = f.mapper;
      G::IOKitBinding binding(*f.pci, f.mmio, foreign, f.proofs.ops(), 11); const auto b = binding.backend();
      CHECK(!b.retain(b.opaque, f.region, true) && f.proofs.retainCalls == 0); }
    for (unsigned mode = 0; mode < 11; ++mode) {
        Fixture f; G::IOKitBinding binding(*f.pci, f.mmio, f.pins, f.proofs.ops(), 11); const auto b = binding.backend();
        auto r = f.region;
        switch (mode) {
        case 0: r.cpu = nullptr; break;
        case 1: r.bytes = 4096; break;
        case 2: ++r.owner; break;
        case 3: Mock::faults.copyDescriptor = true; break;
        case 4: Mock::faults.translatedBounce = 1; break;
        case 5: f.mapper->inactive = true; break;
        case 6: f.pci->fixtureProperty("iommu-parent", nullptr); break;
        case 7: f.pci->deviceNumber = 3; break;
        case 8: f.pci->pciConfig[4] = 2; break; // DMA bus mastering absent.
        case 9: f.pci->pciConfig[0x44] = 3; break; // Physical D3.
        case 10: f.pci->inactive = true; break;
        }
        CHECK(!b.retain(b.opaque, r, true) && f.proofs.retainCalls == 0);
        CHECK(!b.synchronize(b.opaque, r) && Mock::syncCalls == 0);
        CHECK(!b.mappingPublished(b.opaque, r, 11) && f.proofs.publicationCalls == 0);
        CHECK(f.pins.pinnedBytes == 8192 && f.pin.cookie && f.pin.dmaPages == f.region.dmaPages);
    }
    // The read-only pin inspection can expose a changed physical authority.
    // Recheck before an acquiring retain or a blocking DMA synchronization.
    for (unsigned operation = 0; operation < 3; ++operation) {
        Fixture f; G::IOKitBinding binding(*f.pci, f.mmio, f.pins, f.proofs.ops(), 11); const auto b = binding.backend();
        Mock::faults.inspectionContext = &f.proofs;
        Mock::faults.afterInspectionAddress = 0x801fff;
        Mock::faults.afterInspection = [](void *p) { static_cast<Proofs *>(p)->admitted = false; };
        if (operation == 0) CHECK(!b.retain(b.opaque, f.region, true));
        else if (operation == 1) CHECK(!b.synchronize(b.opaque, f.region));
        else CHECK(!b.mappingPublished(b.opaque, f.region, 11));
        CHECK(Mock::inspectionCallbacks == 1 && f.proofs.retainCalls == 0 &&
            f.proofs.publicationCalls == 0 && Mock::syncCalls == 0 && f.proofs.holds == 0);
        CHECK(f.pin.cookie && f.pins.pinnedBytes == 8192);
    }
    { Fixture f; G::IOKitBinding binding(*f.pci, f.mmio, f.pins, f.proofs.ops(), 11); const auto b = binding.backend();
      CHECK(b.retain(b.opaque, f.region, true)); f.proofs.revokeOnPublication = true;
      CHECK(!b.mappingPublished(b.opaque, f.region, 11) && f.proofs.publicationCalls == 1);
      CHECK(f.proofs.holds == 1 && f.pin.cookie && f.pins.pinnedBytes == 8192);
      f.proofs.quiet = false; CHECK(!b.release(b.opaque, f.region));
      f.proofs.quiet = true; CHECK(b.release(b.opaque, f.region)); }
    for (unsigned mode = 0; mode < 5; ++mode) {
        Fixture f; G::IOKitBinding binding(*f.pci, f.mmio, f.pins, f.proofs.ops(), 11); const auto b = binding.backend();
        CHECK(b.retain(b.opaque, f.region, true));
        struct Revocation { Fixture *f; unsigned mode; } revocation {&f, mode};
        Mock::faults.syncContext = &revocation;
        Mock::faults.afterSync = [](void *p, IOOptionBits direction) {
            CHECK(direction == kIODirectionOut); auto &r = *static_cast<Revocation *>(p);
            if (r.mode == 0) r.f->proofs.admitted = false;
            else if (r.mode == 1) Mock::faults.copyDescriptor = true;
            else if (r.mode == 2) Mock::faults.iovaShift = 4096;
            else if (r.mode == 3) {
                Mock::faults.inspectionContext = &r.f->proofs;
                Mock::faults.afterInspectionAddress = 0x801fff;
                Mock::faults.afterInspection = [](void *context) {
                    static_cast<Proofs *>(context)->admitted = false;
                };
            } else r.f->proofs.published = false;
        };
        CHECK(!b.synchronize(b.opaque, f.region) && Mock::syncCalls == 1);
        CHECK(f.proofs.holds == 1 && f.pin.cookie && f.pins.pinnedBytes == 8192);
        f.proofs.quiet = false; CHECK(!b.release(b.opaque, f.region) && f.proofs.holds == 1);
        f.proofs.quiet = true; CHECK(b.release(b.opaque, f.region) && f.proofs.holds == 0);
    }
    std::printf("PASS GuC IOKit direct-DMA binding: %u checks; simulated OS/MMIO, no physical GPU\n", checks);
}
