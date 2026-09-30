// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// CPU corruption controls only; the generated pixels are never GPU evidence.
#include "RenderFixtureOracle.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <limits>
using namespace Mellow::Presentation;
namespace {
unsigned checks {};
void check(bool value, const char *description) {
    ++checks; if (!value) { fprintf(stderr, "FAIL: %s\n", description); exit(1); }
}
std::vector<uint8_t> ideal(const std::array<float, 4> &p) {
    // Barycentric construction is independent of the verifier's edge lines.
    const double ax = (-.72f + p[0] + 1.) * 32., ay = (1. - (-.52f + p[1])) * 24.;
    const double bx = (.58f + p[0] + 1.) * 32., by = (1. - (-.32f + p[1])) * 24.;
    const double cx = (-.18f + p[0] + 1.) * 32., cy = (1. - (.72f + p[1])) * 24.;
    const double denominator = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    std::vector<uint8_t> pixels(64 * 48 * 4, 0);
    for (size_t y = 0; y < 48; ++y) for (size_t x = 0; x < 64; ++x) {
        const double px = static_cast<double>(x) + .5, py = static_cast<double>(y) + .5;
        const double u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denominator;
        const double v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denominator;
        if (u >= 0 && v >= 0 && u + v <= 1) {
            const size_t at = (y * 64 + x) * 4;
            pixels[at] = static_cast<uint8_t>(std::lround(px * p[3] * 255.));
            pixels[at + 1] = static_cast<uint8_t>(std::lround(py * p[3] * 255.));
            pixels[at + 2] = static_cast<uint8_t>(std::lround(p[2] * 255.));
            pixels[at + 3] = 255;
        }
    }
    return pixels;
}
}
int main() {
    const std::array<float, 4> params {0.f, 0.f, 23.f / 255.f, 1.f / 64.f};
    const auto pixels = ideal(params);
    const auto validated = verifyRenderFixture(pixels, params);
    check(validated.passed && validated.pixels == 3072 && validated.foreground >= 500,
          "independent barycentric fixture admitted");
    check(!verifyRenderFixture(std::vector<uint8_t>(pixels.size(), 0), params).passed,
          "black matching readbacks cannot pass");
    auto flipped = pixels;
    for (size_t y = 0; y < 48; ++y) for (size_t x = 0; x < 256; ++x)
        flipped[y * 256 + x] = pixels[(47 - y) * 256 + x];
    check(!verifyRenderFixture(flipped, params).passed, "vertical orientation corruption rejected");
    const size_t center = (24 * 64 + 24) * 4;
    check(pixels[center + 3] == 255, "corruption control uses an interior pixel");
    auto changed = pixels;
    ++changed[center];
    check(verifyRenderFixture(changed, params).passed, "one RGB unit tolerance admitted");
    changed[center] = static_cast<uint8_t>(pixels[center] + 3);
    check(!verifyRenderFixture(changed, params).passed, "three RGB units corruption rejected");
    changed = pixels; changed[center + 3] = 254;
    check(!verifyRenderFixture(changed, params).passed, "alpha corruption rejected");
    changed = pixels; changed[2] = 200;
    check(!verifyRenderFixture(changed, params).passed, "nonclear exterior rejected");
    changed = pixels; changed.pop_back();
    check(!verifyRenderFixture(changed, params).passed, "short readback rejected");
    auto invalid = params; invalid[2] = std::numeric_limits<float>::quiet_NaN();
    check(!verifyRenderFixture(pixels, invalid).passed, "nonfinite parameters rejected");
    invalid = params; invalid[3] = std::numeric_limits<float>::max();
    check(!verifyRenderFixture(pixels, invalid).passed, "unbounded conversion input rejected");
    const std::array<float, 4> shifted {-.032f, .024f, .8f, 1.f / 64.f};
    check(verifyRenderFixture(ideal(shifted), shifted).passed, "translated triangle/color variation admitted");
    printf("Render fixture oracle: %u controls passed; synthetic CPU data only.\n", checks);
}
