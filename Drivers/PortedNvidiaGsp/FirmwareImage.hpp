// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2018-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2017-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Mellow { namespace PortedNvidiaGsp {

struct FirmwareBytes { const uint8_t *data {}; size_t size {}; };
// An explicit byte count excluding any terminal NUL. Embedded NULs are invalid.
struct FirmwareText { const char *data {}; size_t size {}; };
struct FirmwareSelection {
    FirmwareText expectedRelease;
    // Full section name selected from actual chip/HAL/CC state by the caller.
    // No PCI name, device count, architecture guess or first-match selection.
    FirmwareText signatureSectionName;
};
struct FirmwareParseLimits {
    uint32_t maxSections {};
    size_t maxStringTableBytes {};
};
enum class FirmwareStatus : uint8_t {
    Ok, InvalidArgument, IntegerOverflow, OutputAliasesInput,
    Truncated, InvalidElf, UnsupportedElf, ResourceLimit,
    InvalidStringTable, InvalidSection, DuplicateSectionName,
    OverlappingFileRanges, MissingSection, EmptyRequiredSection,
    UnsupportedSectionEncoding, VersionMismatch
};
struct FirmwareImageView {
    FirmwareBytes version;   // Exact release bytes including terminal NUL.
    FirmwareBytes image;     // Raw .fwimage bytes, suitable as Radix3 payload.
    FirmwareBytes signature; // Opaque selected signature bytes, not authenticated.
    // Raw .note.gnu.build-id section, including its note header/name/padding.
    // This is NOT a decoded GNU build-id descriptor or a verified image digest.
    FirmwareBytes buildIdNote;
};

// Validate an ELF64 little-endian GSP container and return borrowed byte views.
// No allocation, source/firmware loading, copying, DMA, authentication or boot.
// Caller keeps the container immutable and alive for all returned-view use and
// establishes its origin/trust, matching driver release and actual chip selection.
// Release text is 1..62 bytes (upstream's <64-byte diagnostic string profile);
// selected signature name is 1..31 bytes (upstream's 32-byte name buffer).
// Output must be disjoint from the container and both selection text spans.
// It is unchanged on every failure. Limits are caller resource budgets, never
// hardware capability limits. The no-heap all-name/all-range audit costs at most
// O(maxSections^2 * maxStringTableBytes). Empty section names may repeat.
// Canonical 64-byte headers and ordinary section indexes only; extended indexes
// and compressed required sections are rejected explicitly. SHT_NOBITS has no
// file contents; its conceptual offset/size never become an input byte span.
FirmwareStatus extractFirmwareImage(FirmwareBytes container,
                                    FirmwareSelection selection,
                                    FirmwareParseLimits limits,
                                    FirmwareImageView &out);

} } // namespace Mellow::PortedNvidiaGsp
