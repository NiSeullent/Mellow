// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#pragma once
#include <stddef.h>

namespace Mellow { namespace Presentation {
enum class SnapshotStatus { Ok, InvalidLayout, InvalidBuffer };
struct SnapshotLayout {
    size_t width {}, height {}, sourceRowBytes {}, sourceBytes {},
           imageRowBytes {}, imageBytes {};
};
constexpr size_t SnapshotBytesMax = 256U * 1024U * 1024U;
// Adapter resource limits, not IOSurface hardware capabilities. Output is
// unchanged on error. The source must be nonplanar four-byte BGRA pixels.
SnapshotStatus validateSnapshotLayout(size_t width, size_t height,
    size_t bytesPerElement, size_t sourceRowBytes, size_t allocationBytes,
    SnapshotLayout &out);
// Copies into distinct, caller-owned storage. Source row 0 can be the bottom
// scanline (OpenGL); image row 0 is always the top scanline. Padding is omitted.
// Valid pointer lifetimes and exclusive producer/consumer access are required.
SnapshotStatus copySnapshotRows(const void *source, size_t sourceBytes,
    const SnapshotLayout &, bool sourceBottomLeft, void *image, size_t imageBytes);
} }
