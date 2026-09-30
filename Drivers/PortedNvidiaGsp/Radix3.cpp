// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2018-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#include "Radix3.hpp"

namespace Mellow { namespace PortedNvidiaGsp {
namespace {
struct CpuSpan { uintptr_t begin, end; };

bool span(const void *pointer, size_t bytes, CpuSpan &out) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    if (bytes > UINTPTR_MAX - begin) return false;
    out = {begin, begin + bytes};
    return true;
}
bool overlap(CpuSpan a, CpuSpan b) {
    return a.begin < b.end && b.begin < a.end;
}
bool sameLayout(const Radix3Layout &a, const Radix3Layout &b) {
    if (a.payloadBytes != b.payloadBytes || a.tablePageCount != b.tablePageCount ||
        a.tableBytes != b.tableBytes || a.allocationBytes != b.allocationBytes)
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (a.pageCount[i] != b.pageCount[i] || a.byteOffset[i] != b.byteOffset[i])
            return false;
    return true;
}
void swap(uint64_t &a, uint64_t &b) {
    const uint64_t value = a; a = b; b = value;
}
void sift(uint64_t *words, size_t root, size_t count) {
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && words[child] < words[child + 1]) ++child;
        if (words[root] >= words[child]) return;
        swap(words[root], words[child]);
        root = child;
    }
}
void heapSort(uint64_t *words, size_t count) {
    for (size_t i = count / 2; i > 0; --i) sift(words, i - 1, count);
    for (size_t remaining = count; remaining > 1; --remaining) {
        swap(words[0], words[remaining - 1]);
        sift(words, 0, remaining - 1);
    }
}
void writeAddress(uint8_t *out, uint64_t address) {
    for (unsigned byte = 0; byte < 8; ++byte)
        out[byte] = static_cast<uint8_t>(address >> (byte * 8));
}
bool validPage(uint64_t address, uint64_t addressMax) {
    return (address & (RadixPageBytes - 1)) == 0 &&
           address <= addressMax - (RadixPageBytes - 1);
}
} // namespace

Status planRadix3(uint64_t payloadBytes, Radix3Layout &out) {
    if (payloadBytes == 0 || payloadBytes > RadixPayloadBytesMax)
        return Status::InvalidSize;
    Radix3Layout candidate;
    candidate.payloadBytes = payloadBytes;
    // Subtract-first ceiling divisions avoid upstream's size + 4095 overflow.
    candidate.pageCount[3] = (payloadBytes - 1) / RadixPageBytes + 1;
    for (unsigned level = 3; level > 0; --level)
        candidate.pageCount[level - 1] =
            (candidate.pageCount[level] - 1) / RadixEntriesPerPage + 1;
    if (candidate.pageCount[0] != 1) return Status::InvalidSize;
    uint64_t precedingPages = 0;
    for (unsigned level = 1; level < 4; ++level) {
        precedingPages += candidate.pageCount[level - 1];
        candidate.byteOffset[level] = precedingPages * RadixPageBytes;
    }
    candidate.tablePageCount = precedingPages;
    candidate.tableBytes = candidate.byteOffset[3];
    candidate.allocationBytes = candidate.tableBytes +
                                candidate.pageCount[3] * RadixPageBytes;
    out = candidate;
    return Status::Ok;
}

Status serializeRadix3(Radix3Layout layout, DmaPages tablePages,
                       DmaPages payloadPages, uint8_t dmaAddressBits,
                       const uint8_t *payload, size_t payloadBytes,
                       uint8_t *output, size_t outputBytes,
                       uint64_t *scratch, size_t scratchWords) {
    Radix3Layout expected;
    if (planRadix3(layout.payloadBytes, expected) != Status::Ok ||
        !sameLayout(layout, expected)) return Status::InvalidLayout;
    if (dmaAddressBits < 12 || dmaAddressBits > 64)
        return Status::InvalidAddressWidth;
    if (layout.allocationBytes > SIZE_MAX || layout.tablePageCount > SIZE_MAX ||
        layout.pageCount[3] > SIZE_MAX) return Status::IntegerOverflow;
    if (tablePages.count != layout.tablePageCount ||
        payloadPages.count != layout.pageCount[3] ||
        payloadBytes != layout.payloadBytes) return Status::InvalidCount;
    if (outputBytes < layout.allocationBytes) return Status::BufferTooSmall;
    if (tablePages.count > SIZE_MAX - payloadPages.count)
        return Status::IntegerOverflow;
    const size_t totalPages = tablePages.count + payloadPages.count;
    if (totalPages > SIZE_MAX / sizeof(uint64_t)) return Status::IntegerOverflow;
    if (scratchWords < totalPages) return Status::BufferTooSmall;
    if (!tablePages.addresses || !payloadPages.addresses || !payload ||
        !output || !scratch) return Status::InvalidArgument;
    if ((reinterpret_cast<uintptr_t>(tablePages.addresses) % alignof(uint64_t)) ||
        (reinterpret_cast<uintptr_t>(payloadPages.addresses) % alignof(uint64_t)) ||
        (reinterpret_cast<uintptr_t>(scratch) % alignof(uint64_t)))
        return Status::InvalidArgument;

    CpuSpan spans[5];
    if (!span(output, static_cast<size_t>(layout.allocationBytes), spans[0]) ||
        !span(payload, payloadBytes, spans[1]) ||
        !span(tablePages.addresses, tablePages.count * sizeof(uint64_t), spans[2]) ||
        !span(payloadPages.addresses, payloadPages.count * sizeof(uint64_t), spans[3]) ||
        !span(scratch, totalPages * sizeof(uint64_t), spans[4]))
        return Status::IntegerOverflow;
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = i + 1; j < 5; ++j)
            if (overlap(spans[i], spans[j])) return Status::OverlappingCpuSpans;

    const uint64_t addressMax = dmaAddressBits == 64 ? UINT64_MAX :
                               (uint64_t(1) << dmaAddressBits) - 1;
    // Validate every full page interval before touching even the scratch span.
    for (size_t i = 0; i < tablePages.count; ++i)
        if (!validPage(tablePages.addresses[i], addressMax)) return Status::InvalidAddress;
    for (size_t i = 0; i < payloadPages.count; ++i)
        if (!validPage(payloadPages.addresses[i], addressMax)) return Status::InvalidAddress;
    for (size_t i = 0; i < tablePages.count; ++i) scratch[i] = tablePages.addresses[i];
    for (size_t i = 0; i < payloadPages.count; ++i)
        scratch[tablePages.count + i] = payloadPages.addresses[i];
    heapSort(scratch, totalPages);
    for (size_t i = 1; i < totalPages; ++i)
        if (scratch[i - 1] == scratch[i]) return Status::AliasedDmaPages;

    // All fallible checks are finished. Populate private CPU bytes, without any
    // device publication or DMA/cache operation. Unused entries remain zero.
    for (size_t i = 0; i < static_cast<size_t>(layout.tableBytes); ++i) output[i] = 0;
    for (unsigned level = 0; level < 2; ++level) {
        const size_t childBegin = static_cast<size_t>(layout.byteOffset[level + 1] / RadixPageBytes);
        const size_t entryBegin = static_cast<size_t>(layout.byteOffset[level]);
        for (size_t i = 0; i < layout.pageCount[level + 1]; ++i)
            writeAddress(output + entryBegin + i * sizeof(uint64_t),
                         tablePages.addresses[childBegin + i]);
    }
    const size_t leafBegin = static_cast<size_t>(layout.byteOffset[2]);
    for (size_t i = 0; i < payloadPages.count; ++i)
        writeAddress(output + leafBegin + i * sizeof(uint64_t), payloadPages.addresses[i]);
    const size_t payloadBegin = static_cast<size_t>(layout.byteOffset[3]);
    for (size_t i = 0; i < payloadBytes; ++i) output[payloadBegin + i] = payload[i];
    for (size_t i = payloadBegin + payloadBytes;
         i < static_cast<size_t>(layout.allocationBytes); ++i) output[i] = 0;
    return Status::Ok;
}
} } // namespace Mellow::PortedNvidiaGsp
