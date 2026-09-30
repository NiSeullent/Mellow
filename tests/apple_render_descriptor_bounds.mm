// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Userspace/AppleMetal/RenderPassLimits.hpp"
#include <cstdio>
#include <cstdlib>

// CPU descriptor metadata only. Never create a MTLDevice/queue or submit work.
// A fresh process checks one index; invalid SDK access can raise or terminate.
int main(int argc, char **argv) {
    if (argc != 2) return 64;
    char *end = nullptr;
    const unsigned long index = std::strtoul(argv[1], &end, 10);
    if (!end || *end || index > 32) return 64;
    @autoreleasepool {
        @try {
            auto *array = [MTLRenderPassDescriptor renderPassDescriptor].sampleBufferAttachments;
            auto *attachment = array[index];
            if (!array || !attachment) return 65;
            std::printf("{\"index\":%lu,\"descriptor_available\":true,\"expected_capacity\":%zu,\"gpu_submission\":false}\n",
                        index, MellowAppleRender::CounterAttachmentCount);
            return 0;
        } @catch (NSException *exception) {
            if (![exception.name isEqualToString:NSRangeException]) {
                std::fprintf(stderr, "Unexpected public descriptor exception: %s\n", exception.name.UTF8String);
                return 66;
            }
            std::printf("{\"index\":%lu,\"descriptor_available\":false,\"expected_capacity\":%zu,\"exception\":\"NSRangeException\",\"gpu_submission\":false}\n",
                        index, MellowAppleRender::CounterAttachmentCount);
            return 0;
        }
    }
}
