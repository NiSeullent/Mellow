// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// BOOT0/BOOT1 decoding follows Nouveau device/base.c at Linux commit
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a; see docs/NATIVE-GPU-OWNER.md.
#include "Probe.hpp"

namespace MellowNativeNvidia {
Architecture architectureForChipset(uint16_t chip) {
    switch (chip & 0x1f0U) {
        case 0x110: case 0x120: return Architecture::Maxwell;
        case 0x130: return Architecture::Pascal;
        case 0x140: return Architecture::Volta;
        case 0x160: return Architecture::Turing;
        case 0x170: return Architecture::Ampere;
        case 0x180: return Architecture::Hopper;
        case 0x190: return Architecture::Ada;
        case 0x1a0: return Architecture::BlackwellGb10x;
        case 0x1b0: return Architecture::BlackwellGb20x;
        default: return Architecture::Unknown;
    }
}
bool knownChipset(uint16_t chip) {
    switch (chip) {
        case 0x117: case 0x118: case 0x120: case 0x124: case 0x126: case 0x12b:
        case 0x130: case 0x132: case 0x134: case 0x136: case 0x137: case 0x138: case 0x13b:
        case 0x140: case 0x162: case 0x164: case 0x166: case 0x167: case 0x168:
        case 0x170: case 0x172: case 0x173: case 0x174: case 0x176: case 0x177:
        case 0x180: case 0x192: case 0x193: case 0x194: case 0x196: case 0x197:
        case 0x1a0: case 0x1a2: case 0x1b2: case 0x1b3: case 0x1b5: case 0x1b6: case 0x1b7:
            return true;
        default: return false;
    }
}
static bool sameDevice(const DeviceIdentity &a, const DeviceIdentity &b) {
    return a.vendor == b.vendor && a.device == b.device &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
static bool sameLocation(PciLocation a, PciLocation b) {
    return a.bus == b.bus && a.slot == b.slot && a.function == b.function;
}
bool bar0PhysicalAddress(const PciSnapshot &s, uint64_t &address) {
    address = 0;
    // Assigned type-0 32/64-bit memory BAR. No sizing/config writes or I/O BARs.
    if (s.bar0Low == 0xffffffffU || (s.bar0Low & 1U) ||
        ((s.bar0Low & 6U) != 0 && (s.bar0Low & 6U) != 4)) return false;
    const uint64_t base = uint64_t(s.bar0Low & ~0xfU) |
        ((s.bar0Low & 6U) == 4 ? uint64_t(s.bar0High) << 32 : 0);
    if (!base || base > UINT64_MAX - 8) return false;
    address = base; return true;
}
ProbeStatus validatePci(const PhysicalExpectation &expected, const PciSnapshot &s) {
    if (expected.device.vendor != 0x10de || !expected.device.device ||
        expected.device.device == 0xffff || !expected.device.registryId ||
        !expected.device.epoch || expected.location.slot > 31 ||
        expected.location.function > 7) return ProbeStatus::Invalid;
    if (!knownChipset(expected.chipset)) return ProbeStatus::UnsupportedChipset;
    if (!sameDevice(expected.device, s.device) || !sameLocation(expected.location, s.location))
        return ProbeStatus::IdentityChanged;
    if (!s.providerOpen || s.inactive) return ProbeStatus::Ownership;
    if (s.tunneled) return ProbeStatus::Unavailable;
    uint64_t barAddress = 0;
    if (!bar0PhysicalAddress(s, barAddress)) return ProbeStatus::Unavailable;
    // Only type-0 VGA/3D display PCI functions; no NVIDIA audio or bridge match.
    const uint16_t classSubclass = static_cast<uint16_t>(s.classRevision >> 16);
    if ((s.headerType & 0x7fU) != 0 ||
        (classSubclass != 0x0300 && classSubclass != 0x0302)) return ProbeStatus::Invalid;
    if (s.command == 0xffff || !(s.command & 2U) || !s.powerStateD0 ||
        s.pmCapabilityOffset < 0x40 || s.pmCapabilityOffset > 0xf8 ||
        (s.pmCapabilityOffset & 3U) || s.pmcsr == 0xffff || (s.pmcsr & 3U))
        return ProbeStatus::Unavailable;
    return ProbeStatus::Ok;
}
ProbeStatus probePhysical(const PhysicalExpectation &expected, ProbeAccess access, PhysicalProbe &out) {
    out = {};
    if (!access.context || !access.snapshot || !access.read32) return ProbeStatus::Unavailable;
    PciSnapshot before {}, after {};
    if (!access.snapshot(access.context, before)) return ProbeStatus::IoFailure;
    ProbeStatus status = validatePci(expected, before);
    if (status != ProbeStatus::Ok) return status;
    uint32_t boot0 = 0, boot1 = 0;
    if (!access.read32(access.context, Boot0Offset, boot0) ||
        !access.read32(access.context, Boot1Offset, boot1)) return ProbeStatus::IoFailure;
    if (!access.snapshot(access.context, after)) return ProbeStatus::IoFailure;
    status = validatePci(expected, after);
    if (status != ProbeStatus::Ok) return status;
    if (before.classRevision != after.classRevision || before.command != after.command ||
        before.pmcsr != after.pmcsr || before.headerType != after.headerType ||
        before.pmCapabilityOffset != after.pmCapabilityOffset ||
        before.bar0Low != after.bar0Low || before.bar0High != after.bar0High)
        return ProbeStatus::IdentityChanged;
    if (!boot0 || boot0 == 0xffffffffU || boot1 == 0xffffffffU || !(boot0 & 0x1f000000U))
        return ProbeStatus::IoFailure;
    if (boot1 == 0x01000001U) return ProbeStatus::UnsupportedEndian;
    const uint16_t chipset = static_cast<uint16_t>((boot0 & 0x1ff00000U) >> 20);
    if (chipset != expected.chipset) return ProbeStatus::IdentityChanged;
    if (!knownChipset(chipset)) return ProbeStatus::UnsupportedChipset;
    const Architecture architecture = architectureForChipset(chipset);
    if (chipset >= 0x160 && (boot1 & 0x00030000U)) return ProbeStatus::VirtualDevice;
    out.physical = expected; out.pci = after;
    out.boot0 = boot0; out.boot1 = boot1;
    out.chipRevision = static_cast<uint8_t>(boot0); out.architecture = architecture;
    return ProbeStatus::Ok;
}
}
