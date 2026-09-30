// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN
FOUNDATION_EXPORT NSString * const MellowAppleMetalErrorDomain;
FOUNDATION_EXPORT NSExceptionName const MellowAppleMetalUnsupportedException;
FOUNDATION_EXPORT NSString * const MellowAppleMetalInitializationStatusKey;
FOUNDATION_EXPORT NSString * const MellowAppleMetalBootstrapSubmissionAttemptedKey;

// Explicit application opt-in. The index selects a real host OpenCL GPU;
// initialization runs the production provider's GPU bootstrap verification.
// This implements ONLY the selector-compatible compute subset documented in
// README.md, not full MTLDevice protocol conformance or system registration.
// Unimplemented selectors raise MellowAppleMetalUnsupportedException.
// No CPU provider, synthetic completion, driver install or WindowServer path.
#ifdef __cplusplus
extern "C" {
#endif
FOUNDATION_EXPORT id<MTLDevice> _Nullable MellowCreateDevice(
    NSUInteger openCLGPUIndex, NSError * _Nullable * _Nullable error) NS_RETURNS_RETAINED;
FOUNDATION_EXPORT NSDictionary<NSString *, id> * _Nullable MellowCopyAdapterCapabilities(
    id<MTLDevice> device) NS_RETURNS_RETAINED;
// A snapshot of actual production OpenCL events after driver completion. These
// facts do not establish independent arithmetic correctness or PCI identity.
FOUNDATION_EXPORT NSArray<NSDictionary<NSString *, id> *> * _Nullable MellowCopyCommandExecutionEvidence(
    id<MTLCommandBuffer> commandBuffer) NS_RETURNS_RETAINED;
#ifdef __cplusplus
}
#endif
NS_ASSUME_NONNULL_END
