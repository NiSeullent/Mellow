// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stddef.h>
#include <stdint.h>
#include <vector>
namespace Mellow { namespace Presentation {
struct FixtureValidation { bool passed {}; size_t pixels {}, foreground {}; };
// Independent CPU verifier for the fixed tests/render_fixture.hpp shader.
// Called after GPU readback; expected pixels are never sent to the provider.
inline FixtureValidation verifyRenderFixture(const std::vector<uint8_t> &rgba,
                                             const std::array<float, 4> &parameters) {
    FixtureValidation result;
    if (rgba.size() != 64 * 48 * 4) return result;
    for (float parameter : parameters) if (!std::isfinite(parameter)) return result;
    if (std::abs(parameters[0]) > .1f || std::abs(parameters[1]) > .1f ||
        parameters[2] < 0 || parameters[2] > 1 || parameters[3] != 1.f / 64.f)
        return result;
    struct Point { double x, y; };
    const std::array<Point, 3> ndc {{{-.72f, -.52f}, {.58f, -.32f}, {-.18f, .72f}}};
    std::array<Point, 3> points;
    for (size_t i = 0; i < 3; ++i) {
        const float x = static_cast<float>(ndc[i].x) + parameters[0];
        const float y = static_cast<float>(ndc[i].y) + parameters[1];
        points[i] = {(static_cast<double>(x) + 1.) * 32., (1. - static_cast<double>(y)) * 24.};
    }
    auto edge = [](Point a, Point b, Point p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    };
    const double sign = edge(points[0], points[1], points[2]) > 0 ? 1. : -1.;
    double tolerance = 0;
    for (size_t i = 0; i < 3; ++i) {
        const Point a = points[i], b = points[(i + 1) % 3];
        tolerance = std::max(tolerance, std::hypot(b.x - a.x, b.y - a.y) / 256.);
    }
    for (size_t y = 0; y < 48; ++y) {
        for (size_t x = 0; x < 64; ++x) {
            const Point center {static_cast<double>(x) + .5, static_cast<double>(y) + .5};
            double minimum = edge(points[0], points[1], center) * sign;
            for (size_t i = 1; i < 3; ++i)
                minimum = std::min(minimum, edge(points[i], points[(i + 1) % 3], center) * sign);
            const size_t at = (y * 64 + x) * 4;
            const bool clear = rgba[at] == 0 && rgba[at + 1] == 0 && rgba[at + 2] == 0 && rgba[at + 3] == 0;
            const long expected[3] = {std::lround(center.x * parameters[3] * 255.),
                std::lround(center.y * parameters[3] * 255.), std::lround(parameters[2] * 255.)};
            bool foreground = rgba[at + 3] == 255;
            for (size_t channel = 0; channel < 3; ++channel)
                foreground = foreground && std::abs(static_cast<long>(rgba[at + channel]) - expected[channel]) <= 1;
            const bool valid = minimum < -tolerance ? clear : minimum > tolerance ? foreground : clear || foreground;
            if (!valid) return result;
            ++result.pixels;
            if (foreground) ++result.foreground;
        }
    }
    result.passed = result.foreground >= 500;
    return result;
}
} }
