// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "Drivers/PortedNvidia/CommandEncoding.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <initializer_list>

using namespace Mellow::PortedNvidia;
namespace {
unsigned checks = 0;
void check(bool value, const char *description) {
    ++checks;
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", description);
        exit(1);
    }
}
constexpr ChannelClass formats[] = {
    ChannelClass::MaxwellA, ChannelClass::PascalA, ChannelClass::VoltaA,
    ChannelClass::TuringA, ChannelClass::AmpereA, ChannelClass::HopperA,
    ChannelClass::BlackwellA, ChannelClass::BlackwellB
};
bool extendedClass(ChannelClass format) {
    return format == ChannelClass::HopperA || format == ChannelClass::BlackwellA ||
           format == ChannelClass::BlackwellB;
}
Status encodeFetch(ChannelClass format, uint64_t address, uint64_t bytes,
                   Sync sync, GpfifoEntry &out) {
    if (!extendedClass(format))
        return encodeGpfifoPushbuffer(format, address, bytes, sync, out);
    GpfifoSegment segment;
    const Status status = encodeGpfifoSegment(format, address, bytes, sync, segment);
    if (status == Status::Ok) out = segment.pushbuffer;
    return status;
}
// A byte-oriented decoder keeps the oracle separate from the encoder's shifts.
uint64_t field(const uint8_t *bytes, unsigned first, unsigned width) {
    uint64_t result = 0;
    for (unsigned bit = 0; bit < width; ++bit) {
        const unsigned source = first + bit;
        if ((bytes[source / 8] & (1U << (source % 8))) != 0)
            result |= 1ULL << bit;
    }
    return result;
}
void unchangedGpfifo(Status expected, ChannelClass format, uint64_t address,
                     uint64_t bytes, Sync sync) {
    GpfifoEntry entry {0x12345678, 0x9ABCDEF0};
    check(encodeGpfifoPushbuffer(format, address, bytes, sync, entry) == expected,
          "invalid GPFIFO input classified");
    check(entry.lower == 0x12345678 && entry.upper == 0x9ABCDEF0,
          "GPFIFO failure preserves both output words");
}
void unchangedMethod(Status expected, ChannelClass format, MethodMode mode,
                     uint32_t address, uint32_t subchannel, uint32_t count) {
    uint32_t word = 0xDEADBEEF;
    check(encodeMethodHeader(format, mode, address, subchannel, count, word) == expected,
          "invalid method input classified");
    check(word == 0xDEADBEEF, "method failure preserves output");
}
void fixedVectors(ChannelClass format) {
    GpfifoEntry entry;
    check(encodeFetch(format, 0x1234567800ULL, 0x100, Sync::Wait, entry)
          == Status::Ok, "GPFIFO golden encoded");
    check(entry.lower == 0x34567800 && entry.upper == 0x80010012,
          "GPFIFO golden numeric words");
    uint8_t bytes[8];
    writeGpfifoLittleEndian(entry, bytes);
    const uint8_t golden[] = {0x00, 0x78, 0x56, 0x34, 0x12, 0x00, 0x01, 0x80};
    for (unsigned i = 0; i < 8; ++i) check(bytes[i] == golden[i], "GPFIFO golden byte order");
    check(encodeFetch(format, 0xFFFFFFFFFCULL, 4, Sync::Proceed, entry)
          == Status::Ok && entry.lower == 0xFFFFFFFC && entry.upper == 0x000004FF,
          "highest encodable address and four-byte fetch");
    check(encodeFetch(format, 0, 0x7FFFFC, Sync::Proceed, entry)
          == Status::Ok && entry.lower == 0 && entry.upper == 0x7FFFFC00,
          "largest dword length without SYNC contamination");
    check(encodeGpfifoNop(format, entry) == Status::Ok && entry.lower == 0 && entry.upper == 0,
          "control NOP is separate from pushbuffer length zero");

    uint32_t word;
    check(encodeMethodHeader(format, MethodMode::Incrementing, 0x1234, 5, 3, word)
          == Status::Ok && word == 0x2003A48D, "incrementing golden");
    check(encodeMethodHeader(format, MethodMode::NonIncrementing, 0x3FFC, 7, 0x1FFF, word)
          == Status::Ok && word == 0x7FFFEFFF, "nonincrementing maximum fields");
    check(encodeMethodHeader(format, MethodMode::Immediate, 0x50, 0, 0, word)
          == Status::Ok && word == 0x80000014, "zero immediate golden");
    check(encodeMethodHeader(format, MethodMode::OneIncrement, 0x3FF8, 7, 2, word)
          == Status::Ok && word == 0xA002EFFE, "increment-once final method golden");
    check(encodeMethodHeader(format, MethodMode::NonIncrementing, 8, 0, 0, word)
          == Status::Ok && word == 0x60000002, "upstream four-byte NOP header");
    check(encodeMethodHeader(format, MethodMode::Incrementing, 0, 0, 0x1000, word)
          == Status::Ok && word == 0x30000000, "incrementing address space endpoint");
}
void boundaries(ChannelClass format) {
    if (extendedClass(format)) {
        unchangedGpfifo(Status::UnsupportedOperation, format, 0, 4, Sync::Proceed);
        unchangedGpfifo(Status::UnsupportedOperation, format, 0x2000, 4, Sync::Wait);
    } else {
        for (uint64_t low = 1; low < 4; ++low) {
            unchangedGpfifo(Status::InvalidAddress, format, low, 4, Sync::Proceed);
            unchangedGpfifo(Status::InvalidLength, format, 0, low, Sync::Proceed);
        }
        unchangedGpfifo(Status::InvalidAddress, format, 0x10000000000ULL, 4, Sync::Proceed);
        unchangedGpfifo(Status::InvalidAddress, format, 0xFFFFFFFFFCULL, 8, Sync::Proceed);
        unchangedGpfifo(Status::InvalidAddress, format, UINT64_MAX, 4, Sync::Proceed);
        unchangedGpfifo(Status::InvalidLength, format, 0, 0, Sync::Proceed);
        unchangedGpfifo(Status::InvalidLength, format, 0, 0x800000, Sync::Proceed);
        unchangedGpfifo(Status::InvalidLength, format, 0, UINT64_MAX - 3, Sync::Proceed);
        unchangedGpfifo(Status::InvalidSync, format, 0, 4, static_cast<Sync>(2));
    }
    unchangedMethod(Status::InvalidMethod, format, MethodMode::Incrementing, 3, 0, 1);
    unchangedMethod(Status::InvalidMethod, format, MethodMode::Incrementing, 0x4000, 0, 1);
    unchangedMethod(Status::InvalidMethod, format, MethodMode::Incrementing, UINT32_MAX, 0, 1);
    unchangedMethod(Status::InvalidMethod, format, MethodMode::Incrementing, 0x3FFC, 0, 2);
    unchangedMethod(Status::InvalidMethod, format, MethodMode::OneIncrement, 0x3FFC, 0, 2);
    unchangedMethod(Status::InvalidSubchannel, format, MethodMode::Immediate, 0, 8, 0);
    unchangedMethod(Status::InvalidCount, format, MethodMode::NonIncrementing, 0, 0, 0x2000);
    unchangedMethod(Status::InvalidCount, format, MethodMode::Immediate, 0, 0, UINT32_MAX);
    for (uint8_t mode : {uint8_t(0), uint8_t(2), uint8_t(6), uint8_t(7), uint8_t(255)})
        unchangedMethod(Status::InvalidMode, format, static_cast<MethodMode>(mode), 0, 0, 1);
}
void independentDecode(ChannelClass format) {
    uint64_t random = 0xC46FB06F12345678ULL;
    for (unsigned iteration = 0; iteration < 2048; ++iteration) {
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        // Leave headroom for the maximal fetch range; do not silently truncate.
        const uint64_t address = (random % 0xFFFF800000ULL) & ~3ULL;
        const uint64_t dwords = 1 + ((random >> 40) % 0x1FFFFFULL);
        const Sync sync = (iteration & 1U) ? Sync::Wait : Sync::Proceed;
        GpfifoEntry entry;
        check(encodeFetch(format, address, dwords * 4, sync, entry)
              == Status::Ok, "generated GPFIFO admitted");
        uint8_t bytes[8];
        writeGpfifoLittleEndian(entry, bytes);
        const uint64_t decodedAddress = field(bytes, 2, 30) * 4 |
                                        (field(bytes, 32, 8) * 0x100000000ULL);
        check(decodedAddress == address, "independent GPFIFO address decode");
        check(field(bytes, 42, 21) * 4 == dwords * 4, "independent GPFIFO byte length decode");
        check(field(bytes, 63, 1) == static_cast<uint64_t>(sync), "independent GPFIFO sync decode");
        check(field(bytes, 0, 2) == 0 && field(bytes, 40, 2) == 0,
              "reserved/privilege/level/fetch fields remain clear");

        const uint32_t index = static_cast<uint32_t>((random >> 8) % 0x1000);
        const uint32_t subchannel = iteration % 8;
        const uint32_t count = static_cast<uint32_t>((random >> 32) % 0x2000);
        uint32_t word;
        check(encodeMethodHeader(format, MethodMode::NonIncrementing, index * 4,
                                 subchannel, count, word) == Status::Ok,
              "generated method header admitted");
        uint8_t packet[8] {};
        writeGpfifoLittleEndian({word, 0}, packet);
        check(field(packet, 0, 12) * 4 == index * 4 && field(packet, 12, 1) == 0,
              "independent method offset and reserved bit decode");
        check(field(packet, 13, 3) == subchannel && field(packet, 16, 13) == count &&
              field(packet, 29, 3) == 3, "independent subchannel count opcode decode");
    }
}
void extendedBase() {
    for (ChannelClass format : {ChannelClass::HopperA, ChannelClass::BlackwellA,
                                ChannelClass::BlackwellB}) {
        GpfifoSegment segment;
        check(encodeGpfifoSegment(format, 0x123451234567800ULL, 0x100, Sync::Wait, segment)
              == Status::Ok, "extended-base pair admitted");
        check(segment.setBase.lower == 0x01234500 && segment.setBase.upper == 4 &&
              segment.pushbuffer.lower == 0x34567800 && segment.pushbuffer.upper == 0x80010012,
              "extended-base independent golden words");
        uint8_t baseBytes[8], fetchBytes[8];
        writeGpfifoLittleEndian(segment.setBase, baseBytes);
        writeGpfifoLittleEndian(segment.pushbuffer, fetchBytes);
        const uint64_t decoded = (field(baseBytes, 8, 17) * 0x10000000000ULL) |
                                 (field(fetchBytes, 32, 8) * 0x100000000ULL) |
                                 (field(fetchBytes, 2, 30) * 4);
        check(decoded == 0x123451234567800ULL && field(baseBytes, 32, 8) == 4 &&
              field(baseBytes, 42, 21) == 0, "extended-base independent decode");
        check(encodeGpfifoSegment(format, 0x1FFFFFFFFFFFFFCULL, 4, Sync::Proceed, segment)
              == Status::Ok && segment.setBase.lower == 0x01FFFF00,
              "57-bit address field endpoint");
        check(encodeGpfifoSegment(format, 0x2000, 4, Sync::Proceed, segment)
              == Status::Ok && segment.setBase.lower == 0 && segment.setBase.upper == 4,
              "zero extended base is explicitly restored");
        for (uint64_t address : {0x1FFFFFFFFFCULL, 0x1FFFFFFFFFFFFFCULL}) {
            segment = {{11, 12}, {13, 14}};
            check(encodeGpfifoSegment(format, address, 8, Sync::Proceed, segment)
                  == Status::SegmentBoundary, "segment-crossing fetch rejected");
            check(segment.setBase.lower == 11 && segment.setBase.upper == 12 &&
                  segment.pushbuffer.lower == 13 && segment.pushbuffer.upper == 14,
                  "failed pair preserves both entries");
        }
        check(encodeGpfifoSegment(format, 0x200000000000000ULL, 4, Sync::Proceed, segment)
              == Status::InvalidAddress, "extended field overflow rejected");
        check(encodeGpfifoSegment(format, 0, 0, Sync::Proceed, segment)
              == Status::InvalidLength, "empty extended fetch rejected");
        check(encodeGpfifoSegment(format, 0, 4, static_cast<Sync>(3), segment)
              == Status::InvalidSync, "invalid extended sync rejected");
    }
    GpfifoSegment segment {{11, 12}, {13, 14}};
    check(encodeGpfifoSegment(ChannelClass::MaxwellA, 0, 4, Sync::Proceed, segment)
          == Status::UnsupportedOperation && segment.setBase.lower == 11 && segment.pushbuffer.lower == 13,
          "older format does not accept extended-base operation");
}
} // namespace
int main() {
    for (ChannelClass format : formats) {
        fixedVectors(format);
        boundaries(format);
        independentDecode(format);
    }
    extendedBase();
    const auto unknown = static_cast<ChannelClass>(0xDEAD);
    unchangedGpfifo(Status::UnknownClass, unknown, 0, 4, Sync::Proceed);
    unchangedMethod(Status::UnknownClass, unknown, MethodMode::Incrementing, 0, 0, 1);
    GpfifoEntry entry {1, 2};
    check(encodeGpfifoNop(unknown, entry) == Status::UnknownClass && entry.lower == 1 && entry.upper == 2,
          "uninspected control format rejected without output mutation");
    printf("PortedNvidia packet construction: %u checks passed; GPU execution NOT_RUN.\n", checks);
}
