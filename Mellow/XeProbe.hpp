// SPDX-License-Identifier: MIT
// Read-only physical discovery. This code executes in IOKit and in fake-I/O tests.
// PCI configuration offsets follow PCI type-0; GMD_ID follows Linux Xe (MIT).
// No arbitrary register interface, forcewake writes, GPU work or DMA is exposed.
#pragma once
#include <stdint.h>
#include "SequoiaTarget.hpp"
namespace MellowProbe {
enum class Status : uint32_t {
    Ok, InvalidAccess, PciUnavailable, WrongDevice, WrongClass,
    PowerUnavailable, BarUnavailable, MmioUnavailable, UnsupportedIp, Changed
};
struct Access {
    void *context {};
    bool (*pci32)(void *, uint16_t, uint32_t &) {};
    bool (*mmio32)(void *, uint32_t, uint32_t &) {};
    uint64_t barBytes {};
    uint8_t bus {}, slot {}, function {}, pmCapability {};
};
struct Snapshot {
    uint32_t pciId {}, subsystemId {}, classRevision {}, command {}, pmcsr {}, gmd {};
    uint64_t barBytes {};
    uint8_t bus {}, slot {}, function {};
};
constexpr uint32_t GmdRegister = 0x0d8c;
constexpr uint32_t architecture(uint32_t gmd) { return gmd >> 22; }
constexpr uint32_t release(uint32_t gmd) { return (gmd >> 14) & 255U; }
inline Status capture(const Access &a, Snapshot &out) {
    out = {};
    if (!a.pci32 || !a.mmio32) return Status::InvalidAccess;
    uint32_t id = 0, cls = 0, cmd = 0, subsystem = 0, pm = 0;
    if (!a.pci32(a.context, 0, id) || id == UINT32_MAX || id == 0) return Status::PciUnavailable;
    if (!MellowTarget::physicalTarget(static_cast<uint16_t>(id), static_cast<uint16_t>(id >> 16),
                                     a.bus, a.slot, a.function)) return Status::WrongDevice;
    if (!a.pci32(a.context, 8, cls) || cls == UINT32_MAX) return Status::PciUnavailable;
    // Class 03, VGA-compatible subclass 00 or other display subclass 80 only.
    if ((cls >> 16) != 0x0300 && (cls >> 16) != 0x0380) return Status::WrongClass;
    if (!a.pci32(a.context, 4, cmd) || cmd == UINT32_MAX || !(cmd & 2U)) return Status::BarUnavailable;
    if (a.barBytes < GmdRegister + 4ULL) return Status::BarUnavailable;
    if (a.pmCapability < 0x40 || a.pmCapability > 0xf8 || (a.pmCapability & 3U)) return Status::PowerUnavailable;
    // The caller must have found the PM capability through IOPCIFamily.
    if (!a.pci32(a.context, static_cast<uint16_t>(a.pmCapability + 4), pm) || pm == UINT32_MAX ||
        (pm & 3U)) return Status::PowerUnavailable;
    if (!a.pci32(a.context, 0x2c, subsystem) || subsystem == UINT32_MAX) return Status::PciUnavailable;
    uint32_t gmd = 0;
    if (!a.mmio32(a.context, GmdRegister, gmd) || gmd == UINT32_MAX) return Status::MmioUnavailable;
    if (architecture(gmd) != 12 || release(gmd) != 70) return Status::UnsupportedIp;
    // Detect an observable identity/power/decoder transition during the sample.
    // This is not a reset epoch or a cryptographic/atomic hardware attestation.
    uint32_t id2 = 0, cmd2 = 0, pm2 = 0, gmd2 = 0;
    if (!a.pci32(a.context, 0, id2) || !a.pci32(a.context, 4, cmd2) ||
        !a.pci32(a.context, static_cast<uint16_t>(a.pmCapability + 4), pm2)) return Status::Changed;
    if (id2 != id || (cmd2 & 7U) != (cmd & 7U) || pm2 == UINT32_MAX || (pm2 & 3U)) return Status::Changed;
    if (!a.mmio32(a.context, GmdRegister, gmd2) || gmd2 != gmd) return Status::Changed;
    out = {id, subsystem, cls, cmd & 0xffffU, pm & 0xffffU, gmd, a.barBytes, a.bus, a.slot, a.function};
    return Status::Ok;
}
}
