// SPDX-License-Identifier: MIT
// Intel MIT protocol/layout sources, pinned at
// https://github.com/torvalds/linux/tree/4d7d9486c04d917265f64c55bd23b2cc4fe7749c/drivers/gpu/drm/xe
// xe_guc_ads.c / xe_guc_fwif.h: Copyright © 2022 Intel Corporation
// abi/guc_capture_abi.h: Copyright © 2024 Intel Corporation
// abi/guc_klvs_abi.h: Copyright © 2021 Intel Corporation
// xe_lrc.c / xe_pci.c: Copyright © 2021 Intel Corporation
// regs/xe_engine_regs.h: Copyright © 2023 Intel Corporation
// Independently authored bounded implementation, 2026, MIT per file.
// License text: Drivers/PortedXe/LICENSE.MIT; repository LICENSE is separate.
#include "XeGuCAds.hpp"

namespace XeGuCAds {
namespace {
constexpr uint32_t regMasked = 1, regSteering = 2;
constexpr uint32_t steeringBits = (31U << 12) | (15U << 20);
constexpr uint32_t engineFlags = regMasked | regSteering | steeringBits;
constexpr uint32_t captureFlags = 15U | steeringBits;
constexpr uint32_t maxRegionBytes = 64U * 1024 * 1024;
constexpr uint32_t maxPrivateBytes = 32U * 1024 * 1024;
constexpr uint32_t maxSourceBytes = 1024 * 1024;
constexpr uint32_t minGgtt = 4U * 1024 * 1024;

bool pointerSpan(const void *p, size_t bytes) {
    return p && bytes && uintptr_t(p) <= UINTPTR_MAX - bytes;
}
bool arraySpan(const void *p, size_t count, size_t element) {
    return count && count <= SIZE_MAX / element && pointerSpan(p, count * element);
}
bool overlap(const void *a, size_t n, const void *b, size_t m) {
    if (!n || !m) return false;
    if (!pointerSpan(a, n) || !pointerSpan(b, m)) return true;
    const uintptr_t x = uintptr_t(a), y = uintptr_t(b);
    return x < y + m && y < x + n;
}
bool addressOverlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return n && m && a < b + m && b < a + n;
}
bool alignPage(uint64_t value, uint32_t &out) {
    if (value > UINT32_MAX - (PageBytes - 1)) return false;
    out = uint32_t((value + PageBytes - 1) & ~(uint64_t(PageBytes) - 1));
    return true;
}
void put16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}
void clear(uint8_t *p, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
}
void copy(uint8_t *to, const uint8_t *from, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) to[i] = from[i];
}
bool same(const uint8_t *a, const uint8_t *b, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) if (a[i] != b[i]) return false;
    return true;
}
bool zero(const uint8_t *p, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) if (p[i]) return false;
    return true;
}
uint8_t captureClass(uint8_t gucClass) {
    return gucClass == 0 || gucClass == 4 ? 0 : 3;
}
uint32_t goldenSize(uint8_t gucClass) {
    return gucClass == 0 || gucClass == 4 ? 14U * PageBytes : 2U * PageBytes;
}
bool regValid(const Reg &r, bool capture) {
    const uint32_t permitted = capture ? captureFlags : engineFlags;
    if (!r.offset || (r.offset & 3) || r.offset >= 0x400000 || (r.flags & ~permitted)) return false;
    if ((r.flags & steeringBits) && !(r.flags & regSteering)) return false;
    // Engine regsets contain saved values written by GuC, not initial MMIO writes.
    return capture || (!r.value && !r.mask);
}
bool inputAliases(const Input &in, const void *p, size_t bytes) {
    if (overlap(&in, sizeof(in), p, bytes) ||
        overlap(in.engines, in.engineCount * sizeof(Engine), p, bytes) ||
        overlap(in.captureLists, in.captureListCount * sizeof(CaptureList), p, bytes) ||
        overlap(in.waKlvs, in.waKlvCount * sizeof(WaKlv), p, bytes)) return true;
    for (size_t i = 0; i < in.engineCount; ++i)
        if (overlap(in.engines[i].regs, in.engines[i].regCount * sizeof(Reg), p, bytes)) return true;
    for (size_t i = 0; i < in.captureListCount; ++i)
        if (overlap(in.captureLists[i].regs, in.captureLists[i].regCount * sizeof(Reg), p, bytes)) return true;
    return false;
}
bool regionShape(const Input &in, const XeGuCFirmware::Region &r) {
    if (r.owner != in.owner || !r.generation || !r.pinCookie ||
        !r.bytes || r.bytes > maxRegionBytes || (r.bytes & (PageBytes - 1)) ||
        (r.ggtt & (PageBytes - 1)) || r.ggtt < minGgtt ||
        r.ggtt >= XeGuCFirmware::gucGgttTop || r.bytes > XeGuCFirmware::gucGgttTop - r.ggtt ||
        !pointerSpan(r.cpu, size_t(r.bytes)) || r.pageCount != r.bytes / PageBytes ||
        !arraySpan(r.dmaPages, r.pageCount, sizeof(uint64_t)) ||
        overlap(r.cpu, size_t(r.bytes), r.dmaPages, r.pageCount * sizeof(uint64_t))) return false;
    return true;
}
// Only after regionOwned authenticates the exact pin/mapping may these kernel
// pointers be dereferenced. Pin generation and the GT reset epoch are distinct.
bool regionPagesValid(const XeGuCFirmware::Region &r) {
    // The authoritative owner also checks hidden aliases via other CPU mappings.
    // Explicit repeats in the supplied page list are rejected locally.
    for (size_t i = 0; i < r.pageCount; ++i) {
        if ((r.dmaPages[i] & (PageBytes - 1)) || r.dmaPages[i] >= (1ULL << 46)) return false;
        for (size_t j = 0; j < i; ++j) if (r.dmaPages[i] == r.dmaPages[j]) return false;
    }
    return true;
}
bool regionsAlias(const XeGuCFirmware::Region &a, const XeGuCFirmware::Region &b) {
    if (a.pinCookie == b.pinCookie || addressOverlap(a.ggtt, a.bytes, b.ggtt, b.bytes) ||
        overlap(a.cpu, size_t(a.bytes), b.cpu, size_t(b.bytes)) ||
        overlap(a.cpu, size_t(a.bytes), b.dmaPages, b.pageCount * sizeof(uint64_t)) ||
        overlap(b.cpu, size_t(b.bytes), a.dmaPages, a.pageCount * sizeof(uint64_t)) ||
        overlap(a.dmaPages, a.pageCount * sizeof(uint64_t), b.dmaPages, b.pageCount * sizeof(uint64_t))) return true;
    for (size_t i = 0; i < a.pageCount; ++i)
        for (size_t j = 0; j < b.pageCount; ++j)
            if (a.dmaPages[i] == b.dmaPages[j]) return true;
    return false;
}
bool layoutSame(const Layout &a, const Layout &b) {
    if (a.regsetOffset != b.regsetOffset || a.regsetUsedBytes != b.regsetUsedBytes ||
        a.regsetBytes != b.regsetBytes || a.goldenOffset != b.goldenOffset ||
        a.goldenBytes != b.goldenBytes || a.waOffset != b.waOffset || a.waUsedBytes != b.waUsedBytes ||
        a.captureOffset != b.captureOffset || a.captureUsedBytes != b.captureUsedBytes ||
        a.captureBytes != b.captureBytes || a.privateOffset != b.privateOffset ||
        a.privateBytes != b.privateBytes || a.totalBytes != b.totalBytes || a.enabledClasses != b.enabledClasses) return false;
    for (size_t c = 0; c < Classes; ++c)
        if (a.classGoldenOffset[c] != b.classGoldenOffset[c] || a.classGoldenBytes[c] != b.classGoldenBytes[c] ||
            a.classStateBytes[c] != b.classStateBytes[c]) return false;
    return true;
}
uint32_t regAddress(const XeGuCFirmware::Region &r, const Layout &l, const Input &in, size_t index) {
    uint32_t offset = l.regsetOffset;
    for (size_t i = 0; i < index; ++i) offset += uint32_t(in.engines[i].regCount * sizeof(Reg));
    return uint32_t(r.ggtt + offset);
}
uint32_t captureListOffset(const Layout &l, const Input &in, size_t index) {
    uint32_t offset = l.captureOffset + PageBytes;
    for (size_t i = 0; i < index; ++i) offset += uint32_t(sizeof(CaptureHeader) + in.captureLists[i].regCount * sizeof(Reg));
    return offset;
}
uint32_t capturePointer(const XeGuCFirmware::Region &r, const Layout &l, const Input &in,
                        unsigned index, CaptureKind kind, unsigned cls) {
    for (size_t i = 0; i < in.captureListCount; ++i) {
        const CaptureList &list = in.captureLists[i];
        if (list.index == index && list.kind == kind && (kind == CaptureKind::Global || list.captureClass == cls))
            return uint32_t(r.ggtt + captureListOffset(l, in, i));
    }
    return uint32_t(r.ggtt + l.captureOffset); // real empty-list header in first capture page
}
void putReg(uint8_t *p, const Reg &r) {
    put32(p, r.offset); put32(p + 4, r.value); put32(p + 8, r.flags); put32(p + 12, r.mask);
}
bool regSame(const uint8_t *p, const Reg &r, bool active) {
    return get32(p) == r.offset && (active || get32(p + 4) == r.value) &&
        get32(p + 8) == r.flags && get32(p + 12) == r.mask;
}
Error inspect(const Input &in, const XeGuCFirmware::Region &r, const Layout &l,
              const KernelAuthority &authority, bool active, uint16_t published) {
    if (!authority.snapshotValid || !authority.regionOwned ||
        !authority.snapshotValid(authority.opaque, in)) return Error::Unavailable;
    Layout expected {};
    Error error = calculateLayout(in, expected);
    if (error != Error::None) return error;
    if (!layoutSame(l, expected) || (published & ~l.enabledClasses)) return Error::Corrupt;
    if (!regionShape(in, r) || r.bytes < l.totalBytes) return Error::Invalid;
    if (!authority.regionOwned(authority.opaque, in, r, Access::Read)) return Error::Unavailable;
    if (!regionPagesValid(r)) return Error::Invalid;
    if (inputAliases(in, r.cpu, size_t(r.bytes)) ||
        inputAliases(in, r.dmaPages, r.pageCount * sizeof(uint64_t))) return Error::Alias;
    if (active) {
        if (!authority.synchronize || !authority.synchronize(authority.opaque, r, 0, l.privateOffset, Direction::DeviceToCpu)) return Error::Synchronization;
        if (!authority.snapshotValid(authority.opaque, in) ||
            !authority.regionOwned(authority.opaque, in, r, Access::Read)) return Error::Unavailable;
    }
    const uint8_t *p = r.cpu;
    if (get32(p + offsetof(Ads, reserved0)) || get32(p + offsetof(Ads, reserved1)) ||
        get32(p + offsetof(Ads, controlData)) || get32(p + offsetof(Ads, umInitData)) ||
        !zero(p + offsetof(Ads, reserved), sizeof(Ads::reserved)) ||
        get32(p + offsetof(Ads, schedulerPolicies)) != r.ggtt + offsetof(Fixed, policies) ||
        get32(p + offsetof(Ads, gtSystemInfo)) != r.ggtt + offsetof(Fixed, systemInfo) ||
        get32(p + offsetof(Ads, privateData)) != r.ggtt + l.privateOffset) return Error::Corrupt;
    for (size_t c = 0; c < Classes; ++c) {
        const uint32_t golden = l.classGoldenBytes[c] ? uint32_t(r.ggtt + l.classGoldenOffset[c]) : 0;
        if (get32(p + offsetof(Ads, goldenLrca) + 4 * c) != golden ||
            get32(p + offsetof(Ads, engineStateBytes) + 4 * c) != l.classStateBytes[c]) return Error::Corrupt;
        for (size_t instance = 0; instance < Instances; ++instance) {
            uint32_t address = 0; uint16_t count = 0;
            for (size_t i = 0; i < in.engineCount; ++i) {
                const Engine &e = in.engines[i];
                if (e.gucClass == c && e.physicalInstance == instance) {
                    address = regAddress(r, l, in, i); count = uint16_t(e.regCount);
                }
            }
            const size_t off = offsetof(Ads, regState) + (c * Instances + instance) * sizeof(RegSet);
            if (get32(p + off) != address || get16(p + off + 4) != count || get16(p + off + 6)) return Error::Corrupt;
            uint8_t mapped = Instances;
            for (size_t i = 0; i < in.engineCount; ++i)
                if (in.engines[i].gucClass == c && in.engines[i].logicalInstance == instance)
                    mapped = in.engines[i].physicalInstance;
            if (p[offsetof(Fixed, systemInfo) + c * Instances + instance] != mapped) return Error::Corrupt;
        }
        if (get32(p + offsetof(Fixed, systemInfo) + offsetof(SystemInfo, enabledMasks) + 4 * c) != in.enabledMasks[c]) return Error::Corrupt;
    }
    const size_t policy = offsetof(Fixed, policies);
    if (!zero(p + policy, sizeof(Policies::submissionQueueDepth)) ||
        get32(p + policy + offsetof(Policies, dpcPromoteTime)) != 500000 ||
        get32(p + policy + offsetof(Policies, isValid)) != 1 ||
        get32(p + policy + offsetof(Policies, maxWorkItems)) != 15 ||
        get32(p + policy + offsetof(Policies, globalFlags)) ||
        !zero(p + policy + offsetof(Policies, reserved), sizeof(Policies::reserved))) return Error::Corrupt;
    for (size_t i = 0; i < 16; ++i)
        if (get32(p + offsetof(Fixed, systemInfo) + offsetof(SystemInfo, generic) + 4 * i) !=
            (i == 2 ? in.doorbellCountPerSqidi : 0)) return Error::Corrupt;
    if ((!active && !zero(p + offsetof(Fixed, engineUsage), sizeof(EngineUsage))) ||
        !zero(p + offsetof(Fixed, um), sizeof(UmInitParams))) return Error::Corrupt;
    uint32_t regOff = l.regsetOffset;
    for (size_t i = 0; i < in.engineCount; ++i)
        for (size_t j = 0; j < in.engines[i].regCount; ++j, regOff += sizeof(Reg))
            if (!regSame(p + regOff, in.engines[i].regs[j], active)) return Error::Corrupt;
    if (!zero(p + regOff, l.goldenOffset - regOff)) return Error::Corrupt;
    for (size_t c = 0; c < Classes; ++c)
        if (l.classGoldenBytes[c] && !(published & (1U << c)) &&
            !zero(p + l.classGoldenOffset[c], l.classGoldenBytes[c])) return Error::Corrupt;
    if (get32(p + offsetof(Ads, waAddressLo)) != r.ggtt + l.waOffset ||
        get32(p + offsetof(Ads, waAddressHi)) || get32(p + offsetof(Ads, waBytes)) != l.waUsedBytes) return Error::Corrupt;
    for (size_t i = 0; i < in.waKlvCount; ++i)
        if (get32(p + l.waOffset + 4 * i) != (uint32_t(in.waKlvs[i].key) << 16)) return Error::Corrupt;
    if (!zero(p + l.waOffset + l.waUsedBytes, l.captureOffset - l.waOffset - l.waUsedBytes) ||
        !zero(p + l.captureOffset, PageBytes)) return Error::Corrupt;
    for (unsigned index = 0; index < 2; ++index) {
        if (get32(p + offsetof(Ads, captureGlobal) + 4 * index) != capturePointer(r, l, in, index, CaptureKind::Global, 0)) return Error::Corrupt;
        for (unsigned c = 0; c < Classes; ++c) {
            const uint32_t cls = c < 6 ? capturePointer(r, l, in, index, CaptureKind::Class, c) : 0;
            const uint32_t instance = c < 6 ? capturePointer(r, l, in, index, CaptureKind::Instance, c) : 0;
            if (get32(p + offsetof(Ads, captureClass) + 4 * (index * Classes + c)) != cls ||
                get32(p + offsetof(Ads, captureInstance) + 4 * (index * Classes + c)) != instance) return Error::Corrupt;
        }
    }
    for (size_t i = 0; i < in.captureListCount; ++i) {
        const CaptureList &list = in.captureLists[i];
        uint32_t off = captureListOffset(l, in, i);
        if (get32(p + off) != list.regCount) return Error::Corrupt;
        off += sizeof(CaptureHeader);
        for (size_t j = 0; j < list.regCount; ++j, off += sizeof(Reg))
            if (!regSame(p + off, list.regs[j], false)) return Error::Corrupt;
    }
    if (!zero(p + l.captureOffset + l.captureUsedBytes, l.privateOffset - l.captureOffset - l.captureUsedBytes)) return Error::Corrupt;
    // Firmware legitimately changes saved register values, usage stats and its
    // private-data region after boot; active validation never expects those zero.
    if (!active && !zero(p + l.privateOffset, size_t(r.bytes - l.privateOffset))) return Error::Corrupt;
    return Error::None;
}
bool captureStructure(const Input &in, const XeGuCFirmware::Region &ads, const Layout &l, const Capture &cap) {
    if (cap.owner != in.owner || cap.epoch != in.epoch || cap.gtId != in.gtId || cap.gucClass >= Classes ||
        !l.classGoldenBytes[cap.gucClass] || !cap.provenanceCookie || cap.source.bytes > maxSourceBytes ||
        !regionShape(in, cap.source) || !regionPagesValid(cap.source) || cap.imageBytes != l.classGoldenBytes[cap.gucClass] ||
        (cap.imageOffset & (PageBytes - 1)) || cap.imageOffset > cap.source.bytes ||
        cap.imageBytes > cap.source.bytes - cap.imageOffset || regionsAlias(ads, cap.source) ||
        overlap(cap.source.dmaPages, cap.source.pageCount * sizeof(uint64_t), ads.cpu, size_t(ads.bytes)) ||
        cap.prime.ggttLrca != cap.source.ggtt + cap.imageOffset ||
        !cap.prime.kernelFence || !cap.switchedTo.kernelFence || cap.prime.kernelFence == cap.switchedTo.kernelFence ||
        cap.prime.timeline == cap.switchedTo.timeline ||
        !cap.prime.sequence || !cap.switchedTo.sequence || cap.prime.contextId >= UINT16_MAX ||
        cap.switchedTo.contextId >= UINT16_MAX || cap.prime.contextId == cap.switchedTo.contextId ||
        (cap.switchedTo.ggttLrca & (PageBytes - 1)) || cap.switchedTo.ggttLrca < minGgtt ||
        uint64_t(cap.switchedTo.ggttLrca) + cap.imageBytes > XeGuCFirmware::gucGgttTop ||
        addressOverlap(cap.prime.ggttLrca, cap.imageBytes, cap.switchedTo.ggttLrca, cap.imageBytes) ||
        addressOverlap(ads.ggtt, ads.bytes, cap.switchedTo.ggttLrca, cap.imageBytes)) return false;
    for (size_t i = 0; i < in.engineCount; ++i)
        if (in.engines[i].gucClass == cap.gucClass && in.engines[i].physicalInstance == cap.physicalInstance &&
            in.engines[i].logicalInstance == cap.logicalInstance) return true;
    return false;
}
}

Error calculateLayout(const Input &in, Layout &out) {
    if (!in.owner || !in.epoch || !in.snapshotCookie || !in.doorbellCountPerSqidi || in.doorbellCountPerSqidi > 256) return Error::Invalid;
    if (in.deviceId != MellowXe::targetDeviceId || in.gtId || in.graphicsVersion != 1270 ||
        (in.mediaVersion && in.mediaVersion != 1300)) return Error::Unsupported;
    if (in.firmware.release.packed() != 0x463500 || in.firmware.submission.packed() != 0x011a00 ||
        !in.firmware.privateDataBytes || in.firmware.privateDataBytes > maxPrivateBytes) return Error::FirmwareMismatch;
    if (!in.engineCount || in.engineCount > MaxEngines || !arraySpan(in.engines, in.engineCount, sizeof(Engine)) ||
        !in.captureListCount || in.captureListCount > MaxCaptureLists ||
        !arraySpan(in.captureLists, in.captureListCount, sizeof(CaptureList)) || in.waKlvCount != 2 ||
        !arraySpan(in.waKlvs, in.waKlvCount, sizeof(WaKlv))) return Error::Invalid;
    Layout l {}; l.regsetOffset = sizeof(Fixed);
    uint32_t masks[Classes] {}; uint8_t captureMask = 0;
    for (size_t i = 0; i < in.engineCount; ++i) {
        const Engine &e = in.engines[i];
        const uint32_t base = e.gucClass == 0 ? 0x2000 : e.gucClass == 3 ? 0x22000 : e.gucClass == 4 ? 0x1a000 : 0;
        if (!base || e.physicalInstance || e.logicalInstance || e.mmioBase != base ||
            e.regCount < 2 || e.regCount > MaxRegsPerList || !arraySpan(e.regs, e.regCount, sizeof(Reg))) return Error::Unsupported;
        if (masks[e.gucClass]) return Error::Invalid;
        masks[e.gucClass] = 1; captureMask |= uint8_t(1U << captureClass(e.gucClass));
        bool hws = false, imr = false;
        for (size_t j = 0; j < e.regCount; ++j) {
            const Reg &r = e.regs[j];
            if (!regValid(r, false)) return Error::Invalid;
            if (r.offset == base + 0x80 && !r.flags) hws = true;
            if (r.offset == base + 0xa8 && !r.flags) imr = true;
        }
        if (!hws || !imr) return Error::Unavailable;
        l.regsetUsedBytes += uint32_t(e.regCount * sizeof(Reg));
        l.enabledClasses |= uint16_t(1U << e.gucClass);
    }
    for (size_t c = 0; c < Classes; ++c) if (masks[c] != in.enabledMasks[c]) return Error::Invalid;
    l.regsetBytes = in.regsetReserveBytes ? in.regsetReserveBytes : l.regsetUsedBytes;
    if (l.regsetBytes < l.regsetUsedBytes || l.regsetBytes > MaxRegsetBytes || l.regsetBytes % sizeof(Reg) ||
        !alignPage(uint64_t(l.regsetOffset) + l.regsetBytes, l.goldenOffset)) return Error::Capacity;
    for (size_t c = 0; c < Classes; ++c) {
        if (!(l.enabledClasses & (1U << c))) continue;
        l.classGoldenOffset[c] = l.goldenOffset + l.goldenBytes;
        l.classGoldenBytes[c] = goldenSize(uint8_t(c));
        l.classStateBytes[c] = l.classGoldenBytes[c] - PageBytes - 96 * sizeof(uint32_t);
        l.goldenBytes += l.classGoldenBytes[c];
    }
    if (!alignPage(uint64_t(l.goldenOffset) + l.goldenBytes, l.waOffset)) return Error::Capacity;
    if (in.waKlvs[0].key == in.waKlvs[1].key) return Error::Invalid;
    for (size_t i = 0; i < in.waKlvCount; ++i)
        if ((in.waKlvs[i].key != WaBlockInterrupts && in.waKlvs[i].key != WaResetBbStack) ||
            in.waKlvs[i].valueDwords || in.waKlvs[i].values) return Error::Unsupported;
    l.waUsedBytes = 8;
    if (!alignPage(uint64_t(l.waOffset) + PageBytes, l.captureOffset)) return Error::Capacity;
    l.captureUsedBytes = PageBytes;
    bool listed[2][3][6] {};
    for (size_t i = 0; i < in.captureListCount; ++i) {
        const CaptureList &list = in.captureLists[i];
        const unsigned kind = unsigned(list.kind);
        if (list.index >= 2 || kind >= 3 || list.captureClass >= 6 ||
            (kind == 0 && list.captureClass) ||
            (kind && !(captureMask & (1U << list.captureClass))) ||
            listed[list.index][kind][list.captureClass] || list.regCount > MaxRegsPerList ||
            (list.regCount ? !arraySpan(list.regs, list.regCount, sizeof(Reg)) : list.regs != nullptr)) return Error::Invalid;
        listed[list.index][kind][list.captureClass] = true;
        for (size_t j = 0; j < list.regCount; ++j) if (!regValid(list.regs[j], true)) return Error::Invalid;
        l.captureUsedBytes += uint32_t(sizeof(CaptureHeader) + list.regCount * sizeof(Reg));
    }
    if (!listed[0][0][0]) return Error::Unavailable;
    for (unsigned c = 0; c < 6; ++c)
        if ((captureMask & (1U << c)) && (!listed[0][1][c] || !listed[0][2][c])) return Error::Unavailable;
    if (l.captureUsedBytes > MaxCaptureBytes || !alignPage(l.captureUsedBytes, l.captureBytes) ||
        !alignPage(uint64_t(l.captureOffset) + l.captureBytes, l.privateOffset) ||
        !alignPage(in.firmware.privateDataBytes, l.privateBytes) ||
        uint64_t(l.privateOffset) + l.privateBytes > maxRegionBytes) return Error::Capacity;
    l.totalBytes = l.privateOffset + l.privateBytes;
    if (inputAliases(in, &out, sizeof(out))) return Error::Alias;
    out = l;
    return Error::None;
}

Error validatePreload(const Input &in, const XeGuCFirmware::Region &r, const KernelAuthority &authority) {
    if (!authority.snapshotValid || !authority.snapshotValid(authority.opaque, in)) return Error::Unavailable;
    Layout l {};
    const Error error = calculateLayout(in, l);
    return error == Error::None ? inspect(in, r, l, authority, false, 0) : error;
}

Error Builder::buildPreload(const Input &in, const XeGuCFirmware::Region &region) {
    if (state_ != State::Empty) return Error::Busy;
    if (!authority_.snapshotValid || !authority_.regionOwned || !authority_.synchronize ||
        !authority_.snapshotValid(authority_.opaque, in)) return Error::Unavailable;
    Layout l {};
    Error error = calculateLayout(in, l);
    if (error != Error::None) return error;
    if (!regionShape(in, region) || region.bytes < l.totalBytes) return Error::Capacity;
    if (!authority_.regionOwned(authority_.opaque, in, region, Access::PreloadWrite)) return Error::Unavailable;
    if (!regionPagesValid(region)) return Error::Invalid;
    if (inputAliases(in, region.cpu, size_t(region.bytes)) ||
        inputAliases(in, region.dmaPages, region.pageCount * sizeof(uint64_t)) || inputAliases(in, this, sizeof(*this)) ||
        overlap(this, sizeof(*this), region.cpu, size_t(region.bytes)) ||
        overlap(this, sizeof(*this), region.dmaPages, region.pageCount * sizeof(uint64_t)) ||
        overlap(&region, sizeof(region), region.cpu, size_t(region.bytes)) ||
        overlap(&region, sizeof(region), region.dmaPages, region.pageCount * sizeof(uint64_t))) return Error::Alias;
    input_ = in; region_ = region; layout_ = l;
    // From the first CPU write, failure is terminal. The real owner must retain
    // backing and handle GuC reset/idle before reclaiming any resource.
    state_ = State::Failed;
    uint8_t *p = region_.cpu;
    clear(p, size_t(region_.bytes));
    put32(p + offsetof(Ads, schedulerPolicies), uint32_t(region_.ggtt + offsetof(Fixed, policies)));
    put32(p + offsetof(Ads, gtSystemInfo), uint32_t(region_.ggtt + offsetof(Fixed, systemInfo)));
    put32(p + offsetof(Ads, privateData), uint32_t(region_.ggtt + l.privateOffset));
    const size_t policy = offsetof(Fixed, policies);
    put32(p + policy + offsetof(Policies, dpcPromoteTime), 500000);
    put32(p + policy + offsetof(Policies, isValid), 1);
    put32(p + policy + offsetof(Policies, maxWorkItems), 15);
    for (size_t i = 0; i < Classes * Instances; ++i) p[offsetof(Fixed, systemInfo) + i] = Instances;
    for (size_t c = 0; c < Classes; ++c) {
        put32(p + offsetof(Fixed, systemInfo) + offsetof(SystemInfo, enabledMasks) + 4 * c, in.enabledMasks[c]);
        if (l.classGoldenBytes[c]) {
            put32(p + offsetof(Ads, goldenLrca) + 4 * c, uint32_t(region_.ggtt + l.classGoldenOffset[c]));
            put32(p + offsetof(Ads, engineStateBytes) + 4 * c, l.classStateBytes[c]);
        }
    }
    put32(p + offsetof(Fixed, systemInfo) + offsetof(SystemInfo, generic) + 8, in.doorbellCountPerSqidi);
    uint32_t regOff = l.regsetOffset;
    for (size_t i = 0; i < in.engineCount; ++i) {
        const Engine &e = in.engines[i];
        const size_t off = offsetof(Ads, regState) + (e.gucClass * Instances + e.physicalInstance) * sizeof(RegSet);
        put32(p + off, uint32_t(region_.ggtt + regOff)); put16(p + off + 4, uint16_t(e.regCount));
        p[offsetof(Fixed, systemInfo) + e.gucClass * Instances + e.logicalInstance] = e.physicalInstance;
        for (size_t j = 0; j < e.regCount; ++j, regOff += sizeof(Reg)) putReg(p + regOff, e.regs[j]);
    }
    put32(p + offsetof(Ads, waAddressLo), uint32_t(region_.ggtt + l.waOffset));
    put32(p + offsetof(Ads, waBytes), l.waUsedBytes);
    for (size_t i = 0; i < in.waKlvCount; ++i) put32(p + l.waOffset + 4 * i, uint32_t(in.waKlvs[i].key) << 16);
    for (unsigned index = 0; index < 2; ++index) {
        put32(p + offsetof(Ads, captureGlobal) + 4 * index, capturePointer(region_, l, in, index, CaptureKind::Global, 0));
        for (unsigned c = 0; c < 6; ++c) {
            put32(p + offsetof(Ads, captureClass) + 4 * (index * Classes + c), capturePointer(region_, l, in, index, CaptureKind::Class, c));
            put32(p + offsetof(Ads, captureInstance) + 4 * (index * Classes + c), capturePointer(region_, l, in, index, CaptureKind::Instance, c));
        }
    }
    for (size_t i = 0; i < in.captureListCount; ++i) {
        const CaptureList &list = in.captureLists[i];
        uint32_t off = captureListOffset(l, in, i);
        put32(p + off, uint32_t(list.regCount)); off += sizeof(CaptureHeader);
        for (size_t j = 0; j < list.regCount; ++j, off += sizeof(Reg)) putReg(p + off, list.regs[j]);
    }
    if (!authority_.synchronize(authority_.opaque, region_, 0, uint32_t(region_.bytes), Direction::CpuToDevice)) return Error::Synchronization;
    error = XeGuCAds::validatePreload(input_, region_, authority_);
    if (error != Error::None) return error;
    state_ = State::Preload;
    return Error::None;
}

Error Builder::validatePreload() const {
    if (state_ != State::Preload || publishedClasses_) return state_ == State::Failed ? Error::Quarantined : Error::Unavailable;
    return inspect(input_, region_, layout_, authority_, false, 0);
}

Error Builder::publishGolden(const Capture &cap) {
    if (state_ == State::Failed) return Error::Quarantined;
    if (state_ != State::Preload && state_ != State::PartiallyPublished) return Error::Busy;
    Error error = inspect(input_, region_, layout_, authority_, true, publishedClasses_);
    if (error != Error::None) { state_ = State::Failed; return error; }
    if (!regionShape(input_, cap.source)) return Error::Invalid;
    if (!authority_.captureValid || !authority_.synchronize ||
        !authority_.regionOwned(authority_.opaque, input_, region_, Access::GoldenWrite) ||
        !authority_.regionOwned(authority_.opaque, input_, cap.source, Access::CapturedSourceRead)) return Error::Unavailable;
    if (inputAliases(input_, cap.source.cpu, size_t(cap.source.bytes)) ||
        inputAliases(input_, cap.source.dmaPages, cap.source.pageCount * sizeof(uint64_t)) ||
        overlap(this, sizeof(*this), cap.source.cpu, size_t(cap.source.bytes)) ||
        overlap(this, sizeof(*this), cap.source.dmaPages, cap.source.pageCount * sizeof(uint64_t)) ||
        overlap(&cap, sizeof(cap), region_.cpu, size_t(region_.bytes)) ||
        overlap(&cap, sizeof(cap), region_.dmaPages, region_.pageCount * sizeof(uint64_t)) ||
        overlap(&cap, sizeof(cap), cap.source.cpu, size_t(cap.source.bytes)) ||
        overlap(&cap, sizeof(cap), cap.source.dmaPages, cap.source.pageCount * sizeof(uint64_t))) return Error::Alias;
    if (!captureStructure(input_, region_, layout_, cap)) return Error::Invalid;
    if (publishedClasses_ & (1U << cap.gucClass)) return Error::Busy;
    if (!authority_.captureValid(authority_.opaque, input_, cap)) return Error::Unavailable;
    if (!authority_.synchronize(authority_.opaque, cap.source, cap.imageOffset, cap.imageBytes, Direction::DeviceToCpu)) return Error::Synchronization;
    if (!authority_.snapshotValid(authority_.opaque, input_) ||
        !authority_.regionOwned(authority_.opaque, input_, region_, Access::GoldenWrite) ||
        !authority_.regionOwned(authority_.opaque, input_, cap.source, Access::CapturedSourceRead) ||
        !authority_.captureValid(authority_.opaque, input_, cap)) return Error::Unavailable;
    const uint8_t *source = cap.source.cpu + cap.imageOffset;
    // A valid saved context has the nonempty register command prefix after its
    // PPHWSP. This only rejects a zero placeholder; authority proves the capture.
    if (zero(source + PageBytes, 96 * sizeof(uint32_t))) return Error::Invalid;
    const uint32_t offset = layout_.classGoldenOffset[cap.gucClass];
    const Capture saved = cap; // input may be referenced by the caller after copy
    // Coherency callbacks can block while hardware resets or loses admission.
    // Re-sample physical ownership immediately before the first destination write.
    if (!authority_.snapshotValid(authority_.opaque, input_) ||
        !authority_.regionOwned(authority_.opaque, input_, region_, Access::GoldenWrite) ||
        !authority_.regionOwned(authority_.opaque, input_, saved.source, Access::CapturedSourceRead) ||
        !authority_.captureValid(authority_.opaque, input_, saved)) return Error::Unavailable;
    state_ = State::Failed;
    copy(region_.cpu + offset, source, cap.imageBytes);
    if (!authority_.synchronize(authority_.opaque, region_, offset, cap.imageBytes, Direction::CpuToDevice)) return Error::Synchronization;
    if (!authority_.snapshotValid(authority_.opaque, input_) ||
        !authority_.regionOwned(authority_.opaque, input_, region_, Access::Read) ||
        !authority_.regionOwned(authority_.opaque, input_, saved.source, Access::CapturedSourceRead) ||
        !authority_.captureValid(authority_.opaque, input_, saved) ||
        !same(region_.cpu + offset, source, saved.imageBytes)) return Error::Unavailable;
    captures_[saved.gucClass] = saved;
    publishedClasses_ |= uint16_t(1U << saved.gucClass);
    state_ = publishedClasses_ == layout_.enabledClasses ? State::PostLoaded : State::PartiallyPublished;
    if (state_ == State::PostLoaded) {
        error = validatePostLoad();
        if (error != Error::None) state_ = State::Failed;
        return error;
    }
    return Error::None;
}

Error Builder::validatePostLoad() const {
    if (state_ == State::Failed) return Error::Quarantined;
    if (state_ != State::PostLoaded || publishedClasses_ != layout_.enabledClasses || !authority_.captureValid ||
        !authority_.synchronize) return Error::Unavailable;
    Error error = inspect(input_, region_, layout_, authority_, true, publishedClasses_);
    if (error != Error::None) return error;
    for (size_t c = 0; c < Classes; ++c) {
        if (!(layout_.enabledClasses & (1U << c))) continue;
        const Capture &cap = captures_[c];
        if (!regionShape(input_, cap.source) ||
            !authority_.regionOwned(authority_.opaque, input_, cap.source, Access::CapturedSourceRead) ||
            inputAliases(input_, cap.source.cpu, size_t(cap.source.bytes)) ||
            inputAliases(input_, cap.source.dmaPages, cap.source.pageCount * sizeof(uint64_t)) ||
            overlap(this, sizeof(*this), cap.source.cpu, size_t(cap.source.bytes)) ||
            overlap(this, sizeof(*this), cap.source.dmaPages, cap.source.pageCount * sizeof(uint64_t)) ||
            !captureStructure(input_, region_, layout_, cap) ||
            !authority_.captureValid(authority_.opaque, input_, cap)) return Error::Unavailable;
        if (!authority_.synchronize(authority_.opaque, cap.source, cap.imageOffset, cap.imageBytes, Direction::DeviceToCpu)) return Error::Synchronization;
        if (!authority_.snapshotValid(authority_.opaque, input_) ||
            !authority_.regionOwned(authority_.opaque, input_, region_, Access::Read) ||
            !authority_.regionOwned(authority_.opaque, input_, cap.source, Access::CapturedSourceRead) ||
            !authority_.captureValid(authority_.opaque, input_, cap)) return Error::Unavailable;
        if (!same(region_.cpu + layout_.classGoldenOffset[c], cap.source.cpu + cap.imageOffset, cap.imageBytes)) return Error::Corrupt;
    }
    return Error::None;
}
}
