// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "Drivers/PortedNvidiaGsp/FirmwareImage.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <vector>

using namespace Mellow::PortedNvidiaGsp;
namespace {
unsigned checks = 0;
void check(bool value, const char *description) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", description); exit(1); }
}
void put(std::vector<uint8_t> &bytes, size_t offset, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
FirmwareText text(const char *value) { return {value, strlen(value)}; }
FirmwareImageView sentinel() {
    static const uint8_t bytes[4] {};
    return {{bytes, 11}, {bytes + 1, 22}, {bytes + 2, 33}, {bytes + 3, 44}};
}
bool same(const FirmwareImageView &a, const FirmwareImageView &b) {
    return a.version.data == b.version.data && a.version.size == b.version.size &&
           a.image.data == b.image.data && a.image.size == b.image.size &&
           a.signature.data == b.signature.data && a.signature.size == b.signature.size &&
           a.buildIdNote.data == b.buildIdNote.data && a.buildIdNote.size == b.buildIdNote.size;
}
struct Fixture {
    // Synthetic ELF metadata and opaque sample bytes; no firmware was acquired.
    std::vector<uint8_t> bytes = std::vector<uint8_t>(1088, 0);
    uint32_t names[9] {};
    size_t stringBytes {};
    FirmwareSelection selection {text("610.57.04"), text(".fwsignature_tu10x")};
    FirmwareParseLimits limits {32, 512};
    FirmwareImageView output = sentinel();
    Fixture() {
        bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
        bytes[4] = 2; bytes[5] = 1; bytes[6] = 1;
        put(bytes, 16, 1, 2); put(bytes, 20, 1, 4);
        put(bytes, 40, 512, 8); put(bytes, 52, 64, 2);
        put(bytes, 58, 64, 2); put(bytes, 60, 9, 2); put(bytes, 62, 1, 2);
        const char strings[] = "\0.shstrtab\0.fwversion\0.fwimage\0.fwsignature_tu10x\0"
                               ".note.gnu.build-id\0.fwsignature_cc_tu10x\0.extra\0.bss\0";
        memcpy(bytes.data() + 64, strings, sizeof(strings));
        stringBytes = sizeof(strings);
        size_t position = 1;
        for (unsigned i = 1; i < 9; ++i) {
            names[i] = static_cast<uint32_t>(position);
            position += strlen(strings + position) + 1;
        }
        add(1, 3, 64, sizeof(strings));
        add(2, 1, 192, 10); memcpy(bytes.data() + 192, "610.57.04", 10);
        add(3, 1, 208, 7);
        const uint8_t image[7] = {0x7F, 'E', 'L', 'F', 0xA1, 0xB2, 0xC3};
        memcpy(bytes.data() + 208, image, sizeof(image));
        add(4, 1, 224, 8);
        const uint8_t signature[8] = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
        memcpy(bytes.data() + 224, signature, sizeof(signature));
        add(5, 7, 240, 20);
        // One synthetic GNU note: namesz=4, descsz=4, type=3, GNU\0, raw desc.
        put(bytes, 240, 4, 4); put(bytes, 244, 4, 4); put(bytes, 248, 3, 4);
        memcpy(bytes.data() + 252, "GNU", 4); put(bytes, 256, 0x44332211, 4);
        add(6, 1, 264, 4); put(bytes, 264, 0xAABBCCDD, 4);
        add(7, 1, 272, 3); bytes[272] = 3; bytes[273] = 2; bytes[274] = 1;
        // SHT_NOBITS offset/size are conceptual and intentionally not file ranges.
        add(8, 8, UINT64_MAX, UINT64_MAX);
    }
    void add(unsigned index, uint32_t type, uint64_t offset, uint64_t size) {
        field(index, 0, names[index], 4); field(index, 4, type, 4);
        field(index, 24, offset, 8); field(index, 32, size, 8); field(index, 48, 1, 8);
    }
    void field(unsigned index, unsigned offset, uint64_t value, unsigned count) {
        put(bytes, 512 + index * 64 + offset, value, count);
    }
    FirmwareStatus parse(size_t count = 1088) {
        return extractFirmwareImage({bytes.data(), count}, selection, limits, output);
    }
    void rejected(FirmwareStatus actual, FirmwareStatus expected) {
        if (actual != expected)
            fprintf(stderr, "status: actual=%u expected=%u (check %u)\n",
                    unsigned(actual), unsigned(expected), checks + 1);
        check(actual == expected, "failure status is precise");
        check(same(output, sentinel()), "failure leaves all output views unchanged");
    }
};
void goldenViews() {
    Fixture f;
    const std::vector<uint8_t> before = f.bytes;
    check(f.parse() == FirmwareStatus::Ok, "ordinary ELF64 GSP metadata accepted");
    check(f.bytes == before, "container remains byte-for-byte immutable");
    check(f.output.version.data == f.bytes.data() + 192 && f.output.version.size == 10 &&
          memcmp(f.output.version.data, "610.57.04", 10) == 0, "release includes terminal NUL");
    const uint8_t image[] = {0x7F, 'E', 'L', 'F', 0xA1, 0xB2, 0xC3};
    check(f.output.image.data == f.bytes.data() + 208 && f.output.image.size == sizeof(image) &&
          memcmp(f.output.image.data, image, sizeof(image)) == 0, "exact borrowed image bytes");
    const uint8_t signature[] = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    check(f.output.signature.data == f.bytes.data() + 224 && f.output.signature.size == 8 &&
          memcmp(f.output.signature.data, signature, 8) == 0, "selected opaque signature bytes");
    check(f.output.buildIdNote.data == f.bytes.data() + 240 && f.output.buildIdNote.size == 20 &&
          f.output.buildIdNote.data[0] == 4 && f.output.buildIdNote.data[16] == 0x11,
          "raw build-id note includes header and GNU name; not descriptor-only");
    Fixture cc;
    cc.selection.signatureSectionName = text(".fwsignature_cc_tu10x");
    check(cc.parse() == FirmwareStatus::Ok && cc.output.signature.data == cc.bytes.data() + 264 &&
          cc.output.signature.size == 4 && cc.output.signature.data[0] == 0xDD,
          "caller full CC section name chooses different raw bytes");
    Fixture unnamed;
    unnamed.field(7, 0, 0, 4); unnamed.field(8, 0, 0, 4);
    check(unnamed.parse() == FirmwareStatus::Ok, "multiple empty section names are permitted");
    Fixture empty;
    empty.field(7, 24, 1088, 8); empty.field(7, 32, 0, 8);
    check(empty.parse() == FirmwareStatus::Ok, "unneeded empty section at exact file end permitted");
    Fixture unaligned;
    std::vector<uint8_t> shifted(unaligned.bytes.size() + 1);
    memcpy(shifted.data() + 1, unaligned.bytes.data(), unaligned.bytes.size());
    check(extractFirmwareImage({shifted.data() + 1, unaligned.bytes.size()}, unaligned.selection,
          unaligned.limits, unaligned.output) == FirmwareStatus::Ok,
          "byte decoding permits an unaligned CPU container without struct casts");
}
void headerFailures() {
    for (size_t length = 0; length < 1088; ++length) {
        Fixture f;
        check(f.parse(length) != FirmwareStatus::Ok, "every truncated synthetic container rejected");
        check(same(f.output, sentinel()), "all truncations preserve result views");
    }
    { Fixture f; f.bytes[0] = 0; f.rejected(f.parse(), FirmwareStatus::InvalidElf); }
    for (unsigned position : {4U, 5U}) {
        Fixture f; f.bytes[position] = position == 4 ? 1 : 2;
        f.rejected(f.parse(), FirmwareStatus::UnsupportedElf);
    }
    for (unsigned position : {6U, 20U, 52U, 58U}) {
        Fixture f; f.bytes[position] = 0; f.rejected(f.parse(), FirmwareStatus::InvalidElf);
    }
    for (uint32_t count : {0U, 0xFF00U, 0xFFFFU}) {
        Fixture f; put(f.bytes, 60, count, 2); f.rejected(f.parse(), FirmwareStatus::UnsupportedElf);
    }
    for (uint32_t index : {0U, 9U, 10U}) {
        Fixture f; put(f.bytes, 62, index, 2); f.rejected(f.parse(), FirmwareStatus::InvalidElf);
    }
    { Fixture f; put(f.bytes, 62, 0xFFFF, 2); f.rejected(f.parse(), FirmwareStatus::UnsupportedElf); }
    { Fixture f; put(f.bytes, 56, 0xFFFF, 2); f.rejected(f.parse(), FirmwareStatus::UnsupportedElf); }
    for (uint64_t offset : {uint64_t(1088), uint64_t(UINT64_MAX), uint64_t(UINT64_MAX - 511)}) {
        Fixture f; put(f.bytes, 40, offset, 8); f.rejected(f.parse(), FirmwareStatus::Truncated);
    }
    { Fixture f; put(f.bytes, 40, 32, 8); f.rejected(f.parse(), FirmwareStatus::OverlappingFileRanges); }
    { Fixture f; f.bytes[512] = 1; f.rejected(f.parse(), FirmwareStatus::InvalidSection); }
    { Fixture f; f.limits.maxSections = 8; f.rejected(f.parse(), FirmwareStatus::ResourceLimit); }
    { Fixture f; f.limits.maxStringTableBytes = 1; f.rejected(f.parse(), FirmwareStatus::ResourceLimit); }
}
void sectionFailures() {
    { Fixture f; f.field(1, 4, 1, 4); f.rejected(f.parse(), FirmwareStatus::InvalidStringTable); }
    { Fixture f; f.field(1, 32, 0, 8); f.rejected(f.parse(), FirmwareStatus::InvalidStringTable); }
    { Fixture f; f.field(1, 8, 0x800, 8); f.rejected(f.parse(), FirmwareStatus::InvalidStringTable); }
    { Fixture f; f.bytes[64] = 'X'; f.rejected(f.parse(), FirmwareStatus::InvalidStringTable); }
    { Fixture f; f.field(1, 32, 3, 8); f.rejected(f.parse(), FirmwareStatus::InvalidStringTable); }
    { Fixture f; f.field(2, 0, 0xFFFFFFFF, 4); f.rejected(f.parse(), FirmwareStatus::InvalidStringTable); }
    { Fixture f; f.field(1, 24, UINT64_MAX, 8); f.rejected(f.parse(), FirmwareStatus::Truncated); }
    for (unsigned which : {24U, 32U}) {
        Fixture f; f.field(3, which, UINT64_MAX, 8); f.rejected(f.parse(), FirmwareStatus::Truncated);
    }
    { Fixture f; f.field(3, 24, 1082, 8); f.rejected(f.parse(), FirmwareStatus::Truncated); }
    for (unsigned target : {2U, 3U, 4U, 5U}) {
        Fixture f; f.field(target, 32, 0, 8); f.rejected(f.parse(), FirmwareStatus::EmptyRequiredSection);
        Fixture nobits; nobits.field(target, 4, 8, 4);
        nobits.rejected(nobits.parse(), FirmwareStatus::EmptyRequiredSection);
        Fixture compressed; compressed.field(target, 8, 0x800, 8);
        compressed.rejected(compressed.parse(), FirmwareStatus::UnsupportedSectionEncoding);
        Fixture missing; missing.field(target, 0, 0, 4);
        missing.rejected(missing.parse(), FirmwareStatus::MissingSection);
    }
    { Fixture f; f.field(7, 0, f.names[3], 4); f.rejected(f.parse(), FirmwareStatus::DuplicateSectionName); }
    { Fixture f; f.field(6, 0, f.names[7], 4); f.rejected(f.parse(), FirmwareStatus::DuplicateSectionName); }
    { Fixture f; memcpy(f.bytes.data() + 64 + f.stringBytes, ".fwimage", 9);
      f.field(1, 32, f.stringBytes + 9, 8); f.field(7, 0, f.stringBytes, 4);
      f.rejected(f.parse(), FirmwareStatus::DuplicateSectionName); }
    { Fixture f; f.field(7, 0, f.names[7] + 1, 4);
      check(f.parse() == FirmwareStatus::Ok, "ELF substring name reference is permitted"); }
    for (uint64_t offset : {uint64_t(0), uint64_t(63), uint64_t(64), uint64_t(224), uint64_t(512)}) {
        Fixture f; f.field(3, 24, offset, 8); f.rejected(f.parse(), FirmwareStatus::OverlappingFileRanges);
    }
    { Fixture f; f.field(3, 48, 3, 8); f.rejected(f.parse(), FirmwareStatus::InvalidSection); }
    { Fixture f; f.field(7, 4, 0, 4); f.field(7, 24, UINT64_MAX, 8);
      f.field(7, 32, UINT64_MAX, 8); check(f.parse() == FirmwareStatus::Ok, "SHT_NULL has no contents span"); }
}
void versionFailures() {
    { Fixture f; f.selection.expectedRelease = text("610.57.05"); f.rejected(f.parse(), FirmwareStatus::VersionMismatch); }
    { Fixture f; f.bytes[201] = 'X'; f.rejected(f.parse(), FirmwareStatus::VersionMismatch); }
    { Fixture f; f.bytes[195] = 0; f.rejected(f.parse(), FirmwareStatus::VersionMismatch); }
    { Fixture f; f.field(2, 32, 9, 8); f.rejected(f.parse(), FirmwareStatus::VersionMismatch); }
    { Fixture f; f.field(2, 32, 11, 8); f.rejected(f.parse(), FirmwareStatus::VersionMismatch); }
    { Fixture f; f.selection.signatureSectionName = text(".fwsignature_ga100");
      f.rejected(f.parse(), FirmwareStatus::MissingSection); }
    for (const char *value : {"", ".fwimage", ".fwsignature_", ".fwsignature_cc_",
         ".fwsignature_012345678901234567890"}) {
        Fixture f; f.selection.signatureSectionName = text(value);
        f.rejected(f.parse(), FirmwareStatus::InvalidArgument);
    }
    for (bool release : {false, true}) {
        Fixture f; const char embedded[] = {'x', 0, 'y'};
        if (release) f.selection.expectedRelease = {embedded, 3};
        else f.selection.signatureSectionName = {embedded, 3};
        f.rejected(f.parse(), FirmwareStatus::InvalidArgument);
    }
    { Fixture f; f.selection.expectedRelease = {nullptr, 0}; f.rejected(f.parse(), FirmwareStatus::InvalidArgument); }
    { Fixture f; f.selection.expectedRelease = text(""); f.rejected(f.parse(), FirmwareStatus::InvalidArgument); }
    { Fixture f; f.selection.expectedRelease = {"x", 63}; f.rejected(f.parse(), FirmwareStatus::InvalidArgument); }
    { Fixture f; char release[63]; memset(release, 'R', 62); release[62] = 0;
      memcpy(f.bytes.data() + 320, release, 63); f.field(2, 24, 320, 8); f.field(2, 32, 63, 8);
      f.selection.expectedRelease = {release, 62};
      check(f.parse() == FirmwareStatus::Ok && f.output.version.size == 63,
            "longest diagnostic-profile release checks exact NUL without guessing release numbers"); }
}
void programTableAndCpuGuards() {
    Fixture valid;
    put(valid.bytes, 32, 320, 8); put(valid.bytes, 54, 56, 2); put(valid.bytes, 56, 1, 2);
    check(valid.parse() == FirmwareStatus::Ok, "ordinary program header table bounded without loading segments");
    { Fixture f; put(f.bytes, 32, 320, 8); f.rejected(f.parse(), FirmwareStatus::InvalidElf); }
    { Fixture f; put(f.bytes, 54, 55, 2); f.rejected(f.parse(), FirmwareStatus::InvalidElf); }
    { Fixture f; put(f.bytes, 56, 1, 2); f.rejected(f.parse(), FirmwareStatus::InvalidElf); }
    for (uint64_t offset : {uint64_t(1080), uint64_t(UINT64_MAX)}) {
        Fixture f; put(f.bytes, 32, offset, 8); put(f.bytes, 54, 56, 2); put(f.bytes, 56, 1, 2);
        f.rejected(f.parse(), FirmwareStatus::Truncated);
    }
    for (uint64_t offset : {uint64_t(0), uint64_t(512), uint64_t(208)}) {
        Fixture f; put(f.bytes, 32, offset, 8); put(f.bytes, 54, 56, 2); put(f.bytes, 56, 1, 2);
        f.rejected(f.parse(), FirmwareStatus::OverlappingFileRanges);
    }
    for (bool strings : {false, true}) {
        Fixture f; if (strings) f.limits.maxStringTableBytes = 0; else f.limits.maxSections = 0;
        f.rejected(f.parse(), FirmwareStatus::InvalidArgument);
    }
    { Fixture f; f.rejected(extractFirmwareImage({nullptr, 1088}, f.selection, f.limits, f.output), FirmwareStatus::InvalidArgument); }
    for (unsigned input = 0; input < 3; ++input) {
        Fixture f;
        FirmwareBytes bytes {f.bytes.data(), f.bytes.size()};
        if (input == 0) bytes = {reinterpret_cast<const uint8_t *>(UINTPTR_MAX - 31), 1088};
        if (input == 1) f.selection.expectedRelease = {reinterpret_cast<const char *>(UINTPTR_MAX - 3), 8};
        if (input == 2) f.selection.signatureSectionName = {reinterpret_cast<const char *>(UINTPTR_MAX - 3), 18};
        f.rejected(extractFirmwareImage(bytes, f.selection, f.limits, f.output), FirmwareStatus::IntegerOverflow);
    }
    { Fixture f; auto *alias = new (f.bytes.data() + 320) FirmwareImageView(sentinel());
      check(extractFirmwareImage({f.bytes.data(), f.bytes.size()}, f.selection, f.limits, *alias) ==
            FirmwareStatus::OutputAliasesInput && same(*alias, sentinel()), "output-container alias rejected before mutation"); }
    for (bool release : {false, true}) {
        Fixture f;
        const FirmwareText alias {reinterpret_cast<const char *>(&f.output), 8};
        if (release) f.selection.expectedRelease = alias; else f.selection.signatureSectionName = alias;
        f.rejected(f.parse(), FirmwareStatus::OutputAliasesInput);
    }
}
} // namespace
int main() {
    goldenViews(); headerFailures(); sectionFailures(); versionFailures(); programTableAndCpuGuards();
    printf("ported NVIDIA GSP firmware ELF: %u checks passed (synthetic containers only)\n", checks);
    return 0;
}
