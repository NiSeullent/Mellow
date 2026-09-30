// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
#pragma once
#include "XeGuCFirmware.hpp"
#include "XeGgtt.hpp"
#include "XeMemory.hpp"

namespace XeGuCRegions {
enum class Status { Ok, Invalid, WrongOwner, WrongEpoch, NotFound, NoSpace,
                    Busy, Unavailable, Io, Quarantined };
enum class State { Free, Pinning, Reserved, Published, Quarantined };
struct Handle {
    size_t slot {SIZE_MAX};
    uint64_t owner {}, epoch {}, generation {};
};
struct View {
    Handle handle {};
    State state {State::Free};
    XeGuCFirmware::Region region {};
    XeGgtt::Handle ggtt {};
    uint32_t firmwareReferences {};
    bool ggttBackingHeld {};
};
// The exact XeMemory::Pin allocation ABI. pin/unpin own the descriptor, device
// IOMapper and immutable DMA array. cpu resolves ONLY pins returned by this
// backend; it proves the canonical complete allocation/CPU extent before any
// pointer reads. No NativeMemoryIOKit::DmaPin cookie conversion is permitted.
// The backend also owns nonaliasing backing and IOVM pages within each pin and
// across ALL live pins, including pins outside this Owner. This is actual
// descriptor/mapper allocation authority, not numerical DMA-address uniqueness
// or a permissive callback. DMA metadata remains immutable until unpin.
// Failed pin with an empty Pin means authoritative complete unwind; a retained
// partial Pin must be returned for checked cleanup. synchronize performs actual
// DMA/cache visibility (including any backend bounce copy), not GPU completion.
struct Pins {
    XeMemory::Backend backend {};
    void *opaque {};
    bool (*cpu)(void *, uint64_t owner, const XeMemory::Pin &, uint64_t bytes,
                uint8_t *&) {};
    XeMemory::Status (*synchronize)(void *, const XeMemory::Pin &, bool forDevice) {};
};
// One retained, authoritative physical 8086:7D41/GMD12.70 owner. ALL callbacks,
// CPU writers, reset/power, other GGTT writers and GuC users share its sleepable
// lock and never reenter this object. A boot argument is not this authority.
// The owner also authorizes only one Loader/reset attempt for this epoch; this
// adapter cannot infer another Loader's identity or admit disjoint boot plans.
struct Hardware {
    void *opaque {};
    bool (*admitted)(void *, uint64_t owner, uint64_t epoch) {};
    // Exclusive actual GGTT ranges with hardware size and firmware/display/
    // stolen/WOPCM/APIC exclusions. Zero PTEs do not establish a lease.
    // A failed acquire retains no lease; a failed release retains the lease.
    bool (*acquireSpace)(void *, uint64_t, uint64_t, const XeGgtt::Range *, size_t) {};
    bool (*releaseSpace)(void *, uint64_t, uint64_t) {};
    // Actual context/IRQ exclusion permitting a GuC reset; it alone does NOT
    // prove GuC stopped accessing ADS/log/backing. Loader uses this callback.
    bool (*resetAllowed)(void *, uint64_t, uint64_t) {};
    // Actual no present/future GuC/GPU/display access to ANY region in this
    // epoch, maintained through release/PTE clear/invalidate/DMA completion.
    // Region and GGTT-reference release require this independent proof.
    bool (*consumersQuiesced)(void *, uint64_t, uint64_t) {};
    // Actual MCR-aware PAT3 readback: this exact profile requires value 2.
    bool (*readPat3)(void *, uint64_t, uint64_t, uint32_t &) {};
    bool (*readPte)(void *, uint64_t, uint64_t, uint64_t ggtt, uint64_t &) {};
    bool (*writePte)(void *, uint64_t, uint64_t, uint64_t ggtt, uint64_t) {};
    // Posted-write completion and every applicable primary/media GT TLB.
    bool (*invalidate)(void *, uint64_t, uint64_t) {};
};

// Actual pin + GGTT + GuC region lifetime. Does not supply physical Hardware,
// choose free firmware/display ranges, initialize full ADS, create PPGTT/LRC,
// start a service, or establish native Metal. Store off the kernel stack.
// All created pins belong to this single-use owner until retire/close succeeds.
// No destructor drops possibly live resources. Keep Pins/Hardware/MMIO alive.
class Owner {
public:
    static constexpr size_t MaxRegions = 8;
    static constexpr uint64_t MaxBytes = 64ULL * 1024 * 1024;
    static constexpr uint64_t MaxTotalBytes = 256ULL * 1024 * 1024;
    Owner() = default;
    Owner(const Owner &) = delete;
    Owner &operator=(const Owner &) = delete;
    Status initialize(uint64_t owner, uint64_t epoch, const XeGgtt::Range *, size_t,
                      Pins, Hardware);
    // A failure that retains resources RETURNS its handle for explicit retry.
    Status allocate(uint64_t owner, uint64_t epoch, uint64_t bytes, Handle &);
    // No write while any firmware reference exists. Requires real quiescence,
    // fresh published PTEs, canonical CPU extent and a disjoint readable source.
    Status write(Handle, uint64_t offset, const uint8_t *source, size_t bytes);
    Status region(Handle, XeGuCFirmware::Region &);
    Status inspect(Handle, View &) const;
    Status retire(Handle);
    Status close();
    // The real Loader backend connects this owner's exact region holds and
    // synchronization. io/revision come from the same admitted physical owner.
    // Every region hold is exclusive: the Loader ABI has no borrower identity.
    // fullAdsValid is deliberately absent: Submission profile remains refused.
    XeGuCFirmware::Backend firmwareBackend(MellowXe::MmioAccess, uint8_t physicalRevision);
    uint64_t chargedBytes() const { return charged_; }
    size_t allocations() const;
    bool draining() const { return draining_; }
private:
    struct Slot {
        View view {};
        XeMemory::Pin pin {};
        XeGgtt::Backing backing {};
        uint64_t bytes {};
        bool pinOwned {}, reserved {}, pinCleanupUncertain {};
    };
    Slot slots_[MaxRegions] {};
    XeGgtt::Manager ggtt_ {};
    Pins pins_ {};
    Hardware hardware_ {};
    uint64_t owner_ {}, epoch_ {}, serial_ {1}, charged_ {};
    bool started_ {}, closed_ {}, draining_ {};
    bool admitted() const;
    bool quiet() const;
    Status lookup(Handle, size_t &) const;
    Slot *find(const XeGuCFirmware::Region &);
    Slot *find(const XeGgtt::Backing &);
    bool canonical(const Slot &, uint8_t *&) const;
    bool alias(const Slot &) const;
    bool fresh(Slot &);
    Status quarantine(Slot &);
    Status cleanup(Slot &);
    XeGgtt::Backend ggttBackend();
    static bool ggttAdmitted(void *, uint64_t);
    static bool acquireSpace(void *, uint64_t, const XeGgtt::Range *, size_t);
    static bool releaseSpace(void *, uint64_t);
    static bool retainBacking(void *, uint64_t, uint64_t, const XeGgtt::Backing &);
    static bool ownsBacking(void *, uint64_t, uint64_t, const XeGgtt::Backing &);
    static bool releaseBacking(void *, uint64_t, uint64_t, const XeGgtt::Backing &);
    static bool patReady(void *, uint64_t, uint8_t);
    static bool readPte(void *, uint64_t, uint64_t, uint64_t &);
    static bool writePte(void *, uint64_t, uint64_t, uint64_t);
    static bool invalidate(void *, uint64_t);
    static bool retired(void *, uint64_t, uint64_t, XeGgtt::Handle);
    static bool loaderAdmitted(void *, uint64_t, uint64_t);
    static bool resetAllowed(void *, uint64_t, uint64_t);
    static bool retainRegion(void *, const XeGuCFirmware::Region &, bool);
    static bool releaseRegion(void *, const XeGuCFirmware::Region &);
    static bool synchronizeRegion(void *, const XeGuCFirmware::Region &);
    static bool readPat3(void *, uint32_t &);
    static bool publishedRegion(void *, const XeGuCFirmware::Region &, uint64_t);
};
} // namespace XeGuCRegions

#ifdef KERNEL
// Only pins created by the existing XeMemoryIOKit backend enter these helpers.
// The context and its exact device mapper must outlive the Owner's final close.
namespace XeMemory { struct IOKitContext; }
namespace XeGuCRegions { Pins makeIOKitPins(XeMemory::IOKitContext &); }
#endif
