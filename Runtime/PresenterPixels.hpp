// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MellowRT {
// Completed RenderTexture snapshots are top-left RGBA8, with straight alpha.
// Core Animation consumes premultiplied BGRA8. Rows keep their original order;
// the layer's sampling shader accounts for OpenGL's bottom-left coordinates.
inline bool copyPresenterPixels(const uint8_t *rgba, size_t sourceBytes,
                                size_t width, size_t height, uint8_t *bgra,
                                size_t destinationBytes, size_t bytesPerRow) {
    if (!rgba || !bgra || !width || !height || width > 2048 || height > 2048)
        return false;
    const size_t rowBytes = width * 4;
    if (bytesPerRow < rowBytes || bytesPerRow > std::numeric_limits<size_t>::max() / height ||
        sourceBytes != rowBytes * height || destinationBytes < bytesPerRow * height)
        return false;
    for (size_t y = 0; y < height; ++y) {
        const auto *source = rgba + y * rowBytes;
        auto *destination = bgra + y * bytesPerRow;
        for (size_t x = 0; x < width; ++x) {
            const unsigned alpha = source[4 * x + 3];
            destination[4 * x] = static_cast<uint8_t>((source[4 * x + 2] * alpha + 127) / 255);
            destination[4 * x + 1] = static_cast<uint8_t>((source[4 * x + 1] * alpha + 127) / 255);
            destination[4 * x + 2] = static_cast<uint8_t>((source[4 * x] * alpha + 127) / 255);
            destination[4 * x + 3] = static_cast<uint8_t>(alpha);
        }
        for (size_t x = rowBytes; x < bytesPerRow; ++x) destination[x] = 0;
    }
    return true;
}
}
