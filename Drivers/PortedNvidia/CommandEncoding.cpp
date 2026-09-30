// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2003-2022 NVIDIA CORPORATION & AFFILIATES
// Copyright (c) 2003-2023 NVIDIA CORPORATION & AFFILIATES
// Copyright (c) 2015-2025 NVIDIA Corporation
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#include "CommandEncoding.hpp"

namespace Mellow { namespace PortedNvidia {
namespace {
bool knownFormat(ChannelClass channel) {
    switch (channel) {
        case ChannelClass::MaxwellA:
        case ChannelClass::PascalA:
        case ChannelClass::VoltaA:
        case ChannelClass::TuringA:
        case ChannelClass::AmpereA:
        case ChannelClass::HopperA:
        case ChannelClass::BlackwellA:
        case ChannelClass::BlackwellB: return true;
    }
    return false;
}
bool extendedFormat(ChannelClass channel) {
    return channel == ChannelClass::HopperA || channel == ChannelClass::BlackwellA ||
           channel == ChannelClass::BlackwellB;
}
Status packGpfifoPushbuffer(uint64_t address, uint64_t bytes,
                           Sync sync, GpfifoEntry &out) {
    if ((address & 3U) != 0 || address > GpfifoAddressMax)
        return Status::InvalidAddress;
    if (bytes == 0 || (bytes & 3U) != 0 || bytes > GpfifoBytesMax)
        return Status::InvalidLength;
    if (bytes - 1 > GpfifoAddressMax - address) return Status::InvalidAddress;
    if (sync != Sync::Proceed && sync != Sync::Wait) return Status::InvalidSync;

    // GET is bits 31:2 of entry 0; GET_HI is bits 7:0 of entry 1.
    // LENGTH is bits 30:10 and counts dwords, SYNC is bit 31.
    GpfifoEntry encoded;
    encoded.lower = static_cast<uint32_t>(address);
    encoded.upper = static_cast<uint32_t>(address >> 32) |
                    (static_cast<uint32_t>(bytes >> 2) << 10) |
                    (static_cast<uint32_t>(sync) << 31);
    out = encoded;
    return Status::Ok;
}
} // namespace

Status encodeGpfifoPushbuffer(ChannelClass channel, uint64_t address,
                             uint64_t bytes, Sync sync, GpfifoEntry &out) {
    if (!knownFormat(channel)) return Status::UnknownClass;
    if (extendedFormat(channel)) return Status::UnsupportedOperation;
    return packGpfifoPushbuffer(address, bytes, sync, out);
}

Status encodeGpfifoNop(ChannelClass channel, GpfifoEntry &out) {
    if (!knownFormat(channel)) return Status::UnknownClass;
    // LENGTH=0 selects a control entry; OPCODE=Nop=0. Entry 0 is unused.
    out = {};
    return Status::Ok;
}

Status encodeGpfifoSegment(ChannelClass channel, uint64_t address, uint64_t bytes,
                          Sync sync, GpfifoSegment &out) {
    if (!knownFormat(channel)) return Status::UnknownClass;
    if (!extendedFormat(channel)) return Status::UnsupportedOperation;
    if ((address & 3U) != 0 || address > SegmentAddressMax)
        return Status::InvalidAddress;
    if (bytes == 0 || (bytes & 3U) != 0 || bytes > GpfifoBytesMax)
        return Status::InvalidLength;
    const uint64_t lowAddress = address & GpfifoAddressMax;
    if (bytes - 1 > GpfifoAddressMax - lowAddress) return Status::SegmentBoundary;
    GpfifoSegment encoded;
    const Status status = packGpfifoPushbuffer(lowAddress, bytes, sync,
                                             encoded.pushbuffer);
    if (status != Status::Ok) return status;
    // NVIDIA Hopper helper: VA[56:40] in control operand [24:8], opcode 4
    // in entry 1 [7:0], LENGTH=0. Blackwell inherits this UVM packing.
    encoded.setBase.lower = static_cast<uint32_t>(address >> 40) << 8;
    encoded.setBase.upper = 4;
    out = encoded;
    return Status::Ok;
}

void writeGpfifoLittleEndian(const GpfifoEntry &entry, uint8_t (&out)[8]) {
    for (uint32_t byte = 0; byte < 4; ++byte) {
        out[byte] = static_cast<uint8_t>(entry.lower >> (byte * 8));
        out[byte + 4] = static_cast<uint8_t>(entry.upper >> (byte * 8));
    }
}

Status encodeMethodHeader(ChannelClass channel, MethodMode mode,
                          uint32_t address, uint32_t subchannel,
                          uint32_t count, uint32_t &out) {
    if (!knownFormat(channel)) return Status::UnknownClass;
    if ((address & 3U) != 0 || address > MethodAddressMax)
        return Status::InvalidMethod;
    if (subchannel > 7) return Status::InvalidSubchannel;
    if (count > MethodCountMax) return Status::InvalidCount;
    const uint32_t index = address >> 2;
    switch (mode) {
        case MethodMode::Incrementing:
            if (count > 0x1000U - index) return Status::InvalidMethod;
            break;
        case MethodMode::OneIncrement:
            if (count > 1 && index == 0xFFFU) return Status::InvalidMethod;
            break;
        case MethodMode::NonIncrementing:
        case MethodMode::Immediate: break;
        default: return Status::InvalidMode;
    }
    // DMA address [11:0], reserved [12], subchannel [15:13],
    // count/immediate [28:16], secondary opcode [31:29].
    out = index | (subchannel << 13) | (count << 16) |
          (static_cast<uint32_t>(mode) << 29);
    return Status::Ok;
}

} } // namespace Mellow::PortedNvidia
