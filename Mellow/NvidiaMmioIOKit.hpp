// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include "../Drivers/NativeNvidia/Probe.hpp"
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNativeNvidia {
// Real x86-64 IOKit PCI/BAR0 read-only owner. The provider driver first opens
// this exact device and holds its sleepable power/reset lock across ALL calls.
// Owner must outlive this object. A reset/power transition requires detach and
// a fresh epoch. Uses a retained descriptor property snapshot, avoiding PCI
// memory-accessor tunnel-power changes. Tunneled devices are unavailable here.
// No GPU submission, config/MMIO writes, provider open/close or PM request.
class IOKitMmio {
public:
    IOKitMmio() = default;
    IOKitMmio(const IOKitMmio &) = delete;
    IOKitMmio &operator=(const IOKitMmio &) = delete;
    ProbeStatus attach(IOPCIDevice *, IOService *owner, PhysicalExpectation);
    ProbeStatus sample(PhysicalProbe &);
    void detach();
    uint64_t mappedLength() const { return length_; }
private:
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOMemoryMap *map_ {};
    IOMemoryDescriptor *bar0_ {};
    volatile uint32_t *base_ {};
    uint64_t length_ {};
    PhysicalExpectation expected_ {};
    bool validBar0(const PciSnapshot &) const;
    static bool snapshot(void *, PciSnapshot &);
    static bool read(void *, uint32_t, uint32_t &);
};
}
