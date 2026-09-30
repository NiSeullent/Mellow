// SPDX-License-Identifier: MIT
// Native macOS integration client. It uses the explicit Mellow selector adapters
// and reports failures; it never substitutes MTLCreateSystemDefaultDevice().
#import "../Runtime/NativeMetalCompute.h"
#import "../Runtime/NativeMetalRender.h"
#import "../Runtime/WindowSurfacePresenter.h"
#include "../tests/render_fixture.hpp"
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char *message, NSError *error = nil) {
    if (!condition) throw std::runtime_error(std::string(message) +
        (error ? std::string(": ") + error.localizedDescription.UTF8String : ""));
}
NSDictionary *compute(NSUInteger index) {
    NSError *error = nil;
    id<MTLDevice> device = MellowCreateNativeComputeDevice(index, &error);
    require(device != nil, "Compute device bootstrap failed", error);
    NSString *source = @"kernel void apply(device uint *x [[buffer(0)]], uint i [[thread_position_in_grid]]) { x[i] = x[i] * 7u + 3u; }";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil, "Compute source compilation failed", error);
    id<MTLFunction> function = [library newFunctionWithName:@"apply"];
    require(function != nil, "Compute function was not resolved");
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    require(pipeline != nil, "Compute GPU pipeline compilation failed", error);
    const std::array<uint32_t, 8> input = {0, 1, 7, 103, 0x10203040u, 0xFFFFFFFEu, 19, 41};
    id<MTLBuffer> buffer = [device newBufferWithBytes:input.data() length:sizeof(input)
                                           options:MTLResourceStorageModeShared];
    require(buffer != nil, "Compute buffer allocation failed");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    require(queue != nil, "Compute queue creation failed");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    require(command != nil, "Compute command buffer creation failed");
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    require(encoder != nil, "Compute encoder creation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(input.size(), 1, 1)
       threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "Compute GPU command failed", command.error);
    const auto *output = static_cast<const uint32_t *>(buffer.contents);
    require(output != nullptr, "Compute result storage unavailable");
    NSMutableArray *actual = [NSMutableArray array];
    for (size_t i = 0; i < input.size(); ++i) {
        const uint32_t expected = input[i] * 7u + 3u;
        require(output[i] == expected, "Compute GPU readback differs from independent expected result");
        [actual addObject:@(output[i])];
    }
    return @{@"route": @"explicit-selector/OpenCL-GPU", @"readback_matched": @YES,
             @"values": actual, @"system_metal_registered": @NO,
             @"unsupported_gpu_driver_verified": @NO};
}
id<MTLTexture> render(id<MTLDevice> *deviceOut, NSDictionary **evidenceOut) {
    NSError *error = nil;
    id<MTLDevice> device = MellowCreateNativeRenderDevice(&error);
    require(device != nil, "Render CGL device initialization failed", error);
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:MellowRenderFixture]
                                                options:nil error:&error];
    require(library != nil, "Render source validation failed", error);
    MTLRenderPipelineDescriptor *pipelineDescriptor = [MTLRenderPipelineDescriptor new];
    pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"triangleVertex"];
    pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"gradientFragment"];
    pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    require(pipelineDescriptor.vertexFunction && pipelineDescriptor.fragmentFunction, "Render functions were not resolved");
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    require(pipeline != nil, "Render GPU pipeline compilation failed", error);
    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                        width:128 height:128 mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
    require(texture != nil, "Render texture creation failed", MellowNativeRenderLastError((id)device));
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    require(queue != nil, "Render queue creation failed", MellowNativeRenderLastError((id)device));
    id<MTLCommandBuffer> command = [queue commandBuffer];
    require(command != nil, "Render command creation failed", MellowNativeRenderLastError((id)queue));
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    require(encoder != nil, "Render encoder creation failed", command.error);
    const std::array<float, 4> parameters = {0.f, 0.f, 0.37f, 1.f / 128.f};
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBytes:parameters.data() length:sizeof(parameters) atIndex:0];
    [encoder setFragmentBytes:parameters.data() length:sizeof(parameters) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "Render GPU command failed", command.error);
    NSUInteger width {}, height {}; uint64_t sequence {};
    NSData *pixels = MellowCopyNativeRenderTextureRGBA8(texture, &width, &height, &sequence, &error);
    require(pixels && width == 128 && height == 128 && sequence, "Render completed snapshot unavailable", error);
    const auto *rgba = static_cast<const uint8_t *>(pixels.bytes);
    // Fragment position is independent of the triangle's vertex interpolation.
    // Interior center is about (0.504,0.504,0.37,1), while corner is clear.
    const size_t center = (64 * width + 64) * 4;
    require(rgba[center] >= 126 && rgba[center] <= 131 && rgba[center + 1] >= 123 && rgba[center + 1] <= 130 &&
            rgba[center + 2] >= 93 && rgba[center + 2] <= 95 && rgba[center + 3] == 255,
            "Render interior differs from independent shader expectation");
    require(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0, "Render clear corner differs");
    NSDictionary *identity = MellowNativeRenderDeviceEvidence(device, &error);
    require(identity != nil, "Render device evidence unavailable", error);
    *deviceOut = device;
    *evidenceOut = @{@"route": @"explicit-selector/CGL-GPU", @"readback_matched": @YES,
                    @"sequence": @(sequence), @"device": identity};
    return texture;
}
void emit(NSDictionary *record) {
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:&error];
    require(json != nil, "Evidence JSON serialization failed", error);
    fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout);
}
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
      @try {
        try {
            require(argc == 2, "Usage: native-metal-client --compute | --render | --window");
            if (!strcmp(argv[1], "--compute")) { emit(compute(0)); return 0; }
            require(!strcmp(argv[1], "--render") || !strcmp(argv[1], "--window"), "Unknown native client mode");
            id<MTLDevice> device = nil; NSDictionary *renderEvidence = nil;
            id<MTLTexture> texture = render(&device, &renderEvidence);
            if (!strcmp(argv[1], "--render")) { emit(renderEvidence); return 0; }
            require([NSThread isMainThread], "Window test must run on the main thread");
            NSApplication *app = [NSApplication sharedApplication];
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 512, 512)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
            window.title = @"Mellow GPU render → IOSurface → Core Animation";
            window.releasedWhenClosed = NO;
            NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 512, 512)];
            window.contentView = view;
            NSError *error = nil;
            MellowWindowSurfacePresenter *presenter = [[MellowWindowSurfacePresenter alloc] initWithView:view error:&error];
            require(presenter != nil, "Window surface presenter creation failed", error);
            [app finishLaunching];
            [window makeKeyAndOrderFront:nil]; [app activateIgnoringOtherApps:YES];
            require([presenter enqueueCompletedTexture:texture error:&error], "Completed frame queue failed", error);
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
            NSDictionary *display = nil;
            do {
                [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
                display = [presenter copyEvidence];
                if ([display[@"error"] length] || [display[@"consumption_fences"] unsignedLongLongValue]) break;
            } while ([deadline timeIntervalSinceNow] > 0);
            [presenter detach]; [window close];
            require([display[@"error"] length] == 0 && [display[@"consumption_fences"] unsignedLongLongValue] > 0,
                    "Core Animation layer draw did not produce a consumption fence");
            // Layer work does not prove that WindowServer displayed or scanned
            // out the requested content, nor that an unsupported GPU owns it.
            emit(@{@"render": renderEvidence, @"layer": display,
                   @"windowserver_accelerator_verified": @NO, @"display_scanout_verified": @NO});
            return 0;
        } catch (const std::exception &error) { fprintf(stderr, "%s\n", error.what()); return 1; }
      }
        @catch (NSException *exception) { fprintf(stderr, "%s: %s\n", exception.name.UTF8String, exception.reason.UTF8String); return 2; }
    }
}
