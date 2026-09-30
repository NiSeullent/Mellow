// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2003-2022 NVIDIA CORPORATION & AFFILIATES
// Copyright (c) 2003-2023 NVIDIA CORPORATION & AFFILIATES
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#pragma once
#include <stdint.h>

namespace Mellow { namespace PortedNvidia {

// These are packet format identifiers, not PCI device admission or GPU support.
enum class ChannelClass : uint32_t {
    MaxwellA = 0xB06F, PascalA = 0xC06F, VoltaA = 0xC36F,
    TuringA = 0xC46F, AmpereA = 0xC56F, HopperA = 0xC86F,
    BlackwellA = 0xC96F, BlackwellB = 0xCA6F
};
enum class Status : uint8_t {
    Ok, UnknownClass, InvalidAddress, InvalidLength, InvalidSync,
    InvalidMethod, InvalidSubchannel, InvalidCount, InvalidMode,
    UnsupportedOperation, SegmentBoundary
};
enum class Sync : uint8_t { Proceed = 0, Wait = 1 };
enum class MethodMode : uint8_t {
    Incrementing = 1, NonIncrementing = 3, Immediate = 4, OneIncrement = 5
};
struct GpfifoEntry { uint32_t lower {}, upper {}; };
struct GpfifoSegment { GpfifoEntry setBase {}, pushbuffer {}; };

constexpr uint64_t GpfifoAddressMax = 0xFFFFFFFFFFULL; // Encoded GET: 40 bits.
constexpr uint64_t GpfifoBytesMax = 0x7FFFFCULL;       // 21-bit dword count.
constexpr uint64_t SegmentAddressMax = 0x1FFFFFFFFFFFFFFULL; // 17+40 bits.
constexpr uint32_t MethodAddressMax = 0x3FFCU;        // 12-bit dword address.
constexpr uint32_t MethodCountMax = 0x1FFFU;

// Adapted from NVIDIA's Turing UVM helper and the pinned class formats.
// Emits unconditional fetch, MAIN level, USER/reserved bit 8 clear. The whole
// byte range must fit the encoding; Hopper/Blackwell are rejected here and must
// use encodeGpfifoSegment so previous extended-base state cannot be hidden.
// No allocation, VM ownership, firmware,
// command validation, memory barrier, ring publication or GPU execution occurs.
// Output is unchanged on failure. Zero length uses a separate control NOP.
Status encodeGpfifoPushbuffer(ChannelClass, uint64_t gpuVirtualAddress,
                             uint64_t byteLength, Sync, GpfifoEntry &out);
Status encodeGpfifoNop(ChannelClass, GpfifoEntry &out);
void writeGpfifoLittleEndian(const GpfifoEntry &, uint8_t (&out)[8]);

// Hopper/Blackwell require a SET_PB_SEGMENT_EXTENDED_BASE control entry before
// fetching from this 40-bit segment. Emit both entries, including a zero base,
// without assuming inherited channel state. Reject fetches crossing a segment.
// Caller publishes them in order under exclusive channel ownership; this
// function performs no publication. Output is unchanged on failure.
Status encodeGpfifoSegment(ChannelClass, uint64_t gpuVirtualAddress,
                          uint64_t byteLength, Sync, GpfifoSegment &out);

// byteAddress is an engine method offset, not a GPU virtual address.
// countOrImmediate is a payload dword count, or 13-bit data for Immediate.
// Zero counts are representable (upstream uses NONINC count 0 for a NOP).
// Advancing methods may not wrap the 12-bit address under this adapter's policy.
// Output is unchanged on failure; method/class binding and payload validation
// remain the caller's responsibility.
Status encodeMethodHeader(ChannelClass, MethodMode, uint32_t byteAddress,
                          uint32_t subchannel, uint32_t countOrImmediate,
                          uint32_t &out);

} } // namespace Mellow::PortedNvidia
