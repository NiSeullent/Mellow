// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "NvidiaMmioIOKit.hpp"
#include "HardwareAccess.hpp"
#include <libkern/c++/OSArray.h>

namespace MellowNativeNvidia {
ProbeStatus IOKitMmio::attach(IOPCIDevice *device, IOService *owner, PhysicalExpectation expected) {
    if (device_ || map_) return ProbeStatus::Ownership;
    if (!device || !owner || owner->getProvider() != device) return ProbeStatus::Ownership;
    // The provider must be open already. Retains protect object lifetime only;
    // the provider's power/reset lock supplies hardware access exclusion.
    device_ = device; owner_ = owner; expected_ = expected;
    device_->retain();
    PciSnapshot pci {};
    if (!snapshot(this, pci)) { detach(); return ProbeStatus::IoFailure; }
    ProbeStatus status = validatePci(expected_, pci);
    if (status != ProbeStatus::Ok) { detach(); return status; }
    uint64_t physical = 0;
    if (!bar0PhysicalAddress(pci, physical)) { detach(); return ProbeStatus::Unavailable; }
    auto *property = device_->copyProperty(gIODeviceMemoryKey);
    auto *array = OSDynamicCast(OSArray, property);
    bool duplicate = false;
    if (array) for (unsigned i = 0; i < array->getCount(); ++i) {
        auto *descriptor = OSDynamicCast(IOMemoryDescriptor, array->getObject(i));
        if (!descriptor) continue;
        const auto tag = descriptor->getTag();
        if ((tag & 0xffU) != kIOPCIConfigBaseAddress0) continue;
        if (bar0_) { duplicate = true; break; }
        const auto memorySpace = (tag >> 24) & 3U;
        const auto expectedSpace = (pci.bar0Low & 6U) == 4 ? kIOPCI64BitMemorySpace : kIOPCI32BitMemorySpace;
        const uint32_t expectedBdf = uint32_t(pci.location.bus) << 16 |
            uint32_t(pci.location.slot) << 11 | uint32_t(pci.location.function) << 8;
        if (memorySpace != static_cast<unsigned>(expectedSpace) ||
            (tag & 0x00ffff00U) != expectedBdf) { duplicate = true; break; }
        IOByteCount contiguous = 0;
        const auto descriptorPhysical = descriptor->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        if (descriptorPhysical != physical || descriptor->getLength() < 8 ||
            descriptor->getLength() > UINT64_MAX - physical || contiguous < descriptor->getLength()) {
            duplicate = true; break;
        }
        descriptor->retain(); bar0_ = descriptor;
    }
    if (property) property->release();
    if (!bar0_ || duplicate) { detach(); return ProbeStatus::Unavailable; }
    map_ = bar0_->map(kIOMapInhibitCache);
    if (!map_ || map_->getLength() < 8 || !map_->getVirtualAddress() || (map_->getVirtualAddress() & 3U)) {
        detach(); return ProbeStatus::IoFailure;
    }
    if (map_->getPhysicalAddress() != physical || map_->getLength() != bar0_->getLength()) {
        detach(); return ProbeStatus::IdentityChanged;
    }
    base_ = reinterpret_cast<volatile uint32_t *>(map_->getVirtualAddress());
    length_ = map_->getLength();
    PhysicalProbe probeResult {};
    status = sample(probeResult);
    if (status != ProbeStatus::Ok) detach();
    return status;
}
void IOKitMmio::detach() {
    base_ = nullptr; length_ = 0;
    if (map_) { map_->release(); map_ = nullptr; }
    if (bar0_) { bar0_->release(); bar0_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    owner_ = nullptr; expected_ = {};
}
ProbeStatus IOKitMmio::sample(PhysicalProbe &out) {
    if (!device_ || !map_) { out = {}; return ProbeStatus::Unavailable; }
    return probePhysical(expected_, {this, snapshot, read}, out);
}
bool IOKitMmio::snapshot(void *opaque, PciSnapshot &out) {
    auto &self = *static_cast<IOKitMmio *>(opaque);
    out = {};
    if (!self.device_ || !self.owner_) return false;
    auto *device = self.device_;
    const uint32_t id = device->configRead32(kIOPCIConfigVendorID);
    out.device = {static_cast<uint16_t>(id), static_cast<uint16_t>(id >> 16),
        device->getRegistryEntryID(), self.expected_.device.epoch};
    out.location = {device->getBusNumber(), device->getDeviceNumber(), device->getFunctionNumber()};
    out.classRevision = device->configRead32(kIOPCIConfigRevisionID);
    out.command = device->configRead16(kIOPCIConfigCommand);
    out.headerType = device->configRead8(0x0e);
    out.bar0Low = device->configRead32(kIOPCIConfigBaseAddress0);
    out.bar0High = (out.bar0Low & 6U) == 4 ? device->configRead32(kIOPCIConfigBaseAddress1) : 0;
    auto *tunnelProperty = device->copyProperty(kIOPCITunnelledKey);
    out.tunneled = tunnelProperty != nullptr;
    if (tunnelProperty) tunnelProperty->release();
    // Bounded read-only type-0 capability walk. findPCICapability invokes
    // configAccess(true) in Apple's implementation; use guarded reads instead.
    out.pmcsr = 0xffff;
    const uint16_t pciStatus = device->configRead16(kIOPCIConfigStatus);
    uint8_t offset = pciStatus != 0xffff && (pciStatus & 0x10U) && !(out.headerType & 0x7fU)
        ? device->configRead8(0x34) : 0;
    uint64_t visited = 0;
    for (unsigned count = 0; offset && count < 48; ++count) {
        if (offset < 0x40 || offset > 0xfc || (offset & 3U)) break;
        const uint64_t bit = uint64_t(1) << ((offset - 0x40) / 4);
        if (visited & bit) break;
        visited |= bit;
        const uint32_t capability = device->configRead32(offset);
        if (capability == UINT32_MAX) break;
        if ((capability & 0xffU) == kIOPCIPowerManagementCapability) {
            if (offset <= 0xf8) {
                out.pmCapabilityOffset = offset;
                out.pmcsr = device->configRead16(offset + 4U);
            }
            break;
        }
        offset = static_cast<uint8_t>(capability >> 8);
    }
    out.providerOpen = device->isOpen(self.owner_) && self.owner_->getProvider() == device;
    out.inactive = device->isInactive() || self.owner_->isInactive();
    out.powerStateD0 = device->getPowerState() == kIOPCIDeviceOnState;
    return true;
}
bool IOKitMmio::read(void *opaque, uint32_t offset, uint32_t &value) {
    auto &self = *static_cast<IOKitMmio *>(opaque);
    // Only documented read-only identity registers; queue register leases are
    // supplied by a separately negotiated channel, never guessed here.
    if (offset != Boot0Offset && offset != Boot1Offset) return false;
    PciSnapshot pci {};
    if (!snapshot(opaque, pci) || validatePci(self.expected_, pci) != ProbeStatus::Ok) return false;
    if (!self.validBar0(pci)) return false;
    __sync_synchronize();
    const bool result = MellowHardware::read32(self.base_, self.length_, offset, value);
    __sync_synchronize();
    return result;
}
bool IOKitMmio::validBar0(const PciSnapshot &pci) const {
    uint64_t physical = 0;
    if (!map_ || !bar0_ || !bar0PhysicalAddress(pci, physical) ||
        map_->getPhysicalAddress() != physical || map_->getLength() != length_ ||
        bar0_->getLength() != length_ || length_ < 8 || length_ > UINT64_MAX - physical) return false;
    IOByteCount contiguous = 0;
    if (bar0_->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone) != physical ||
        contiguous < length_) return false;
    const auto tag = bar0_->getTag();
    const uint32_t bdf = uint32_t(pci.location.bus) << 16 |
        uint32_t(pci.location.slot) << 11 | uint32_t(pci.location.function) << 8;
    const unsigned space = (pci.bar0Low & 6U) == 4 ? kIOPCI64BitMemorySpace : kIOPCI32BitMemorySpace;
    return (tag & 0xffU) == kIOPCIConfigBaseAddress0 &&
        (tag & 0x00ffff00U) == bdf && ((tag >> 24) & 3U) == space;
}
}
