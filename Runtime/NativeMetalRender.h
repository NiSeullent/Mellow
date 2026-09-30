// SPDX-License-Identifier: MIT
#pragma once
#if !defined(__APPLE__) || !defined(__OBJC__)
#error NativeMetalRender requires the real macOS Foundation and Metal SDKs.
#endif
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN
FOUNDATION_EXPORT NSString * const MellowNativeRenderErrorDomain;
FOUNDATION_EXPORT NSString * const MellowNativeRenderUnsupportedSelectorException;
typedef NS_ENUM(NSInteger, MellowNativeRenderErrorCode) {
    MellowNativeRenderInvalidArgument = 1,
    MellowNativeRenderInvalidState = 2,
    MellowNativeRenderWrongDevice = 3,
    MellowNativeRenderUnsupported = 4,
    MellowNativeRenderCompilation = 5,
    MellowNativeRenderExecution = 6,
};

// Explicit application opt-in. Objects implement the documented selector subset;
// they do not assert complete MTL protocol conformance or register a system device.
// Build the .mm implementation with ARC and the real Foundation/Metal/OpenGL SDK.
// Factory initialization requires an actual accelerated CGL provider. It never
// delegates work to an Apple Metal device or uses a software renderer.
//
// Supported path: newLibraryWithSource:options:error: (options must be nil),
// newFunctionWithName:, newRenderPipelineStateWithDescriptor:error:,
// newTextureWithDescriptor:, newCommandQueue, commandBuffer,
// renderCommandEncoderWithDescriptor:, setRenderPipelineState:,
// setVertexBytes:length:atIndex:, setFragmentBytes:length:atIndex:,
// drawPrimitives:vertexStart:vertexCount:, endEncoding, commit,
// waitUntilCompleted, status/error and full-texture getBytes readback.
// No compute/CL interop, buffers, drawables, presentation, system/family capability
// claims, shader textures, depth/stencil, blending, MSAA, mipmaps or texture views.
// Unsupported selectors raise the named exception. Invalid supported encoding
// operations poison their command buffer; inspect its error/status before use.
// Queues and resources retain the selected device; its monitor serializes calls.
// Synchronous commit can block in the driver; use an externally timed process.
FOUNDATION_EXPORT id<MTLDevice> _Nullable
MellowCreateNativeRenderDevice(NSError * _Nullable * _Nullable error);

// RGBA8Unorm, 2D, depth/array/sample/mipmap count 1, shared/default-cache storage,
// hazard tracking default and usage exactly RenderTarget are admitted. Render
// passes use one color target, Clear/Store and no resolve/depth/stencil attachment.
// Each draw is Triangle, vertexStart=0, vertexCount=3. A copied, finite float4
// (16 bytes) must be bound independently to both stages at index 0 and match
// byte-for-byte. The existing bounded MSL frontend supplies vertex_id semantics.
//
// Returns copied tightly packed top-left RGBA8 only after correlated successful
// GPU rendering. Encoding new content invalidates an earlier snapshot, including
// when that command is abandoned or fails. No caller-provided pixels are admitted.
// A command superseded by another command targeting the same texture is rejected
// before GPU submission; its failure cannot invalidate the newer command's result.
// Output dimensions/sequence are reset to zero on failure. This is an ARC-managed
// Objective-C result, not a CF object. Presenter must handle a nil result.
FOUNDATION_EXPORT NSData * _Nullable
MellowCopyNativeRenderTextureRGBA8(id<MTLTexture> texture,
                                 NSUInteger * _Nullable width,
                                 NSUInteger * _Nullable height,
                                 uint64_t * _Nullable sequence,
                                 NSError * _Nullable * _Nullable error);
FOUNDATION_EXPORT NSError * _Nullable MellowNativeRenderLastError(id object);
// Driver-reported CGL identity only; physical PCI/system Metal/WindowServer fields
// stay false. This record does not certify any unsupported GPU driver.
FOUNDATION_EXPORT NSDictionary<NSString *, id> * _Nullable
MellowNativeRenderDeviceEvidence(id<MTLDevice> device,
                                NSError * _Nullable * _Nullable error);
NS_ASSUME_NONNULL_END
