// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and the pinned sources in docs/NVIDIA-GSP-FIRMWARE-OWNER.md.
#pragma once
#include "FirmwareImage.hpp"
#include "Radix3.hpp"
#include "../NativeGpu/MemoryOwner.hpp"

namespace MellowNative { class NativeMemoryIOKit; }
namespace Mellow { namespace PortedNvidiaGsp {

enum class OwnerStatus : uint8_t {
    Ok, Invalid, Unavailable, Ownership, Busy, Capacity, FirmwareInvalid,
    AliasedStorage, IoFailure, AuthenticationFailed, InitDoneFailed, Quarantined
};
enum class FirmwareOwnerState : uint8_t {
    Empty, Initialized, Preparing, Staged, BootAttempted, Running, Failed, Quarantined, Closed
};
struct BootBinary {
    FirmwareBytes bytes;
    // Actual matching-release, chip/HAL-selected RM_RISCV_UCODE_DESC offsets.
    uint64_t codeOffset {}, dataOffset {}, manifestOffset {};
};
struct FirmwareDmaImage {
    MellowNative::DeviceIdentity device;
    uint64_t owner {}, generation {};
    uint64_t radixRoot {}, imageBytes {}, radixAllocationBytes {};
    uint64_t signatureAddress {}, signatureBytes {}; // Booter extent rounded to 256.
    uint64_t bootBinaryAddress {}, bootBinaryBytes {};
    uint64_t codeOffset {}, dataOffset {}, manifestOffset {};
    uint64_t wprMetadataAddress {};
    // Read-only diagnostics only; never CPU-write/reuse after staging.
    FirmwareBytes radixAllocation, signatureAllocation, bootBinaryAllocation, wprMetadataAllocation;
};
struct FirmwareBootAuthority {
    void *context {};
    // Same exclusive physical NVIDIA device, mapper, reset/power/IRQ owner and
    // live epoch as memoryBackend. Source names/PCI metadata alone cannot admit.
    bool (*admitted)(void *, const MellowNative::DeviceIdentity &,
                     const MellowNative::MemoryBackend &) {};
    // Actual controller/context + device/mapper/reset/power/IRQ lifetime hold.
    // False MUST be a clean rejection with no acquisition. A successful hold
    // remains until release succeeds after MemoryOwner's epoch-wide stop/close.
    bool (*retain)(void *, const MellowNative::DeviceIdentity &, uint64_t owner, uint64_t generation) {};
    // False preserves the actual hold; caller cannot detach/free the context.
    bool (*release)(void *, const MellowNative::DeviceIdentity &, uint64_t owner, uint64_t generation) {};
    // Select from measured chip/HAL/CC state, matching driver release and actual
    // signed boot-binary storage. Source bytes remain immutable through prepare.
    bool (*select)(void *, const MellowNative::DeviceIdentity &, uint64_t owner,
                   uint64_t generation, FirmwareSelection &, BootBinary &) {};
    // Fill exact 256-byte GspFwWprMeta from actual FB/VBIOS/FRTS/carveout/heap
    // authority. Supports HAL-specific layout; no guessed register or FB value.
    // Seeded identity/sysmem/descriptor fields must remain exact; verified=0.
    bool (*populateWpr)(void *, const FirmwareDmaImage &, uint8_t *bytes, size_t) {};
    // Optional until a real bootstrap is implemented. ALL three are required
    // before invoking any boot operation. Missing callbacks preserve DMA pins.
    // bootstrap owns FWSEC/FRTS/RISC-V/Booter/args/RPC/MMIO ordering and effects.
    bool (*bootstrap)(void *, const FirmwareDmaImage &) {};
    // Actual signed Booter/firmware verification and locked WPR handoff, read
    // from device authority. A CPU metadata verified value is never this proof.
    bool (*authenticated)(void *, const FirmwareDmaImage &) {};
    // Actual GSP_INIT_DONE response with successful rpc_result for this epoch.
    // A timeout/CPU flag/register-ready bit alone must never return true.
    bool (*waitInitDone)(void *, const FirmwareDmaImage &) {};
};
struct FirmwareOwnerView {
    FirmwareOwnerState state {FirmwareOwnerState::Empty};
    bool authorityHeld {}, dmaStaged {}, bootstrapAttempted {}, authenticated {}, rmInitDone {};
    uint32_t ownedResources {};
    uint64_t chargedBytes {};
    FirmwareDmaImage dma;
    MellowNative::MemoryStatus lastMemory {MellowNative::MemoryStatus::Ok};
    FirmwareStatus lastFirmware {FirmwareStatus::Ok};
    Status lastRadix {Status::Ok};
};

// Single-use private raw-DMA owner, without fabricated GPU-VA mappings. The
// internal MemoryOwner is never exposed: only this object can release its pins.
// ALL calls, CPU writers, physical callbacks, detach/reset and other consumers
// share the same sleepable owner lock. Store the object off the kernel stack.
// No destructor releases live resources; all backends/contexts and this object
// outlive close()==Ok. MemoryBackend::quiesce MUST stop GSP/Booter DMA as well as
// every GPU/display/IRQ/worker consumer and prevent future access in this epoch.
class FirmwareOwner {
public:
    FirmwareOwner() = default;
    FirmwareOwner(const FirmwareOwner &) = delete;
    FirmwareOwner &operator=(const FirmwareOwner &) = delete;
    OwnerStatus initialize(MellowNative::DeviceIdentity, uint64_t owner,
                           MellowNative::MemoryLimits, MellowNative::MemoryBackend,
                           FirmwareBootAuthority);
#ifdef KERNEL
    // Direct production connection to the already attached real IOKit adapter.
    // Does not attach/open a device, provide missing physical VM/boot authority,
    // change mapper/addressing policy or allow an identity fallback.
    OwnerStatus initializeNative(MellowNative::DeviceIdentity, uint64_t owner,
        MellowNative::MemoryLimits, MellowNative::NativeMemoryIOKit &, FirmwareBootAuthority);
#endif
    // Allocates/copies/synchronizes image Radix3, signature, selected boot binary
    // and WPR metadata. Ok means DMA staged, never authenticated or RM-ready.
    // Scratch contains at least all four allocations' page counts in uint64_t
    // words. Disjoint stable scratch/source/pin spans are required. No scratch
    // survives this synchronous call. Once any pin is acquired, failure retains
    // its ownership until explicit close with actual all-consumer quiescence.
    OwnerStatus prepare(FirmwareBytes container, FirmwareParseLimits,
                        uint64_t *scratch, size_t scratchWords);
    OwnerStatus boot();
    void inspect(FirmwareOwnerView &) const;
    OwnerStatus close();
private:
    static constexpr size_t ResourceCount = 4;
    MellowNative::MemoryOwner memory_;
    MellowNative::MemoryBackend memoryBackend_ {};
    MellowNative::MemoryLimits limits_ {};
    FirmwareBootAuthority authority_ {};
    MellowNative::DeviceIdentity device_ {};
    uint64_t owner_ {}, generation_ {1};
    MellowNative::MemoryHandle handles_[ResourceCount] {};
    MellowNative::MemoryView pins_[ResourceCount] {};
    FirmwareOwnerView view_ {};
    bool initialized_ {}, complete_ {}, authorityHeld_ {};
    bool admitted() const;
    bool currentPins() const;
    OwnerStatus memoryStatus(MellowNative::MemoryStatus);
    OwnerStatus fail(OwnerStatus);
    bool wprExact() const;
};

} } // namespace Mellow::PortedNvidiaGsp
