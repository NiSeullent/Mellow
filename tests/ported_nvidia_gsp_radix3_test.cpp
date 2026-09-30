// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "Drivers/PortedNvidiaGsp/Radix3.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

using namespace Mellow::PortedNvidiaGsp;
namespace {
unsigned checks = 0;
void check(bool value, const char *description) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", description); exit(1); }
}
uint64_t readLe(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned i = 8; i > 0; --i) value = value * 256 + bytes[i - 1];
    return value;
}
bool allBytes(const uint8_t *bytes, size_t count, uint8_t value) {
    for (size_t i = 0; i < count; ++i) if (bytes[i] != value) return false;
    return true;
}
void layoutVectors() {
    struct Vector { uint64_t bytes, pages[4], offsets[4], tables, allocation; };
    const Vector vectors[] = {
        {1, {1, 1, 1, 1}, {0, 4096, 8192, 12288}, 3, 16384},
        {4096, {1, 1, 1, 1}, {0, 4096, 8192, 12288}, 3, 16384},
        {4097, {1, 1, 1, 2}, {0, 4096, 8192, 12288}, 3, 20480},
        {0x200000, {1, 1, 1, 512}, {0, 4096, 8192, 12288}, 3, 2109440},
        {0x200001, {1, 1, 2, 513}, {0, 4096, 8192, 16384}, 4, 2117632},
        {0x40000001, {1, 2, 513, 262145}, {0, 4096, 12288, 2113536}, 516, 1075859456},
        {0x8000000000ULL, {1, 512, 262144, 134217728},
         {0, 4096, 2101248, 1075843072}, 262657, 550831656960ULL}
    };
    for (const Vector &v : vectors) {
        Radix3Layout layout;
        check(planRadix3(v.bytes, layout) == Status::Ok, "fixed size planned");
        check(layout.payloadBytes == v.bytes && layout.tablePageCount == v.tables &&
              layout.tableBytes == v.offsets[3] && layout.allocationBytes == v.allocation,
              "independent fixed allocation vector");
        check(memcmp(layout.pageCount, v.pages, sizeof(v.pages)) == 0 &&
              memcmp(layout.byteOffset, v.offsets, sizeof(v.offsets)) == 0,
              "independent fixed tree counts and offsets");
    }
    for (uint64_t bytes : {uint64_t(0), uint64_t(0x8000000001ULL), uint64_t(UINT64_MAX)}) {
        Radix3Layout unchanged;
        unchanged.payloadBytes = 0xAABBCCDD;
        unchanged.pageCount[2] = 123;
        const Radix3Layout before = unchanged;
        check(planRadix3(bytes, unchanged) == Status::InvalidSize, "oversized or empty image rejected");
        check(memcmp(&unchanged, &before, sizeof(unchanged)) == 0, "planner failure leaves output unchanged");
    }
}
struct Fixture {
    Radix3Layout layout;
    uint64_t tables[3] {0x1000, 0x5000, 0x9000};
    uint64_t data[1] {0x21000};
    uint8_t payload[5] {0x7F, 'E', 'L', 'F', 0xAB}; // Synthetic bytes, no firmware image.
    std::vector<uint8_t> output = std::vector<uint8_t>(16384 + 16, 0xA5);
    uint64_t scratch[4] {};
    Fixture() { check(planRadix3(5, layout) == Status::Ok, "fixture planned"); }
    Status encode(uint8_t bits = 32) {
        return serializeRadix3(layout, {tables, 3}, {data, 1}, bits,
                               payload, 5, output.data(), output.size(), scratch, 4);
    }
    void rejected(Status result, Status expected) {
        check(result == expected, "invalid serializer input classified");
        check(allBytes(output.data(), output.size(), 0xA5), "failure preserves entire firmware output");
    }
};
void fixedImageVector() {
    Fixture f;
    check(f.encode() == Status::Ok, "small image serialized");
    const uint8_t root[8] = {0x00, 0x50, 0, 0, 0, 0, 0, 0};
    const uint8_t middle[8] = {0x00, 0x90, 0, 0, 0, 0, 0, 0};
    const uint8_t leaf[8] = {0x00, 0x10, 0x02, 0, 0, 0, 0, 0};
    check(memcmp(f.output.data(), root, 8) == 0 &&
          memcmp(f.output.data() + 4096, middle, 8) == 0 &&
          memcmp(f.output.data() + 8192, leaf, 8) == 0,
          "golden bytes contain raw addresses without GPU PTE flags or shifts");
    check(allBytes(f.output.data() + 8, 4088, 0) &&
          allBytes(f.output.data() + 4104, 4088, 0) &&
          allBytes(f.output.data() + 8200, 4088, 0), "unused table entries are zero");
    check(memcmp(f.output.data() + 12288, f.payload, 5) == 0 &&
          allBytes(f.output.data() + 12293, 4091, 0), "image bytes and last-page padding");
    check(allBytes(f.output.data() + 16384, 16, 0xA5), "output beyond allocation is untouched");

    Fixture high;
    high.data[0] = 0x0123456789ABC000ULL;
    check(high.encode(64) == Status::Ok, "full untruncated DMA address accepted with declared 64-bit mapper");
    const uint8_t fullAddress[] = {0x00, 0xC0, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    check(memcmp(high.output.data() + 8192, fullAddress, 8) == 0, "64-bit golden address byte order");
    Fixture last;
    last.data[0] = 0xFFFFFFFFFFFFF000ULL;
    check(last.encode(64) == Status::Ok && readLe(last.output.data() + 8192) == last.data[0],
          "last complete 64-bit DMA page has no arithmetic wrap");
    Fixture zero;
    zero.data[0] = 0;
    check(zero.encode() == Status::Ok, "mechanical raw-zero encoding does not invent DMA admission policy");
}
void splitLeafVector() {
    Radix3Layout layout;
    check(planRadix3(0x200001, layout) == Status::Ok, "513 image pages planned");
    const uint64_t tables[] = {0x7000, 0x3000, 0xB000, 0x5000};
    std::vector<uint64_t> data(513), scratch(517);
    std::vector<uint8_t> payload(0x200001), output(2117632, 0xA5);
    for (size_t i = 0; i < data.size(); ++i) data[i] = 0x100000 + (512 - i) * 8192;
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>((i * 17 + 3) % 256);
    check(serializeRadix3(layout, {tables, 4}, {data.data(), data.size()}, 32,
                         payload.data(), payload.size(), output.data(), output.size(),
                         scratch.data(), scratch.size()) == Status::Ok,
          "noncontiguous reverse-order DMA pages serialized");
    check(readLe(output.data()) == 0x3000 && readLe(output.data() + 4096) == 0xB000 &&
          readLe(output.data() + 4104) == 0x5000, "fixed root and two leaf-page references");
    bool entriesMatch = true;
    for (size_t i = 0; i < 513; ++i)
        entriesMatch = entriesMatch && readLe(output.data() + 8192 + i * 8) == data[i];
    check(entriesMatch && readLe(output.data() + 12288) == 0x100000,
          "512-entry leaf transition keeps original logical payload order after alias sort");
    check(allBytes(output.data() + 12304, 4080, 0) &&
          readLe(output.data() + 12296) == 0, "second leaf unused entries zero");
    check(memcmp(output.data() + 16384, payload.data(), payload.size()) == 0 &&
          allBytes(output.data() + 2113537, 4095, 0), "multi-leaf payload and padding preserve exact bytes");
    bool sorted = true;
    for (size_t i = 1; i < scratch.size(); ++i) sorted = sorted && scratch[i - 1] < scratch[i];
    check(sorted, "scratch alias audit sorts independently without changing input page lists");
    data[512] = data[3];
    std::fill(output.begin(), output.end(), 0xA5);
    check(serializeRadix3(layout, {tables, 4}, {data.data(), data.size()}, 32,
                         payload.data(), payload.size(), output.data(), output.size(),
                         scratch.data(), scratch.size()) == Status::AliasedDmaPages &&
          allBytes(output.data(), output.size(), 0xA5), "duplicate across different leaf pages rejected transactionally");
}
void invalidInputs() {
    for (unsigned field = 0; field < 12; ++field) {
        Fixture f;
        if (field < 4) ++f.layout.pageCount[field];
        else if (field < 8) ++f.layout.byteOffset[field - 4];
        else if (field == 8) ++f.layout.tablePageCount;
        else if (field == 9) ++f.layout.tableBytes;
        else if (field == 10) ++f.layout.allocationBytes;
        else f.layout.payloadBytes = 0;
        f.rejected(f.encode(), Status::InvalidLayout);
    }
    for (uint8_t bits : {uint8_t(0), uint8_t(11), uint8_t(65), uint8_t(255)}) {
        Fixture f; f.rejected(f.encode(bits), Status::InvalidAddressWidth);
    }
    for (unsigned i = 0; i < 4; ++i) {
        Fixture f;
        if (i < 3) ++f.tables[i]; else ++f.data[0];
        f.rejected(f.encode(), Status::InvalidAddress);
    }
    { Fixture f; f.data[0] = 0x100000000ULL; f.rejected(f.encode(), Status::InvalidAddress); }
    { Fixture f; f.tables[2] = 0x100000000ULL; f.rejected(f.encode(), Status::InvalidAddress); }
    { Fixture f; f.rejected(f.encode(12), Status::InvalidAddress); }
    for (unsigned i = 0; i < 3; ++i) {
        Fixture f;
        if (i == 0) f.tables[2] = f.tables[0];
        else f.data[0] = f.tables[i];
        f.rejected(f.encode(), Status::AliasedDmaPages);
    }
    for (unsigned which = 0; which < 4; ++which) {
        Fixture f;
        f.rejected(serializeRadix3(f.layout, {f.tables, which == 0 ? 2U : 3U},
                   {f.data, which == 1 ? 2U : 1U}, 32, f.payload, which == 2 ? 4U : 5U,
                   f.output.data(), f.output.size(), f.scratch, which == 3 ? 3U : 4U),
                   which == 3 ? Status::BufferTooSmall : Status::InvalidCount);
    }
    { Fixture f; f.rejected(serializeRadix3(f.layout, {f.tables, 3}, {f.data, 1}, 32,
        f.payload, 5, f.output.data(), 16383, f.scratch, 4), Status::BufferTooSmall); }
    for (unsigned which = 0; which < 5; ++which) {
        Fixture f;
        f.rejected(serializeRadix3(f.layout, {which == 0 ? nullptr : f.tables, 3},
            {which == 1 ? nullptr : f.data, 1}, 32, which == 2 ? nullptr : f.payload, 5,
            which == 3 ? nullptr : f.output.data(), f.output.size(),
            which == 4 ? nullptr : f.scratch, 4), Status::InvalidArgument);
    }
    for (unsigned which = 0; which < 3; ++which) {
        Fixture f;
        f.rejected(serializeRadix3(f.layout,
            {which == 0 ? reinterpret_cast<const uint64_t *>(f.payload + 1) : f.tables, 3},
            {which == 1 ? reinterpret_cast<const uint64_t *>(f.payload + 1) : f.data, 1},
            32, f.payload, 5, f.output.data(), f.output.size(),
            which == 2 ? reinterpret_cast<uint64_t *>(f.payload + 1) : f.scratch, 4),
            Status::InvalidArgument);
    }
}
void cpuSpanGuards() {
    for (unsigned which = 0; which < 6; ++which) {
        Fixture f;
        const uint8_t *payload = which == 0 ? f.output.data() + 17 : f.payload;
        const uint64_t *tables = which == 1 ? reinterpret_cast<const uint64_t *>(f.output.data()) : f.tables;
        const uint64_t *data = which == 2 ? f.tables : f.data;
        uint64_t *scratch = f.scratch;
        if (which == 3) scratch = reinterpret_cast<uint64_t *>(f.output.data());
        if (which == 4) scratch = f.tables;
        if (which == 5) payload = reinterpret_cast<const uint8_t *>(f.data);
        f.rejected(serializeRadix3(f.layout, {tables, 3}, {data, 1}, 32, payload, 5,
            f.output.data(), f.output.size(), scratch, 4), Status::OverlappingCpuSpans);
    }
    for (unsigned which = 0; which < 4; ++which) {
        Fixture f;
        const uint64_t *tables = which == 0 ? reinterpret_cast<const uint64_t *>(UINTPTR_MAX - 7) : f.tables;
        const uint8_t *payload = which == 1 ? reinterpret_cast<const uint8_t *>(UINTPTR_MAX - 3) : f.payload;
        uint8_t *output = which == 2 ? reinterpret_cast<uint8_t *>(UINTPTR_MAX - 8191) : f.output.data();
        uint64_t *scratch = which == 3 ? reinterpret_cast<uint64_t *>(UINTPTR_MAX - 7) : f.scratch;
        f.rejected(serializeRadix3(f.layout, {tables, 3}, {f.data, 1}, 32,
            payload, 5, output, f.output.size(), scratch, 4), Status::IntegerOverflow);
    }
}
} // namespace
int main() {
    layoutVectors(); fixedImageVector(); splitLeafVector(); invalidInputs(); cpuSpanGuards();
    printf("ported NVIDIA GSP Radix3: %u checks passed (software serialization only)\n", checks);
    return 0;
}
