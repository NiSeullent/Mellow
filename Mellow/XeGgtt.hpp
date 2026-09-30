// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See Drivers/PortedXe/LICENSE.MIT.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace XeGgtt {
constexpr uint64_t PageSize=4096;
enum class Status { Ok, Invalid, WrongOwner, WrongEpoch, NotFound, NoSpace,
                    Busy, Unavailable, Io, Quarantined };
enum class State { Free, Reserved, Published, Quarantined };
struct Range { uint64_t address {}, bytes {}; };
struct Handle { size_t slot {}; uint64_t generation {}; };
struct Backing {
    void *cookie {}; uint8_t *cpu {}; const uint64_t *dmaPages {};
    size_t pageCount {}; uint64_t identity {};
};
struct Backend {
    void *context {};
    // Must prove physical 8086:7D41 / graphics 12.70, D0, correct device mapper,
    // MMIO/forcewake and exact reset epoch. Serialize reset/power with ALL calls.
    bool (*admitted)(void *,uint64_t epoch) {};
    // Acquire exclusive authority over every supplied range until releaseSpace.
    // Prove hardware GGTT size, firmware/display/stolen/WOPCM/APIC exclusions.
    // A zero PTE is NOT a range ownership proof. Failure acquires nothing.
    bool (*acquireSpace)(void *,uint64_t,const Range *,size_t) {};
    // False retains the space lease, permitting retry. No partial release.
    bool (*releaseSpace)(void *,uint64_t) {};
    // Validate owner, device IOMapper, identity, CPU view and full immutable DMA
    // array, exclusive backing/no unauthorized aliases. Retain pins and metadata
    // until release; false retain acquires nothing, false release retains all.
    bool (*retainBacking)(void *,uint64_t owner,uint64_t epoch,const Backing &) {};
    bool (*ownsBacking)(void *,uint64_t,uint64_t,const Backing &) {};
    bool (*releaseBacking)(void *,uint64_t,uint64_t,const Backing &) {};
    // Actual platform PAT programming/readback; GGTT has no per-PTE RO policy.
    bool (*patReady)(void *,uint64_t,uint8_t) {};
    // Addresses are GGTT VAs, not MMIO offsets. Writes may take effect on false.
    // Adapter supplies qword access/order and all applicable write workarounds.
    bool (*readPte)(void *,uint64_t epoch,uint64_t address,uint64_t &) {};
    bool (*writePte)(void *,uint64_t,uint64_t,uint64_t) {};
    // Finish posted writes AND all relevant GT/media invalidations, bounded.
    // True requires authoritative completion, never an unknown interrupt.
    bool (*invalidate)(void *,uint64_t) {};
    // No present/future consumer (including GuC/display) can reference this exact
    // allocation generation. Must include required fences/stop/reset completion.
    // Hold this condition through release/retire in the shared serialization domain.
    bool (*retired)(void *,uint64_t owner,uint64_t epoch,Handle) {};
};
struct Mapping {
    State state {State::Free}; uint64_t owner {}, epoch {}, generation {};
    Range range {}; Backing backing {}; uint8_t pat {}; uint32_t users {};
};
// Trusted kernel API, not user-client input. Allocation-free bounded storage.
// Caller serializes every operation, all other GGTT writers and reset/power;
// callbacks must not reenter. Keep manager/backend/backing alive until close Ok.
// No destructor tears down live resources. No default boot or submission hooks.
class Manager {
public:
    static constexpr size_t MaxRanges=32, MaxMappings=64, MaxPages=16384;
    Manager()=default;
    Manager(const Manager&)=delete; Manager& operator=(const Manager&)=delete;
    Status initialize(uint64_t epoch,const Range *,size_t,Backend);
    Status reserve(uint64_t owner,uint64_t epoch,uint64_t bytes,uint64_t alignment,Handle &);
    Status reserveAt(uint64_t owner,uint64_t epoch,Range,Handle &);
    Status publish(uint64_t owner,uint64_t epoch,Handle,const Backing &,uint8_t pat);
    // Exact identity + fresh hardware readback + completed earlier publication.
    // Retain additionally prevents retirement; release needs real quiescence.
    Status published(uint64_t owner,uint64_t epoch,Handle,const Backing &);
    Status retain(uint64_t owner,uint64_t epoch,Handle,const Backing &);
    Status release(uint64_t owner,uint64_t epoch,Handle);
    Status retire(uint64_t owner,uint64_t epoch,Handle);
    Status inspect(uint64_t owner,uint64_t epoch,Handle,Mapping &) const;
    Status close();
private:
    struct Slot { Mapping m {}; bool held {}, touched {}, cleared {}; };
    Slot slots_[MaxMappings] {}; Range ranges_[MaxRanges] {};
    size_t count_ {}; uint64_t epoch_ {}, serial_ {1}; Backend io_ {};
    bool started_ {}, closed_ {};
    bool live() const;
    Status get(uint64_t,uint64_t,Handle,size_t &) const;
    bool fits(Range) const;
    Status insert(uint64_t,Range,Handle &);
    bool read(uint64_t,uint64_t &) const;
    bool expected(const Slot &,size_t,uint64_t &) const;
    bool verify(const Slot &,bool clearing) const;
    Status teardown(size_t);
};
}
