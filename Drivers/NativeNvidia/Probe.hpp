// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include "../NativeGpu/MemoryOwner.hpp"

namespace MellowNativeNvidia {
using MellowNative::DeviceIdentity;
enum class ProbeStatus : uint8_t {
    Ok, Invalid, Ownership, Unavailable, IoFailure, IdentityChanged,
    UnsupportedChipset, UnsupportedEndian, VirtualDevice
};
enum class Architecture : uint8_t {
    Unknown, Maxwell, Pascal, Volta, Turing, Ampere, Hopper, Ada,
    BlackwellGb10x, BlackwellGb20x
};
struct PciLocation {
    uint8_t bus {}, slot {}, function {};
};
struct PhysicalExpectation {
    DeviceIdentity device {};
    PciLocation location {};
    uint16_t chipset {};
};
struct PciSnapshot {
    DeviceIdentity device {};
    PciLocation location {};
    uint32_t classRevision {};
    uint16_t command {}, pmcsr {};
    uint8_t headerType {}, pmCapabilityOffset {};
    bool providerOpen {}, inactive {true}, powerStateD0 {};
    uint32_t bar0Low {}, bar0High {};
    bool tunneled {};
};
struct PhysicalProbe {
    PhysicalExpectation physical {};
    PciSnapshot pci {};
    uint32_t boot0 {}, boot1 {};
    uint8_t chipRevision {};
    Architecture architecture {Architecture::Unknown};
    // This read-only observation never negotiates an engine/channel, enables
    // bus mastering, authenticates firmware, or establishes acceleration.
};
struct ProbeAccess {
    void *context {};
    bool (*snapshot)(void *, PciSnapshot &) {};
    bool (*read32)(void *, uint32_t byteOffset, uint32_t &) {};
};
constexpr uint32_t Boot0Offset = 0, Boot1Offset = 4;
bool knownChipset(uint16_t);
Architecture architectureForChipset(uint16_t);
bool bar0PhysicalAddress(const PciSnapshot &, uint64_t &);
ProbeStatus validatePci(const PhysicalExpectation &, const PciSnapshot &);
// Trusted provider call under the same sleepable ownership/power/reset lock.
// Samples config before AND after BAR reads; all failure outputs are empty.
ProbeStatus probePhysical(const PhysicalExpectation &, ProbeAccess, PhysicalProbe &);
}
