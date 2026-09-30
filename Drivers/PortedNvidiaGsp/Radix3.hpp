// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2018-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Mellow { namespace PortedNvidiaGsp {

// LibOS firmware image scatter table, not an NVIDIA GPU-VA page table/PTE.
constexpr uint64_t RadixPageBytes = 4096;
constexpr uint64_t RadixEntriesPerPage = 512;
constexpr uint64_t RadixPayloadBytesMax = 0x8000000000ULL; // 512^3 * 4096.

enum class Status : uint8_t {
    Ok, InvalidSize, InvalidLayout, InvalidArgument, InvalidCount,
    InvalidAddressWidth, InvalidAddress, BufferTooSmall, IntegerOverflow,
    OverlappingCpuSpans, AliasedDmaPages
};

struct Radix3Layout {
    uint64_t payloadBytes {};
    // Levels 0..2 contain addresses; level 3 contains the image bytes.
    uint64_t pageCount[4] {};
    uint64_t byteOffset[4] {};
    uint64_t tablePageCount {};
    uint64_t tableBytes {};
    uint64_t allocationBytes {};
};

// Count and offsets only. No memory allocation or physical-device admission.
// Exactly three table levels and one root page; out is unchanged on failure.
Status planRadix3(uint64_t payloadBytes, Radix3Layout &out);

struct DmaPages {
    // Genuine page addresses in logical allocation order, obtained from the
    // caller's GPU-DMA mapper (AT_GPU in upstream), never CPU VA or GPU VA.
    const uint64_t *addresses {};
    size_t count {};
};

// Produce the embedded-image form used for GSP-RM .fwimage. tablePages names
// the three table levels in layout order; payloadPages names the final image
// pages. Both counts must be exact. dmaAddressBits is the actual mapper/device
// address limit, 12..64, not a chipset-derived guess. Every complete 4 KiB page
// must fit it; distinct pages must not alias, including tables versus payload.
// Zero is mechanically representable; this function cannot attest DMA ownership.
//
// Output, payload, both address lists, and scratch must be disjoint CPU spans.
// Addresses/scratch must be naturally aligned uint64_t arrays. The caller keeps
// all inputs stable and owns output exclusively. Scratch needs at least
// tablePageCount + pageCount[3] words for allocation-free O(N log N) alias checks.
// Scratch is temporary and may change on failure. Firmware output is unchanged
// on every failure; unused entries and final image padding are zero on success.
// Bytes after layout.allocationBytes are untouched. No DMA synchronization,
// firmware validation, publication, boot, allocation or resource retirement occurs.
Status serializeRadix3(Radix3Layout layout, DmaPages tablePages,
                       DmaPages payloadPages, uint8_t dmaAddressBits,
                       const uint8_t *payload, size_t payloadBytes,
                       uint8_t *output, size_t outputBytes,
                       uint64_t *scratch, size_t scratchWords);

} } // namespace Mellow::PortedNvidiaGsp
