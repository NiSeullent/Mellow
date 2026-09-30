// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Explicit native app presentation test. Does not register an Apple GPU driver.
#import <AppKit/AppKit.h>
#import "SurfacePresenter.h"
#include "RenderFixtureOracle.hpp"
#include "../../Runtime/RenderObjects.hpp"
#include "../../tests/render_fixture.hpp"
#include "../../tests/opencl_runtime_sha256.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace MellowMTL;
using MellowRT::RenderShaderJit::Stage;
namespace {
struct Unavailable : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
NSString *string(const std::string &value) {
    NSString *result = [NSString stringWithUTF8String:value.c_str()];
    return result ?: @"Invalid UTF-8 diagnostic";
}
unsigned number(const char *value, unsigned minimum, unsigned maximum) {
    char *end = nullptr;
    const unsigned long result = strtoul(value, &end, 10);
    require(value[0] && end && !*end && result >= minimum && result <= maximum,
            "Numeric option outside allowed range");
    return static_cast<unsigned>(result);
}
void pump(unsigned milliseconds) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:milliseconds / 1000.0];
    while ([deadline timeIntervalSinceNow] > 0) {
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:deadline
            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event) [NSApp sendEvent:event];
        [NSApp updateWindows];
    }
}
size_t compareSurfaceImage(CGImageRef image, const std::vector<uint8_t> &rgba,
                          size_t width, size_t height) {
    require(CGImageGetWidth(image) == width && CGImageGetHeight(image) == height &&
            CGImageGetBitsPerPixel(image) == 32 && CGImageGetBytesPerRow(image) == width * 4 &&
            CGImageGetAlphaInfo(image) == kCGImageAlphaFirst,
            "Snapshot image layout differs from GPU RGBA layout");
    require(rgba.size() == width * height * 4, "GPU RGBA readback size differs");
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
    require(data != nullptr, "Cannot obtain actual snapshot image bytes");
    const bool sizeMatches = static_cast<size_t>(CFDataGetLength(data)) == rgba.size();
    bool matches = sizeMatches;
    const UInt8 *bgra = CFDataGetBytePtr(data);
    if (matches) {
        for (size_t pixel = 0; pixel < rgba.size(); pixel += 4) {
            if (bgra[pixel] != rgba[pixel + 2] || bgra[pixel + 1] != rgba[pixel + 1] ||
                bgra[pixel + 2] != rgba[pixel] || bgra[pixel + 3] != rgba[pixel + 3]) {
                matches = false;
                break;
            }
        }
    }
    CFRelease(data);
    require(matches, "IOSurface BGRA channels/orientation differ from GPU RGBA readback");
    return rgba.size();
}
}

int main(int argc, char **argv) {
    @autoreleasepool {
        bool presentRequested = false, attempted = false, passed = false, unavailable = false;
        unsigned requested = 90, intervalMs = 33, completed = 0;
        size_t comparedBytes = 0, verifiedPixels = 0;
        int result = 1;
        std::string reportPath;
        NSMutableDictionary<NSString *, id> *report = [@{
            @"schema_version":@1, @"scope":@"application-surface-presentation",
            @"status":@"NOT_RUN", @"passed":@NO, @"native_macos_execution":@YES,
            @"apple_metal_abi_registered":@NO, @"windowserver_acceleration_verified":@NO,
            @"display_scanout_verified":@NO, @"physical_pci_identity_verified":@NO,
            @"cpu_render_fallback":@NO
        } mutableCopy];
        NSMutableArray<NSDictionary *> *samples = [NSMutableArray array];
        NSWindow *window = nil;
        try {
            for (int i = 1; i < argc; ++i) {
                if (!strcmp(argv[i], "--present")) presentRequested = true;
                else if (!strcmp(argv[i], "--report") && i + 1 < argc) reportPath = argv[++i];
                else if (!strcmp(argv[i], "--frames") && i + 1 < argc) requested = number(argv[++i], 2, 600);
                else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) intervalMs = number(argv[++i], 1, 1000);
                else throw std::runtime_error("Unknown or incomplete argument");
            }
            if (!presentRequested) throw Unavailable("Explicit --present request required");
            require(!reportPath.empty(), "--report PATH required; optional --frames 2..600 --interval-ms 1..1000");
            require(requested * intervalMs <= 30000, "Requested display time exceeds 30 seconds");
#if !defined(__x86_64__)
            throw Unavailable("This acceptance target requires an x86_64 Mac");
#endif
            const NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
            report[@"os_version"] = @{@"major":@(version.majorVersion), @"minor":@(version.minorVersion),
                                      @"patch":@(version.patchVersion)};
            if (version.majorVersion != 15 && version.majorVersion != 26)
                throw Unavailable("Acceptance target requires macOS 15 or 26");
            Error error;
            auto device = RenderDevice::createOpenGL(error);
            if (!device) {
                if (error.message == "No accelerated CGL 4.1 pixel format available" ||
                    error.message == "Non-accelerated or fallback CGL renderer rejected" ||
                    error.message == "CGL OpenGL 4.1 core is required" ||
                    error.message == "Software OpenGL renderer rejected")
                    throw Unavailable(error.message);
                throw std::runtime_error(error.message);
            }
            const auto info = device->hardware();
            require(info.acceleratedPixelFormat && info.softwareRendererRejected,
                    "Native accelerated CGL provider unavailable");
            report[@"device"] = @{@"vendor":string(info.vendor), @"renderer":string(info.renderer),
                @"version":string(info.version), @"glsl_version":string(info.shadingLanguageVersion),
                @"accelerated_pixel_format":@(info.acceleratedPixelFormat),
                @"software_renderer_rejected":@(info.softwareRendererRejected)};
            auto texture = device->newIOSurfaceTexture(64, 48, error);
            require(bool(texture), error.message);
            require(texture->iosurface() == nullptr, "Unrendered texture exposed completed IOSurface content");
            auto library = device->newLibraryWithSource(MellowRenderFixture, error);
            require(bool(library), error.message);
            auto vertex = library->newFunction("triangleVertex", Stage::Vertex, error);
            auto fragment = library->newFunction("gradientFragment", Stage::Fragment, error);
            require(bool(vertex) && bool(fragment), error.message);
            auto pipeline = device->newRenderPipeline(vertex, fragment, error);
            require(bool(pipeline), error.message);
            auto queue = device->newCommandQueue();
            require(bool(queue), "Cannot create render command queue");
            [NSApplication sharedApplication];
            require([NSApp setActivationPolicy:NSApplicationActivationPolicyRegular],
                    "Application activation policy rejected");
            [NSApp finishLaunching];
            window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 480)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                backing:NSBackingStoreBuffered defer:NO];
            require(window != nil, "Cannot create application window");
            window.releasedWhenClosed = NO;
            window.title = @"Mellow IOSurface presentation — application layer";
            [window center];
            window.contentView.wantsLayer = YES;
            CALayer *layer = [CALayer layer];
            layer.frame = window.contentView.bounds;
            layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
            layer.contentsGravity = kCAGravityResizeAspect;
            layer.contentsScale = window.backingScaleFactor;
            [window.contentView.layer addSublayer:layer];
            MellowSurfacePresenter *presenter = [[MellowSurfacePresenter alloc] initWithLayer:layer];
            require(presenter != nil, "Cannot create main-thread surface presenter");
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            pump(50);
            require(window.visible, "Application window is not visible");
            uint32_t firstSurfaceID = 0;
            uint64_t firstSequence = 0;
            for (unsigned frameIndex = 0; frameIndex < requested; ++frameIndex) {
                @autoreleasepool {
                    require(window.visible, "Window closed before requested frames completed");
                    auto command = queue->commandBuffer();
                    require(bool(command), "Cannot create render command buffer");
                    auto encoder = command->renderCommandEncoder({texture, {0.f,0.f,0.f,0.f}}, error);
                    require(bool(encoder), error.message);
                    require(encoder->setRenderPipeline(pipeline, error), error.message);
                    const std::array<float, 4> parameters {
                        (static_cast<int>(frameIndex % 17) - 8) * .004f,
                        (static_cast<int>(frameIndex % 13) - 6) * .004f,
                        static_cast<float>((frameIndex * 37U + 23U) % 256U) / 255.f, 1.f / 64.f};
                    require(encoder->setSharedParameters(parameters, error), error.message);
                    require(encoder->drawPrimitives(PrimitiveType::Triangle, 0, 3, error), error.message);
                    require(encoder->endEncoding(error), error.message);
                    attempted = true;
                    require(command->commit(error), error.message);
                    require(command->status() == CommandStatus::Completed && command->waitUntilCompleted(error),
                            "Render command did not reach completed state");
                    require(command->executions().size() == 1, "Unexpected number of GPU render passes");
                    const auto &frame = command->executions().front();
                    require(frame.renderSubmitted && frame.fenceSignaled && frame.readbackCompleted &&
                            frame.resourcesReleased && frame.ioSurfaceWritten, "Incomplete GPU/IOSurface completion contract");
                    IOSurfaceRef surface = texture->iosurface(); // Borrowed; texture remains alive.
                    require(surface && frame.ioSurfaceID != 0 && IOSurfaceGetID(surface) == frame.ioSurfaceID,
                            "GPU frame and IOSurface identifiers differ");
                    if (!frameIndex) { firstSurfaceID = frame.ioSurfaceID; firstSequence = frame.sequence; }
                    require(frame.ioSurfaceID == firstSurfaceID && frame.sequence == firstSequence + frameIndex &&
                            texture->contentSequence() == frame.sequence, "Surface/command sequence correlation failed");
                    const auto rgba = texture->read(error);
                    require(error.code == ErrorCode::None, error.message);
                    const auto pixelValidation = Mellow::Presentation::verifyRenderFixture(rgba, parameters);
                    require(pixelValidation.passed, "GPU pixels fail independent triangle/gradient oracle");
                    verifiedPixels += pixelValidation.pixels;
                    NSError *nativeError = nil;
                    CGImageRef image = MellowCopyCompletedSurfaceImage(surface, YES, &nativeError);
                    require(image != nullptr, nativeError ? nativeError.localizedDescription.UTF8String : "Snapshot failed");
                    try { comparedBytes += compareSurfaceImage(image, rgba, 64, 48); }
                    catch (...) { CGImageRelease(image); throw; }
                    CGImageRelease(image);
                    NSDictionary<NSString *, id> *receipt = [presenter presentCompletedSurface:surface
                        sourceBottomLeft:YES error:&nativeError];
                    require(receipt != nil, nativeError ? nativeError.localizedDescription.UTF8String : "Presentation failed");
                    require([receipt[@"snapshotCreated"] boolValue] && [receipt[@"transactionCommitCalled"] boolValue] &&
                            [receipt[@"surfaceID"] unsignedIntValue] == frame.ioSurfaceID &&
                            [receipt[@"sequence"] unsignedLongLongValue] == frameIndex + 1ULL,
                            "Layer transaction receipt does not match GPU surface");
                    if (!frameIndex || frameIndex == requested - 1) {
                        OpenCLTestSha256 digest; digest.append(rgba.data(), rgba.size());
                        [samples addObject:@{@"frame":@(frameIndex), @"epoch":@(frame.epoch),
                            @"sequence":@(frame.sequence), @"surfaceID":@(frame.ioSurfaceID),
                            @"gpu_rgba_sha256":string(digest.hex()), @"receipt":receipt}];
                    }
                    ++completed;
                    pump(intervalMs);
                }
            }
            passed = completed == requested;
            result = passed ? 0 : 1;
        } catch (const Unavailable &failure) {
            report[@"error"] = string(failure.what());
            unavailable = true;
            result = 77;
        } catch (const std::exception &failure) {
            report[@"error"] = string(failure.what());
            result = 1;
        }
        if (window) [window close];
        report[@"status"] = passed ? @"PASS" : (unavailable ? @"NOT_RUN" : @"FAIL");
        report[@"passed"] = @(passed);
        report[@"requested_frames"] = @(requested);
        report[@"frames_completed"] = @(completed);
        report[@"interval_ms"] = @(intervalMs);
        report[@"gpu_execution_attempted"] = @(attempted);
        const BOOL validated = completed > 0;
        report[@"gpu_render_submitted"] = @(validated);
        report[@"gpu_fence_signaled"] = @(validated);
        report[@"gpu_readback_completed"] = @(validated);
        report[@"gpu_resources_released"] = @(validated);
        report[@"iosurface_gpu_written"] = @(validated);
        report[@"surface_id_correlated"] = @(validated);
        report[@"snapshot_rgba_matches_gpu_readback"] = @(validated);
        report[@"gpu_output_values_verified"] = @(validated);
        report[@"independent_pixels_verified"] = @(verifiedPixels);
        report[@"snapshot_alpha_mode"] = @"straight";
        report[@"calayer_transaction_commit_called"] = @(validated);
        report[@"snapshot_bytes_compared"] = @(comparedBytes);
        report[@"samples"] = samples;
        NSError *jsonError = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:&jsonError];
        if (!json) { fprintf(stderr, "Cannot serialize acceptance report\n"); return 1; }
        if (!reportPath.empty() && ![json writeToFile:string(reportPath) options:NSDataWritingAtomic error:&jsonError]) {
            fprintf(stderr, "Cannot write acceptance report\n"); return 1;
        }
        fwrite(json.bytes, 1, json.length, stdout);
        fputc('\n', stdout);
        return result;
    }
}
