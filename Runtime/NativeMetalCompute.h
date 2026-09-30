#pragma once

#if defined(__APPLE__) && defined(__OBJC__)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSErrorDomain const MellowNativeComputeErrorDomain;
FOUNDATION_EXPORT NSExceptionName const MellowNativeComputeUnsupportedException;

// Explicit opt-in Objective-C selector adapter over Mellow's actual OpenCL GPU
// runtime. This does not register a system Metal device or an IOAccelerator.
// The returned objects implement the documented compute selector subset below;
// they deliberately do not claim complete conformance to Apple's protocols.
// Build the implementation as Objective-C++ with ARC, blocks and C++17 on macOS.
// Creation performs the existing runtime's actual GPU bootstrap dispatch.
FOUNDATION_EXPORT id<MTLDevice> _Nullable MellowCreateNativeComputeDevice(
    NSUInteger openCLGPUIndex, NSError * _Nullable * _Nullable error);
FOUNDATION_EXPORT BOOL MellowIsNativeComputeAdapter(id _Nullable object);

// AIR is explicitly named input, not a claim that arbitrary .metallib containers
// are supported. The decoder requires an explicit absolute shared-library path
// and actually loads/checks its LLVM C API (supported majors 18 through 20).
FOUNDATION_EXPORT id<MTLLibrary> _Nullable MellowNativeComputeNewLibraryWithAIR(
    id<MTLDevice> device, NSData *bitcode, NSString *entry,
    NSString *llvmLibraryPath, NSError * _Nullable * _Nullable error);
FOUNDATION_EXPORT id<MTLLibrary> _Nullable MellowNativeComputeNewLibraryWithAIRText(
    id<MTLDevice> device, NSString *assembly, NSString *entry,
    NSError * _Nullable * _Nullable error);

// Supported caller sequence:
//   newLibraryWithSource:options:nil error: -> newFunctionWithName:
//   newComputePipelineStateWithFunction:error: -> newCommandQueue
//   newBufferWithLength:options: / newBufferWithBytes:length:options:
//   commandBuffer -> computeCommandEncoder -> setComputePipelineState:
//   setBuffer:offset:0 atIndex:0 -> dispatchThreads:threadsPerThreadgroup:
//   endEncoding -> [addScheduledHandler: / addCompletedHandler:] -> commit
//   waitUntilScheduled / waitUntilCompleted -> inspect status/error/contents.
//
// Buffers are shared, default-cache uint32 arrays, 4..16384 bytes. The stable
// contents allocation lives until the buffer is released. Grid dimensions must
// be {length/4,1,1}; the supported threadgroup request is {1,1,1}. This shader
// subset has no group-local operations; OpenCL chooses its actual local size.
// Only one writable buffer(0) and the existing typed single-kernel MSL/AIR
// subset are accepted. No threadExecutionWidth, GPU identity, feature family,
// GPU timestamp or render/presentation capability is invented.
//
// Commit is asynchronous on a serial per-device worker. Buffer bytes are copied
// to the actual runtime before submission and back only after verified GPU
// completion/readback. Scheduled handlers are conservatively deferred until
// the synchronous provider returns submission evidence. Completion wait includes
// all registered handlers. Do not wait from a handler on the device worker.
// Each command buffer and its encoders require one encoding thread; separate
// command buffers may be encoded concurrently after reserving their queue order.
// Keep the command queue alive while it holds unsubmitted reservations. Releasing
// such a queue cancels reservations; committed commands waiting behind them
// report Error and invoke completion handlers. Unreserved/dispatched commands
// retain the queue; every command independently retains its device/backend.
// Unsupported selectors raise MellowNativeComputeUnsupportedException; foreign
// resources and invalid encoding raise NSInvalidArgumentException. Allocation
// failure returns nil; compilation and factory failures use NSError.
NS_ASSUME_NONNULL_END
#endif
