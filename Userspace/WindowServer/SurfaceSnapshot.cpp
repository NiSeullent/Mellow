// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "SurfaceSnapshot.hpp"
#include <stdint.h>
#include <string.h>

namespace Mellow { namespace Presentation {
SnapshotStatus validateSnapshotLayout(size_t width, size_t height,
    size_t elementBytes, size_t rowBytes, size_t allocationBytes,
    SnapshotLayout &out) {
    if (!width || !height || width > 16384 || height > 16384 || elementBytes != 4)
        return SnapshotStatus::InvalidLayout;
    const size_t imageRowBytes = width * 4;
    if (rowBytes < imageRowBytes || rowBytes > SIZE_MAX / height)
        return SnapshotStatus::InvalidLayout;
    const size_t sourceBytes = rowBytes * height;
    const size_t imageBytes = imageRowBytes * height;
    if (sourceBytes > allocationBytes || imageBytes > SnapshotBytesMax)
        return SnapshotStatus::InvalidLayout;
    out = {width, height, rowBytes, sourceBytes, imageRowBytes, imageBytes};
    return SnapshotStatus::Ok;
}

SnapshotStatus copySnapshotRows(const void *source, size_t sourceBytes,
    const SnapshotLayout &layout, bool bottomLeft, void *image, size_t imageBytes) {
    SnapshotLayout checked;
    if (validateSnapshotLayout(layout.width, layout.height, 4, layout.sourceRowBytes,
            sourceBytes, checked) != SnapshotStatus::Ok ||
        checked.sourceBytes != layout.sourceBytes ||
        checked.imageRowBytes != layout.imageRowBytes || checked.imageBytes != layout.imageBytes)
        return SnapshotStatus::InvalidLayout;
    if (!source || !image || imageBytes < layout.imageBytes)
        return SnapshotStatus::InvalidBuffer;
    const uintptr_t sourceBegin = reinterpret_cast<uintptr_t>(source);
    const uintptr_t imageBegin = reinterpret_cast<uintptr_t>(image);
    if (sourceBegin > UINTPTR_MAX - layout.sourceBytes ||
        imageBegin > UINTPTR_MAX - layout.imageBytes ||
        (sourceBegin < imageBegin + layout.imageBytes &&
         imageBegin < sourceBegin + layout.sourceBytes))
        return SnapshotStatus::InvalidBuffer;
    const auto *input = static_cast<const unsigned char *>(source);
    auto *output = static_cast<unsigned char *>(image);
    for (size_t row = 0; row < layout.height; ++row) {
        const size_t sourceRow = bottomLeft ? layout.height - 1 - row : row;
        memcpy(output + row * layout.imageRowBytes,
               input + sourceRow * layout.sourceRowBytes, layout.imageRowBytes);
    }
    return SnapshotStatus::Ok;
}
} }
