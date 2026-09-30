// SPDX-License-Identifier: MIT
// GuC ABI declarations adapted from Intel's MIT-licensed Linux Xe sources:
// xe_guc_fwif.h / xe_guc_ads.c: Copyright © 2022 Intel Corporation
// abi/guc_capture_abi.h: Copyright © 2024 Intel Corporation
// abi/guc_klvs_abi.h: Copyright © 2021 Intel Corporation
// Source pin: 4d7d9486c04d917265f64c55bd23b2cc4fe7749c, drivers/gpu/drm/xe/.
// Independently authored bounded serialization/ownership code, 2026, MIT per
// file. MIT text: Drivers/PortedXe/LICENSE.MIT. Repository LICENSE is separate.
#pragma once
#include "XeGuCFirmware.hpp"

namespace XeGuCAds {
constexpr size_t PageBytes = 4096, Classes = 16, Instances = 32;
constexpr size_t MaxEngines = 3, MaxCaptureLists = 10, MaxRegsPerList = 4096;
constexpr uint32_t MaxRegsetBytes = 1024 * 1024, MaxCaptureBytes = 1024 * 1024;
constexpr uint16_t WaBlockInterrupts = 0x9002, WaResetBbStack = 0x900b;
enum class Error : uint8_t { None, Invalid, Unsupported, Capacity, Alias,
    Unavailable, FirmwareMismatch, Busy, Synchronization, Corrupt, Quarantined };
enum class State : uint8_t { Empty, Preload, PartiallyPublished, PostLoaded, Failed };
enum class CaptureKind : uint8_t { Global = 0, Class = 1, Instance = 2 };
enum class Access : uint8_t { Read, PreloadWrite, GoldenWrite, CapturedSourceRead };
enum class Direction : uint8_t { CpuToDevice, DeviceToCpu };

// Packed on-wire structs; fields are serialized explicitly as little endian.
// RegSet.count is 16 bits, not a u32. No C++ flexible-array extension is used.
struct __attribute__((packed)) Reg {
    uint32_t offset, value, flags, mask;
};
struct __attribute__((packed)) RegSet {
    uint32_t address;
    uint16_t count, reserved;
};
struct __attribute__((packed)) Policies {
    uint32_t submissionQueueDepth[Classes];
    uint32_t dpcPromoteTime, isValid, maxWorkItems, globalFlags, reserved[4];
};
struct __attribute__((packed)) SystemInfo {
    uint8_t mapping[Classes][Instances];
    uint32_t enabledMasks[Classes], generic[16];
};
struct __attribute__((packed)) Ads {
    RegSet regState[Classes][Instances];
    uint32_t reserved0, schedulerPolicies, gtSystemInfo, reserved1, controlData;
    uint32_t goldenLrca[Classes], engineStateBytes[Classes], privateData, umInitData;
    uint32_t captureInstance[2][Classes], captureClass[2][Classes], captureGlobal[2];
    uint32_t waAddressLo, waAddressHi, waBytes, reserved[11];
};
struct __attribute__((packed)) EngineUsageRecord {
    uint32_t currentContextIndex, lastSwitchInStamp, reserved0, totalRuntime, reserved1[4];
};
struct __attribute__((packed)) EngineUsage {
    EngineUsageRecord engines[Classes][Instances];
};
struct __attribute__((packed)) UmQueueParams {
    uint64_t baseDpa;
    uint32_t baseGgtt, bytes, reserved[4];
};
struct __attribute__((packed)) UmInitParams {
    uint64_t pageResponseTimeoutUs;
    uint32_t reserved[6];
    UmQueueParams queues[3];
};
struct __attribute__((packed)) Fixed {
    Ads ads;
    Policies policies;
    SystemInfo systemInfo;
    EngineUsage engineUsage;
    UmInitParams um;
    // Dynamic Reg entries immediately follow, then page-aligned sections.
};
struct __attribute__((packed)) CaptureHeader { uint32_t info; };
static_assert(sizeof(Reg) == 16 && sizeof(RegSet) == 8, "GuC register ABI");
static_assert(offsetof(RegSet, count) == 4 && offsetof(RegSet, reserved) == 6, "GuC regset fields");
static_assert(sizeof(Policies) == 96 && sizeof(SystemInfo) == 640, "GuC policy/system ABI");
static_assert(sizeof(EngineUsageRecord) == 32 && sizeof(EngineUsage) == 16384, "GuC usage ABI");
static_assert(sizeof(UmQueueParams) == 32 && sizeof(UmInitParams) == 128, "GuC UM ABI");
static_assert(sizeof(CaptureHeader) == 4 && sizeof(Ads) == 4572, "GuC ADS ABI");
static_assert(offsetof(Ads, schedulerPolicies) == 4100 && offsetof(Ads, gtSystemInfo) == 4104, "GuC ADS links");
static_assert(offsetof(Ads, controlData) == 4112 && offsetof(Ads, goldenLrca) == 4116, "GuC golden links");
static_assert(offsetof(Ads, engineStateBytes) == 4180 && offsetof(Ads, privateData) == 4244, "GuC state/private links");
static_assert(offsetof(Ads, umInitData) == 4248 && offsetof(Ads, captureInstance) == 4252, "GuC UM/capture links");
static_assert(offsetof(Ads, captureClass) == 4380 && offsetof(Ads, captureGlobal) == 4508, "GuC capture arrays");
static_assert(offsetof(Ads, waAddressLo) == 4516 && offsetof(Ads, waAddressHi) == 4520 && offsetof(Ads, waBytes) == 4524, "GuC WA links");
static_assert(offsetof(Ads, reserved) == 4528, "GuC ADS reserved tail");
static_assert(offsetof(Fixed, policies) == 4572 && offsetof(Fixed, systemInfo) == 4668, "GuC fixed policy layout");
static_assert(offsetof(Fixed, engineUsage) == 5308 && offsetof(Fixed, um) == 21692 && sizeof(Fixed) == 21820, "GuC fixed prefix");

struct Engine {
    uint8_t gucClass {}, physicalInstance {}, logicalInstance {};
    uint32_t mmioBase {};
    const Reg *regs {};
    size_t regCount {};
};
struct CaptureList {
    uint8_t index {}, captureClass {}; // PF=0/VF=1; capture class differs from GuC class
    CaptureKind kind {CaptureKind::Global};
    const Reg *regs {};
    size_t regCount {};
};
struct WaKlv {
    uint16_t key {}, valueDwords {};
    const uint32_t *values {};
};
// Kernel-owned immutable snapshot, never an IOUserClient argument. Main GT0 of
// 8086:7D41 / graphics1270 only. Media1300 may be reported, but its separate GT
// is not implemented here. Xe-LPG has neither USM queues nor indirect ring state.
// Snapshot authority must authenticate the retained pinned firmware metadata,
// the complete fused/admitted engine topology, complete merged reg_sr lists,
// capture lists (including authoritative empty lists), WA rules, and actual
// doorbell register readback. A subset may
// only be supplied if the real driver disabled the other engines before boot.
struct Input {
    uint64_t owner {}, epoch {};
    uint16_t deviceId {}, graphicsVersion {}, mediaVersion {};
    uint8_t gtId {}, pciRevision {};
    const void *snapshotCookie {};
    MellowXe::FirmwareInfo firmware {};
    const Engine *engines {};
    size_t engineCount {};
    uint32_t enabledMasks[Classes] {};
    uint32_t doorbellCountPerSqidi {};
    uint32_t regsetReserveBytes {}; // zero uses exact supplied regset size
    const CaptureList *captureLists {};
    size_t captureListCount {};
    const WaKlv *waKlvs {};
    size_t waKlvCount {};
};
struct Layout {
    uint32_t regsetOffset {}, regsetUsedBytes {}, regsetBytes {};
    uint32_t goldenOffset {}, goldenBytes {}, waOffset {}, waUsedBytes {};
    uint32_t captureOffset {}, captureUsedBytes {}, captureBytes {};
    uint32_t privateOffset {}, privateBytes {}, totalBytes {};
    uint32_t classGoldenOffset[Classes] {}, classGoldenBytes[Classes] {}, classStateBytes[Classes] {};
    uint16_t enabledClasses {};
};
struct FenceProof {
    uint64_t timeline {}, sequence {};
    uint32_t contextId {}, ggttLrca {};
    const void *kernelFence {};
};
struct Capture {
    uint64_t owner {}, epoch {};
    uint8_t gtId {}, gucClass {}, physicalInstance {}, logicalInstance {};
    XeGuCFirmware::Region source {};
    uint32_t imageOffset {}, imageBytes {};
    FenceProof prime {}, switchedTo {}; // two distinct actual kernel contexts/jobs
    const void *provenanceCookie {};
};
// All calls share the driver's ownership/GT lock. The driver retains Input,
// every referenced immutable array, ADS/source mappings, and capture receipts
// through the Builder's lifetime. This module never acquires/releases them.
// regionOwned must verify exact CPU/DMA/GGTT mappings and exclude hidden aliases;
// PreloadWrite additionally requires stopped/quiesced GuC, and GoldenWrite only
// allows the controlled bootstrap publication phase with user work blocked.
// captureValid must verify both GPU fence completions, ordering, the actual WA
// job then NOP context switch on this physical engine, saved LRC identity, and
// acquire-ordered CPU visibility. IDs/sequences here alone prove none of that.
// No production implementation of this authority is supplied by this module.
struct KernelAuthority {
    void *opaque {};
    bool (*snapshotValid)(void *, const Input &) {};
    bool (*regionOwned)(void *, const Input &, const XeGuCFirmware::Region &, Access) {};
    bool (*synchronize)(void *, const XeGuCFirmware::Region &, uint32_t offset, uint32_t bytes, Direction) {};
    bool (*captureValid)(void *, const Input &, const Capture &) {};
};
// Pure bounded sizing/structural admission of already accessible, immutable
// kernel arrays; it cannot authenticate arbitrary virtual pointers. Production
// entrypoints call snapshotValid before this helper. A direct caller must have
// established array ownership/lifetime before calling. Success is not readiness.
Error calculateLayout(const Input &, Layout &);
// Read-only strict pre-boot inspection, including empty reserved golden slots.
// This is the Loader's preloadAdsValid integration point. Minimal ADS fails it.
Error validatePreload(const Input &, const XeGuCFirmware::Region &, const KernelAuthority &);

// Allocation-free and single-use. Allocate off stack. The Loader verifies the
// actual GGTT PTEs/firmware independently. PostLoaded state is not a health proof;
// general submission admission must call validatePostLoad() for fresh authority.
class Builder {
public:
    explicit Builder(KernelAuthority authority) : authority_(authority) {}
    Builder(const Builder &) = delete;
    Builder &operator=(const Builder &) = delete;
    Error buildPreload(const Input &, const XeGuCFirmware::Region &);
    Error validatePreload() const;
    Error publishGolden(const Capture &);
    Error validatePostLoad() const;
    State state() const { return state_; }
    const Layout &layout() const { return layout_; }
    uint16_t publishedClasses() const { return publishedClasses_; }
private:
    KernelAuthority authority_ {};
    Input input_ {};
    XeGuCFirmware::Region region_ {};
    Layout layout_ {};
    Capture captures_[Classes] {};
    uint16_t publishedClasses_ {};
    State state_ {State::Empty};
};
}
