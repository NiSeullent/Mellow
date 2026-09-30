// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See Drivers/PortedXe/LICENSE.MIT.
#pragma once
#include "XeGgtt.hpp"
#include "XeMmioIOKit.hpp"
#include <IOKit/IOService.h>

namespace XeGgtt {
// Trusted kernel authority, supplied by the PCI owner's retained IOService.
// These callbacks describe real leases in that owner, never user-client values
// or readiness constants. They must not reenter the binding or acquire its
// serialization lock: caller holds ONE sleepable domain covering all GGTT/PAT,
// MMIO invalidation, firmware/CT, reset, power and backing allocation operations.
struct Authority {
    IOService *owner {};
    // Exclusive physical PCI/mapper/reset epoch authority, with its main GT
    // forcewake reference held. An open PCI client alone does not prove this.
    bool (*ownsEpoch)(IOService *,uint64_t) {};
    // Both GuCs are stopped/reset and CT/submission disabled; no firmware,
    // display, other driver or GAM-port caller can issue a competing request or
    // consume a mapping/PAT update during the ENTIRE write/invalidation call.
    // This is stronger than merely observing that CT is disabled.
    bool (*ownsBootstrap)(IOService *,uint64_t) {};
    // Actual exclusive range leases, including firmware/display/stolen aliases
    // and any exclusions beyond the adapter's conservative WOPCM/APIC bounds.
    // False acquires nothing; false release retains the complete lease.
    bool (*acquireSpace)(IOService *,uint64_t,const Range *,size_t) {};
    bool (*releaseSpace)(IOService *,uint64_t) {};
    // Validate private owner records BEFORE the adapter dereferences a cookie
    // or DMA array. Prove exact device IOMapper, immutable full DMA array,
    // identity, pin, CPU view and absence of unauthorized backing aliases.
    bool (*validateBacking)(IOService *,uint64_t,uint64_t,const Backing &) {};
    // False retain acquires nothing; false release retains the complete pin.
    bool (*retainBacking)(IOService *,uint64_t,uint64_t,const Backing &) {};
    bool (*releaseBacking)(IOService *,uint64_t,uint64_t,const Backing &) {};
    // Exact allocation generation has no present/future GuC/display/engine
    // consumer, with actual fence/stop/reset completion held through retirement.
    bool (*retired)(IOService *,uint64_t,uint64_t,Handle) {};
};

// Physical PF 8086:7D41, main graphics12.70 AND media13.00 only. Performs real
// BAR0 GSM PTE access, MCR-aware PAT3 read/program and bounded main/media GuC
// MMIO TLB invalidation. It installs no IOService, IRQ, firmware or submit hook.
// Raw invalidation/PTE/PAT writes are ONLY for stopped-GuC bootstrap. After
// firmware starts, existing published mappings can still be read/retained;
// mutations require a later real GuC-action backend or a completed reset.
//
// Constructor retains PCI and Authority.owner. The caller must retain the
// already attached IOKitMmio and this binding while any Manager/firmware/CT
// consumer exists. close() balances references only after all leases release.
// No destructor tears down resources. On uncertain MMIO/wake/flush failure,
// quarantine is sticky: pins, ranges, owner and wake references remain held.
// There is deliberately no synthetic reset recovery or unconditional release.
class IOKitBinding {
public:
    IOKitBinding(IOPCIDevice &,MellowXe::IOKitMmio &,Authority,uint64_t epoch);
    IOKitBinding(const IOKitBinding &)=delete;
    IOKitBinding &operator=(const IOKitBinding &)=delete;
    Backend backend();
    Status readPat3(uint32_t &value);
    Status programPat3();
    Status close();
    bool quarantined() const { return faulted_; }
    uint64_t epoch() const { return epoch_; }
private:
    struct PinLease { bool held {}; uint64_t owner {}; Backing backing {}; };
    IOPCIDevice &device_; MellowXe::IOKitMmio &mmio_; Authority authority_ {};
    uint64_t epoch_ {}; Range ranges_[Manager::MaxRanges] {};
    size_t rangeCount_ {}; PinLease pins_[Manager::MaxMappings] {};
    bool spaceHeld_ {}, mainWakeHeld_ {}, mediaWakeHeld_ {}, semaphoreHeld_ {};
    bool faulted_ {}, closed_ {};
    bool physical(uint64_t,bool allowFault=false);
    bool bootstrap();
    bool read32(uint32_t,uint32_t &);
    bool write32(uint32_t,uint32_t);
    bool posted();
    bool poll(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,bool resetRequired);
    bool wakes();
    bool beginPat();
    bool endPat();
    bool within(uint64_t) const;
    PinLease *find(uint64_t,const Backing &);
    bool validPinned(uint64_t,const Backing &);
    bool ownsEntry(uint64_t);
    bool flushOne(uint32_t);
    static bool admitted(void *,uint64_t);
    static bool acquireSpace(void *,uint64_t,const Range *,size_t);
    static bool releaseSpace(void *,uint64_t);
    static bool retainBacking(void *,uint64_t,uint64_t,const Backing &);
    static bool ownsBacking(void *,uint64_t,uint64_t,const Backing &);
    static bool releaseBacking(void *,uint64_t,uint64_t,const Backing &);
    static bool patReady(void *,uint64_t,uint8_t);
    static bool readPte(void *,uint64_t,uint64_t,uint64_t &);
    static bool writePte(void *,uint64_t,uint64_t,uint64_t);
    static bool invalidate(void *,uint64_t);
    static bool retired(void *,uint64_t,uint64_t,Handle);
};
}
