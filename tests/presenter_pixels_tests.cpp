// SPDX-License-Identifier: MIT
#include "../Runtime/PresenterPixels.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    // Distinct corners, non-opaque alpha and row padding catch RGBA/BGRA swaps,
    // double vertical flips, straight-alpha output and overwrites independently.
    const std::array<uint8_t, 16> rgba = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 255, 250, 120, 80, 0
    };
    std::array<uint8_t, 26> output;
    output.fill(0xCD);
    assert(MellowRT::copyPresenterPixels(rgba.data(), rgba.size(), 2, 2,
                                        output.data(), 24, 12));
    const std::array<uint8_t, 24> expected = {
        0, 0, 255, 255, 0, 128, 0, 128, 0, 0, 0, 0,
        255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0
    };
    for (size_t i = 0; i < expected.size(); ++i) assert(output[i] == expected[i]);
    assert(output[24] == 0xCD && output[25] == 0xCD);
    output.fill(0xCD);
    assert(!MellowRT::copyPresenterPixels(rgba.data(), 15, 2, 2, output.data(), 24, 12));
    assert(!MellowRT::copyPresenterPixels(rgba.data(), 16, 2, 2, output.data(), 23, 12));
    assert(!MellowRT::copyPresenterPixels(rgba.data(), 16, 2, 2, output.data(), 24, 7));
    assert(!MellowRT::copyPresenterPixels(rgba.data(), 16, 2, 2, output.data(), 24,
                                        std::numeric_limits<size_t>::max()));
    assert(!MellowRT::copyPresenterPixels(rgba.data(), 16, 2049, 2, output.data(), 24, 12));
    assert(!MellowRT::copyPresenterPixels(nullptr, 16, 2, 2, output.data(), 24, 12));
    assert(!MellowRT::copyPresenterPixels(rgba.data(), 16, 2, 2, nullptr, 24, 12));
    for (uint8_t byte : output) assert(byte == 0xCD);
    // An independent scalar oracle covers every alpha, including round-to-nearest.
    for (unsigned alpha = 0; alpha <= 255; ++alpha) {
        std::array<uint8_t, 4> input = {17, 101, 233, static_cast<uint8_t>(alpha)}, actual {};
        assert(MellowRT::copyPresenterPixels(input.data(), 4, 1, 1, actual.data(), 4, 4));
        auto nearest = [alpha](unsigned value) {
            return static_cast<unsigned>(static_cast<double>(value) * alpha / 255.0 + 0.5);
        };
        assert(actual[0] == nearest(233) && actual[1] == nearest(101) && actual[2] == nearest(17));
        assert(actual[3] == alpha);
    }
    std::puts("presenter pixels: orientation, channels, alpha, pitch and bounds passed");
}
