// SPDX-License-Identifier: MIT
// Target admission shared by the real IOKit service and portable regression tests.
#pragma once
#include <stdint.h>
namespace MellowTarget {
struct BootPolicy {
    unsigned darwinMajor {};
    bool diagnostic {}, disabled {}, legacyRequested {}, nativeRequested {};
};
constexpr bool admitDiagnostic(const BootPolicy &p) {
    return (p.darwinMajor == 24 || p.darwinMajor == 25) && p.diagnostic &&
        !p.disabled && !p.legacyRequested && !p.nativeRequested;
}
constexpr bool physicalTarget(uint16_t vendor, uint16_t device,
                              uint8_t bus, uint8_t slot, uint8_t function) {
    return vendor == 0x8086 && device == 0x7d41 && bus == 0 && slot == 2 && function == 0;
}
}
