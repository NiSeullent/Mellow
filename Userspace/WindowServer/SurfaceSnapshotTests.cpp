// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "SurfaceSnapshot.hpp"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using namespace Mellow::Presentation;
namespace {
unsigned checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
}
int main() {
    SnapshotLayout layout;
    check(validateSnapshotLayout(2, 2, 4, 12, 24, layout) == SnapshotStatus::Ok,
          "padded two-row layout admitted");
    const uint8_t source[24] = {1,2,3,4,5,6,7,8,99,99,99,99,
                               9,10,11,12,13,14,15,16,88,88,88,88};
    uint8_t image[16] {};
    const uint8_t topFirst[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const uint8_t bottomFirst[16] = {9,10,11,12,13,14,15,16,1,2,3,4,5,6,7,8};
    check(copySnapshotRows(source, sizeof(source), layout, false, image, sizeof(image))
          == SnapshotStatus::Ok && memcmp(image, topFirst, sizeof(image)) == 0,
          "top-first pixels copied without row padding");
    check(copySnapshotRows(source, sizeof(source), layout, true, image, sizeof(image))
          == SnapshotStatus::Ok && memcmp(image, bottomFirst, sizeof(image)) == 0,
          "OpenGL bottom-first rows reversed without pixel channel changes");
    SnapshotLayout sentinel {21,22,23,24,25,26};
    check(validateSnapshotLayout(0, 2, 4, 12, 24, sentinel) == SnapshotStatus::InvalidLayout
          && sentinel.width == 21 && sentinel.imageBytes == 26, "failure preserves layout");
    check(validateSnapshotLayout(2, 2, 3, 12, 24, layout) == SnapshotStatus::InvalidLayout,
          "non-four-byte element rejected");
    check(validateSnapshotLayout(2, 2, 4, 7, 24, layout) == SnapshotStatus::InvalidLayout,
          "row too short rejected");
    check(validateSnapshotLayout(2, 2, 4, 12, 23, layout) == SnapshotStatus::InvalidLayout,
          "allocation too short rejected");
    check(validateSnapshotLayout(2, 2, 4, SIZE_MAX, SIZE_MAX, layout) == SnapshotStatus::InvalidLayout,
          "stride multiplication overflow rejected");
    check(validateSnapshotLayout(16384, 16384, 4, 65536, SIZE_MAX, layout)
          == SnapshotStatus::InvalidLayout, "snapshot allocation policy enforced");
    check(validateSnapshotLayout(16385, 1, 4, 65540, 65540, layout)
          == SnapshotStatus::InvalidLayout, "dimension policy enforced");
    memset(image, 0xA5, sizeof(image));
    check(copySnapshotRows(source, sizeof(source), layout, true, image, 15)
          == SnapshotStatus::InvalidBuffer && image[0] == 0xA5 && image[15] == 0xA5,
          "short destination rejected without mutation");
    check(copySnapshotRows(source, 23, layout, true, image, sizeof(image))
          == SnapshotStatus::InvalidLayout, "short source rejected");
    uint8_t overlap[24] {};
    check(copySnapshotRows(overlap, sizeof(overlap), layout, true, overlap + 4, 20)
          == SnapshotStatus::InvalidBuffer, "overlapping buffers rejected");
    check(copySnapshotRows(nullptr, sizeof(source), layout, true, image, sizeof(image))
          == SnapshotStatus::InvalidBuffer, "null source rejected");
    SnapshotLayout tampered = layout;
    tampered.imageBytes = 1;
    check(copySnapshotRows(source, sizeof(source), tampered, true, image, sizeof(image))
          == SnapshotStatus::InvalidLayout, "inconsistent layout rejected");
    printf("Surface snapshot: %u checks passed; native presentation NOT_RUN.\n", checks);
}
