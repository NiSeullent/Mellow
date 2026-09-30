// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "../Drivers/NativeNvidia/Probe.hpp"
#include <stdio.h>
#include <stdlib.h>
using namespace MellowNativeNvidia;
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr); abort(); } } while (0)
struct Fake {
    PhysicalExpectation expected {{0x10de, 0x2204, 123, 7}, {1,0,0}, 0x172};
    PciSnapshot pci {expected.device, expected.location, 0x030000a1, 2, 0, 0, 0x60, true, false, true, 0xa0000000, 0, false};
    uint32_t boot0 {0x172000a1}, boot1 {};
    unsigned samples {}, reads {};
    bool snapshotFailure {}, readFailure {}, removeAfterReads {}, commandChange {}, pmChange {}, barChange {};
    static bool snapshot(void *p, PciSnapshot &out) {
        auto &f = *static_cast<Fake *>(p); ++f.samples;
        if (f.snapshotFailure) return false;
        out = f.pci;
        if (f.reads >= 2) {
            if (f.removeAfterReads) ++out.device.registryId;
            if (f.commandChange) out.command ^= 4;
            if (f.pmChange) out.pmcsr ^= 0x8000;
            if (f.barChange) out.bar0Low ^= 0x1000000;
        }
        return true;
    }
    static bool read(void *p, uint32_t offset, uint32_t &out) {
        auto &f = *static_cast<Fake *>(p); ++f.reads;
        if (f.readFailure) return false;
        if (offset != Boot0Offset && offset != Boot1Offset) return false;
        out = offset == Boot0Offset ? f.boot0 : f.boot1; return true;
    }
    ProbeAccess access() { return {this, snapshot, read}; }
};
static void expectFailure(Fake f, ProbeStatus expected) {
    PhysicalProbe out {}; out.boot0 = 99; out.physical.device.epoch = 99;
    CHECK(probePhysical(f.expected, f.access(), out) == expected);
    CHECK(out.boot0 == 0 && out.boot1 == 0 && out.physical.device.epoch == 0 &&
        out.architecture == Architecture::Unknown);
}
int main() {
    Fake f; PhysicalProbe out {};
    CHECK(probePhysical(f.expected, f.access(), out) == ProbeStatus::Ok);
    CHECK(f.samples == 2 && f.reads == 2);
    CHECK(out.physical.device.registryId == 123 && out.physical.device.epoch == 7);
    CHECK(out.architecture == Architecture::Ampere && out.chipRevision == 0xa1);
    // Class recognition and exact-chipset admission are independent. Test every
    // 9-bit chipset value, including holes within otherwise known architectures.
    const uint16_t known[] = {0x117,0x118,0x120,0x124,0x126,0x12b,
        0x130,0x132,0x134,0x136,0x137,0x138,0x13b,0x140,
        0x162,0x164,0x166,0x167,0x168,0x170,0x172,0x173,0x174,0x176,0x177,
        0x180,0x192,0x193,0x194,0x196,0x197,0x1a0,0x1a2,
        0x1b2,0x1b3,0x1b5,0x1b6,0x1b7};
    for (uint16_t chip = 0; chip < 512; ++chip) {
        bool present = false; for (auto item : known) present |= item == chip;
        CHECK(knownChipset(chip) == present);
        Fake model; model.expected.chipset = chip; model.boot0 = (uint32_t(chip) << 20) | 0xa1;
        const auto status = probePhysical(model.expected, model.access(), out);
        CHECK(status == (present ? ProbeStatus::Ok : ProbeStatus::UnsupportedChipset));
        if (present) CHECK(out.architecture != Architecture::Unknown);
        else CHECK(model.reads == 0 && out.boot0 == 0);
    }
    CHECK(architectureForChipset(0x100) == Architecture::Unknown); // Kepler
    CHECK(architectureForChipset(0x118) == Architecture::Maxwell);
    CHECK(architectureForChipset(0x130) == Architecture::Pascal);
    CHECK(architectureForChipset(0x140) == Architecture::Volta);
    CHECK(architectureForChipset(0x160) == Architecture::Turing);
    CHECK(architectureForChipset(0x180) == Architecture::Hopper);
    CHECK(architectureForChipset(0x190) == Architecture::Ada);
    CHECK(architectureForChipset(0x1a0) == Architecture::BlackwellGb10x);
    CHECK(architectureForChipset(0x1b0) == Architecture::BlackwellGb20x);
    f = {}; f.expected.device.vendor = 0x8086; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.expected.device.device = 0xffff; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.expected.device.registryId = 0; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.expected.device.epoch = 0; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.expected.location.slot = 32; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.expected.location.function = 8; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.pci.device.device ^= 1; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.pci.device.epoch++; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.pci.location.bus++; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.pci.providerOpen = false; expectFailure(f, ProbeStatus::Ownership);
    f = {}; f.pci.inactive = true; expectFailure(f, ProbeStatus::Ownership);
    f = {}; f.pci.tunneled = true; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.bar0Low = 0; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.bar0Low = 0xffffffff; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.bar0Low |= 1; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.bar0Low |= 2; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.bar0Low |= 6; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.bar0Low = 4; f.pci.bar0High = 1;
    CHECK(probePhysical(f.expected,f.access(),out)==ProbeStatus::Ok);
    uint64_t physical = 0; CHECK(bar0PhysicalAddress(f.pci, physical) && physical == 0x100000000ULL);
    f = {}; f.pci.headerType = 1; expectFailure(f, ProbeStatus::Invalid);
    f = {}; f.pci.classRevision = 0x040300a1; expectFailure(f, ProbeStatus::Invalid); // audio
    f = {}; f.pci.command = 0xffff; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.command = 4; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.powerStateD0 = false; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.pmcsr = 3; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.pmcsr = 0xffff; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.pmCapabilityOffset = 0; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.pmCapabilityOffset = 0xfc; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.pmCapabilityOffset = 0x61; expectFailure(f, ProbeStatus::Unavailable);
    f = {}; f.pci.pmCapabilityOffset = 0xf8; CHECK(probePhysical(f.expected,f.access(),out)==ProbeStatus::Ok);
    f = {}; f.pci.classRevision = 0x030200a1; f.pci.headerType = 0x80;
    CHECK(probePhysical(f.expected,f.access(),out)==ProbeStatus::Ok);
    f = {}; f.snapshotFailure = true; expectFailure(f, ProbeStatus::IoFailure);
    f = {}; f.readFailure = true; expectFailure(f, ProbeStatus::IoFailure);
    f = {}; f.removeAfterReads = true; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.commandChange = true; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.pmChange = true; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.barChange = true; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.boot0 = 0; expectFailure(f, ProbeStatus::IoFailure);
    f = {}; f.boot0 = 0xffffffff; expectFailure(f, ProbeStatus::IoFailure);
    f = {}; f.boot1 = 0xffffffff; expectFailure(f, ProbeStatus::IoFailure);
    f = {}; f.boot0 = 0x174000a1; expectFailure(f, ProbeStatus::IdentityChanged);
    f = {}; f.boot1 = 0x01000001; expectFailure(f, ProbeStatus::UnsupportedEndian);
    f = {}; f.boot1 = 0x00010000; expectFailure(f, ProbeStatus::VirtualDevice);
    f = {}; f.boot1 = 0x00020000; expectFailure(f, ProbeStatus::VirtualDevice);
    f = {}; f.boot1 = 0x00030000; expectFailure(f, ProbeStatus::VirtualDevice);
    CHECK(probePhysical(f.expected, {nullptr,Fake::snapshot,Fake::read},out)==ProbeStatus::Unavailable);
    CHECK(probePhysical(f.expected, {&f,nullptr,Fake::read},out)==ProbeStatus::Unavailable);
    CHECK(probePhysical(f.expected, {&f,Fake::snapshot,nullptr},out)==ProbeStatus::Unavailable);
    printf("PASS native NVIDIA probe: %u checks; no physical GPU execution\n",checks);
}
