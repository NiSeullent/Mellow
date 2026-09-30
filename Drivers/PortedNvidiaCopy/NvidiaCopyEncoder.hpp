/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mellow contributors.
 * Hardware constants derived from NVIDIA class headers:
 * Copyright (c) 2019 NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2003-2022, 2024 NVIDIA CORPORATION & AFFILIATES.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure packet construction only: no MMIO, DMA mapping, firmware, submission,
// completion, GPU execution or Metal support is implemented here.
// Primary sources, inspected 2026-09-30:
// https://github.com/NVIDIA/open-gpu-doc/tree/master/classes/dma-copy
// Immutable equivalent headers and implementation contracts at revision
// e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb:
// https://github.com/NVIDIA/open-gpu-kernel-modules/tree/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class
// clb0b5.h: upper address 7:0 (40 total bits).
// clc0b5.h, clc1b5.h, clc3b5.h, clc5b5.h, clc6b5.h, clc7b5.h:
// upper 16:0 (49 bits), independently checked for each class.
// clc8b5.h: upper 24:0 (57 bits). clc9b5.h declares class C9B5;
// uvm_hal.c explicitly inherits Hopper's offset methods for C9B5 and
// then C9B5's methods for CAB5. clcab5.h adds a PREFETCH transfer enum;
// this encoder uses its separately documented NON_PIPELINED=2 value.
// open-gpu-doc/clc9b5.h and clcab5.h also explicitly document 57-bit
// offsets. Field width is an encoding limit,
// NOT proof of the device's actual usable virtual-address aperture.
// clb06f.h / clc56f.h: incrementing header opcode 31:29=1,
// count 28:16, subchannel 15:13, method word address 11:0.
// https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/include/nvif/push906f.h
// independently documents the method byte-offset >> 2 conversion.
// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_ampere_ce.c
// supplies DISABLE_PLC for C7B5; uvm_hal.c inherits it for C8/C9/CAB5.
// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_turing_ce.c
// documents that transfer FLUSH alone does not establish a memory barrier.
namespace MellowNvidia {

enum class CopyStatus {
    Ok, UnsupportedClass, InvalidChannel, InvalidLease, WrongOwner,
    StaleEpoch, NotResident, PermissionDenied, InvalidSize, OutOfRange,
    Overlap, AliasedAllocation, Capacity
};

// These are caller-supplied descriptions, not ownership or evidence receipts.
// A real backend must obtain them from its synchronized device/VM owner.
struct CopyChannelLease {
    uint64_t ownerId {}, vmId {}, epoch {};
    uint32_t ceClass {}, subchannel {};
    bool classBound {}, ordinaryChannel {}, nonConfidential {};
};
struct CopyBufferLease {
    uint64_t ownerId {}, vmId {}, epoch {}, allocationId {};
    uint64_t gpuVa {}, byteLength {};
    bool resident {}, readable {}, writable {};
};
struct VirtualLinearCopy {
    CopyChannelLease channel;
    CopyBufferLease source, destination;
    uint64_t sourceOffset {}, destinationOffset {}, byteCount {};
};

constexpr size_t virtualLinearCopyWordCount = 11;

namespace CopyDetail {
struct ClassTraits { unsigned addressBits; uint32_t launchExtra; };
inline bool classTraits(uint32_t ceClass, ClassTraits &out) {
    // Consume the exact CE class admitted/bound by the RM or backend owner.
    // No marketing-name, PCI ID, or GPU-family inference is performed.
    switch (ceClass) {
        case 0xB0B5: out = {40, 0}; return true;
        case 0xC0B5: case 0xC1B5: case 0xC3B5: case 0xC5B5: case 0xC6B5:
            out = {49, 0}; return true;
        case 0xC7B5: out = {49, 1U << 26}; return true;
        case 0xC8B5: case 0xC9B5: case 0xCAB5:
            out = {57, 1U << 26}; return true;
        default: return false;
    }
}
inline CopyStatus validateBuffer(const CopyBufferLease &b,
                                 const CopyChannelLease &c,
                                 uint64_t encodingLimit) {
    // Reserving GPU VA zero is this encoder's allocator/admission policy,
    // not a NVIDIA hardware requirement or a claim that VA zero is invalid.
    if (!b.ownerId || !b.vmId || !b.epoch || !b.allocationId ||
        !b.gpuVa || !b.byteLength) return CopyStatus::InvalidLease;
    if (b.ownerId != c.ownerId || b.vmId != c.vmId) return CopyStatus::WrongOwner;
    if (b.epoch != c.epoch) return CopyStatus::StaleEpoch;
    if (!b.resident) return CopyStatus::NotResident;
    // Validate the complete declared allocation using subtraction, avoiding
    // wraparound in gpuVa + byteLength and admitting a final byte at limit-1.
    if (b.gpuVa >= encodingLimit || b.byteLength > encodingLimit - b.gpuVa)
        return CopyStatus::OutOfRange;
    return CopyStatus::Ok;
}
constexpr uint32_t header(uint32_t method, uint32_t count, uint32_t subchannel) {
    return (1U << 29) | (count << 16) | (subchannel << 13) | (method >> 2);
}
}

// Caller preconditions, required also after a successful return:
// - Hold the real channel/VM/reset lock or equivalent lease for this snapshot;
//   ownerId/vmId/epoch identify the current owner, address space and reset epoch.
// - The chosen subchannel is already bound to the exact admitted CE class, on
//   an ordinary non-proxy, non-peer, non-confidential channel. Only ordinary
//   non-protected virtual allocations are supported; no SR-IOV proxy channels.
// - gpuVa ranges are mapped/pinned in that VM, lie within its actual aperture,
//   have no physical aliasing, and remain resident through observed completion.
// - Serialize CE context state. This packet sets render-enable TRUE and uses
//   virtual pitched, single-line, non-pipelined copy with remap disabled.
// - The submission owner must append a reviewed WFI + appropriate MEMBAR and
//   completion/fence sequence before consuming or releasing either allocation.
//   FLUSH and semaphore bits are intentionally zero. Encoding success says
//   nothing about GPU execution, visibility, native driver readiness or Metal.
// - output points to writable uint32_t storage; writtenWords does not alias it.
// Errors leave both output and writtenWords unchanged. This is a transactional
// construction guarantee, not atomic publication to concurrent consumers.
inline CopyStatus encodeVirtualLinearCopy(const VirtualLinearCopy &request,
                                         uint32_t *output, size_t capacityWords,
                                         size_t &writtenWords) {
    CopyDetail::ClassTraits traits {};
    const auto &c = request.channel;
    if (!CopyDetail::classTraits(c.ceClass, traits)) return CopyStatus::UnsupportedClass;
    if (!c.ownerId || !c.vmId || !c.epoch || c.subchannel > 7 ||
        !c.classBound || !c.ordinaryChannel || !c.nonConfidential)
        return CopyStatus::InvalidChannel;
    if (!request.byteCount || request.byteCount > UINT32_MAX)
        return CopyStatus::InvalidSize;
    const uint64_t limit = uint64_t(1) << traits.addressBits;
    auto status = CopyDetail::validateBuffer(request.source, c, limit);
    if (status != CopyStatus::Ok) return status;
    status = CopyDetail::validateBuffer(request.destination, c, limit);
    if (status != CopyStatus::Ok) return status;
    if (!request.source.readable || !request.destination.writable)
        return CopyStatus::PermissionDenied;
    if (request.sourceOffset > request.source.byteLength ||
        request.byteCount > request.source.byteLength - request.sourceOffset ||
        request.destinationOffset > request.destination.byteLength ||
        request.byteCount > request.destination.byteLength - request.destinationOffset)
        return CopyStatus::OutOfRange;
    if (request.source.allocationId == request.destination.allocationId &&
        (request.source.gpuVa != request.destination.gpuVa ||
         request.source.byteLength != request.destination.byteLength))
        return CopyStatus::AliasedAllocation;
    const uint64_t sourceVa = request.source.gpuVa + request.sourceOffset;
    const uint64_t destinationVa = request.destination.gpuVa + request.destinationOffset;
    // Both ends were already bounded by the allocation and class limit.
    // CE copy does not provide memmove semantics for overlapping regions.
    if (sourceVa < destinationVa + request.byteCount &&
        destinationVa < sourceVa + request.byteCount) return CopyStatus::Overlap;
    if (!output || capacityWords < virtualLinearCopyWordCount) return CopyStatus::Capacity;

    // All validation precedes the first output write. Snapshot all input data
    // in this local packet before publication, with no allocating operations.
    const uint32_t words[virtualLinearCopyWordCount] = {
        CopyDetail::header(0x25C, 1, c.subchannel), 1U, // SET_RENDER_ENABLE_C TRUE
        CopyDetail::header(0x400, 4, c.subchannel),
        uint32_t(sourceVa >> 32), uint32_t(sourceVa),
        uint32_t(destinationVa >> 32), uint32_t(destinationVa),
        CopyDetail::header(0x418, 1, c.subchannel), uint32_t(request.byteCount),
        CopyDetail::header(0x300, 1, c.subchannel),
        0x182U | traits.launchExtra // NON_PIPELINED=2, PITCH in/out at bits 7/8
    };
    for (size_t i = 0; i < virtualLinearCopyWordCount; ++i) output[i] = words[i];
    writtenWords = virtualLinearCopyWordCount;
    return CopyStatus::Ok;
}
}
