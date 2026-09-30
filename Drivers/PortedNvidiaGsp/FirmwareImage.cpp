// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2018-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2017-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#include "FirmwareImage.hpp"

namespace Mellow { namespace PortedNvidiaGsp {
namespace {
constexpr size_t ElfHeaderBytes = 64, SectionHeaderBytes = 64, ProgramHeaderBytes = 56;
constexpr uint32_t SectionNull = 0, SectionStrings = 3, SectionNoBits = 8;
constexpr uint64_t SectionCompressed = 0x800;
struct Span { uintptr_t begin, end; };
struct Range { uint64_t offset, size; };
struct Section {
    uint32_t name, type;
    uint64_t flags, offset, size, alignment;
};
bool cpuSpan(const void *data, size_t bytes, Span &out) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    if (bytes > UINTPTR_MAX - begin) return false;
    out = {begin, begin + bytes};
    return true;
}
bool overlaps(Span a, Span b) { return a.begin < b.end && b.begin < a.end; }
bool inFile(Range range, size_t bytes) {
    return range.offset <= bytes && range.size <= bytes - range.offset;
}
// Every nonempty range passed here has already been checked against the file.
bool overlaps(Range a, Range b) {
    return a.size && b.size && a.offset < b.offset + b.size && b.offset < a.offset + a.size;
}
uint64_t le(const uint8_t *bytes, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}
Section section(const uint8_t *table, size_t index) {
    const uint8_t *bytes = table + index * SectionHeaderBytes;
    return {static_cast<uint32_t>(le(bytes, 4)), static_cast<uint32_t>(le(bytes + 4, 4)),
            le(bytes + 8, 8), le(bytes + 24, 8), le(bytes + 32, 8), le(bytes + 48, 8)};
}
bool fileContents(Section value) { return value.type != SectionNull && value.type != SectionNoBits; }
bool equal(FirmwareText a, FirmwareText b) {
    if (a.size != b.size) return false;
    for (size_t i = 0; i < a.size; ++i) if (a.data[i] != b.data[i]) return false;
    return true;
}
bool textValid(FirmwareText value) {
    if (!value.data || !value.size) return false;
    for (size_t i = 0; i < value.size; ++i) if (value.data[i] == '\0') return false;
    return true;
}
bool starts(FirmwareText value, const char *prefix, size_t bytes) {
    if (value.size <= bytes) return false;
    for (size_t i = 0; i < bytes; ++i) if (value.data[i] != prefix[i]) return false;
    return true;
}
FirmwareText name(const uint8_t *strings, size_t bytes, uint32_t index) {
    if (index >= bytes) return {};
    size_t end = index;
    while (end < bytes && strings[end]) ++end;
    if (end == bytes) return {};
    return {reinterpret_cast<const char *>(strings + index), end - index};
}
FirmwareBytes view(FirmwareBytes container, Section value) {
    return {container.data + static_cast<size_t>(value.offset), static_cast<size_t>(value.size)};
}
} // namespace

FirmwareStatus extractFirmwareImage(FirmwareBytes container,
                                    FirmwareSelection selection,
                                    FirmwareParseLimits limits,
                                    FirmwareImageView &out) {
    if (!container.data || !container.size || !selection.expectedRelease.data ||
        !selection.signatureSectionName.data || !limits.maxSections ||
        !limits.maxStringTableBytes) return FirmwareStatus::InvalidArgument;
    Span input, release, signature, output;
    if (!cpuSpan(container.data, container.size, input) ||
        !cpuSpan(selection.expectedRelease.data, selection.expectedRelease.size, release) ||
        !cpuSpan(selection.signatureSectionName.data, selection.signatureSectionName.size, signature) ||
        !cpuSpan(&out, sizeof(out), output)) return FirmwareStatus::IntegerOverflow;
    if (overlaps(output, input) || overlaps(output, release) || overlaps(output, signature))
        return FirmwareStatus::OutputAliasesInput;
    // Match the upstream diagnostic bound and 32-byte HAL-selected name buffer.
    if (selection.expectedRelease.size > 62 || selection.signatureSectionName.size > 31 ||
        !textValid(selection.expectedRelease) || !textValid(selection.signatureSectionName))
        return FirmwareStatus::InvalidArgument;
    const FirmwareText selectedName = selection.signatureSectionName;
    constexpr char signaturePrefix[] = ".fwsignature_";
    constexpr char ccPrefix[] = ".fwsignature_cc_";
    if (!starts(selectedName, signaturePrefix, sizeof(signaturePrefix) - 1) ||
        (selectedName.size >= sizeof(ccPrefix) - 1 &&
         equal({selectedName.data, sizeof(ccPrefix) - 1}, {ccPrefix, sizeof(ccPrefix) - 1}) &&
         selectedName.size == sizeof(ccPrefix) - 1)) return FirmwareStatus::InvalidArgument;
    if (container.size < ElfHeaderBytes) return FirmwareStatus::Truncated;
    const uint8_t *header = container.data;
    if (header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F')
        return FirmwareStatus::InvalidElf;
    if (header[4] != 2 || header[5] != 1) return FirmwareStatus::UnsupportedElf;
    if (header[6] != 1 || le(header + 20, 4) != 1 || le(header + 52, 2) != ElfHeaderBytes ||
        le(header + 58, 2) != SectionHeaderBytes) return FirmwareStatus::InvalidElf;
    const uint64_t sectionOffset = le(header + 40, 8);
    const uint32_t sectionCount = static_cast<uint32_t>(le(header + 60, 2));
    const uint32_t stringsIndex = static_cast<uint32_t>(le(header + 62, 2));
    const uint32_t programCount = static_cast<uint32_t>(le(header + 56, 2));
    if (!sectionCount || sectionCount >= 0xFF00 || stringsIndex >= 0xFF00 || programCount == 0xFFFF)
        return FirmwareStatus::UnsupportedElf;
    if (!stringsIndex || stringsIndex >= sectionCount) return FirmwareStatus::InvalidElf;
    if (sectionCount > limits.maxSections) return FirmwareStatus::ResourceLimit;
    const Range headerRange {0, ElfHeaderBytes};
    const Range sectionTable {sectionOffset, uint64_t(sectionCount) * SectionHeaderBytes};
    if (!inFile(sectionTable, container.size)) return FirmwareStatus::Truncated;
    if (overlaps(headerRange, sectionTable)) return FirmwareStatus::OverlappingFileRanges;
    const uint64_t programOffset = le(header + 32, 8);
    const uint64_t programEntryBytes = le(header + 54, 2);
    Range programTable {programOffset, uint64_t(programCount) * ProgramHeaderBytes};
    if (programCount) {
        if (programEntryBytes != ProgramHeaderBytes) return FirmwareStatus::InvalidElf;
        if (!inFile(programTable, container.size)) return FirmwareStatus::Truncated;
        if (overlaps(programTable, headerRange) || overlaps(programTable, sectionTable))
            return FirmwareStatus::OverlappingFileRanges;
    } else if (programOffset || (programEntryBytes && programEntryBytes != ProgramHeaderBytes)) {
        return FirmwareStatus::InvalidElf;
    }
    const uint8_t *table = container.data + static_cast<size_t>(sectionOffset);
    // Ordinary-index profile: the first section header has no extension fields.
    for (size_t i = 0; i < SectionHeaderBytes; ++i)
        if (table[i]) return FirmwareStatus::InvalidSection;
    const Section stringSection = section(table, stringsIndex);
    const Range stringRange {stringSection.offset, stringSection.size};
    if (stringSection.type != SectionStrings || !stringSection.size ||
        (stringSection.flags & SectionCompressed)) return FirmwareStatus::InvalidStringTable;
    if (!inFile(stringRange, container.size)) return FirmwareStatus::Truncated;
    if (stringSection.size > limits.maxStringTableBytes) return FirmwareStatus::ResourceLimit;
    const uint8_t *strings = container.data + static_cast<size_t>(stringSection.offset);
    const size_t stringBytes = static_cast<size_t>(stringSection.size);
    if (strings[0] || strings[stringBytes - 1]) return FirmwareStatus::InvalidStringTable;

    constexpr char versionName[] = ".fwversion", imageName[] = ".fwimage";
    constexpr char noteName[] = ".note.gnu.build-id";
    const FirmwareText targets[] = {{versionName, sizeof(versionName) - 1},
        {imageName, sizeof(imageName) - 1}, selectedName, {noteName, sizeof(noteName) - 1}};
    Section selected[4] {};
    bool found[4] {};
    for (uint32_t i = 1; i < sectionCount; ++i) {
        const Section current = section(table, i);
        const FirmwareText currentName = name(strings, stringBytes, current.name);
        if (!currentName.data) return FirmwareStatus::InvalidStringTable;
        if (current.type != SectionNull && current.alignment &&
            (current.alignment & (current.alignment - 1))) return FirmwareStatus::InvalidSection;
        const Range currentRange {current.offset, current.size};
        if (fileContents(current)) {
            if (!inFile(currentRange, container.size)) return FirmwareStatus::Truncated;
            if (overlaps(currentRange, headerRange) || overlaps(currentRange, sectionTable) ||
                overlaps(currentRange, programTable)) return FirmwareStatus::OverlappingFileRanges;
        }
        for (uint32_t j = 1; j < i; ++j) {
            const Section previous = section(table, j);
            if (currentName.size && equal(currentName, name(strings, stringBytes, previous.name)))
                return FirmwareStatus::DuplicateSectionName;
            if (fileContents(current) && fileContents(previous) &&
                overlaps(currentRange, {previous.offset, previous.size}))
                return FirmwareStatus::OverlappingFileRanges;
        }
        for (unsigned target = 0; target < 4; ++target) {
            if (!equal(currentName, targets[target])) continue;
            if (!fileContents(current) || !current.size) return FirmwareStatus::EmptyRequiredSection;
            if (current.flags & SectionCompressed) return FirmwareStatus::UnsupportedSectionEncoding;
            selected[target] = current;
            found[target] = true;
        }
    }
    for (bool present : found) if (!present) return FirmwareStatus::MissingSection;
    const FirmwareBytes version = view(container, selected[0]);
    if (version.size != selection.expectedRelease.size + 1 || version.data[version.size - 1])
        return FirmwareStatus::VersionMismatch;
    for (size_t i = 0; i < selection.expectedRelease.size; ++i)
        if (version.data[i] != static_cast<uint8_t>(selection.expectedRelease.data[i]))
            return FirmwareStatus::VersionMismatch;
    const FirmwareImageView candidate {version, view(container, selected[1]),
                                      view(container, selected[2]), view(container, selected[3])};
    out = candidate;
    return FirmwareStatus::Ok;
}
} } // namespace Mellow::PortedNvidiaGsp
