// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#pragma once
#if !defined(__APPLE__) || !defined(__OBJC__)
#error SurfacePresenter requires macOS Objective-C or Objective-C++.
#endif
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>

NS_ASSUME_NONNULL_BEGIN
FOUNDATION_EXPORT NSErrorDomain const MellowSurfacePresentationErrorDomain;
typedef NS_ERROR_ENUM(MellowSurfacePresentationErrorDomain, MellowSurfacePresentationError) {
    MellowSurfacePresentationInvalidArgument = 1,
    MellowSurfacePresentationInvalidSurface,
    MellowSurfacePresentationLockFailed,
    MellowSurfacePresentationAllocationFailed,
    MellowSurfacePresentationUnlockFailed,
    MellowSurfacePresentationWrongThread,
    MellowSurfacePresentationSequenceExhausted
};

// Caller has completed all GPU writes and owns exclusive access for this call.
// BGRA has straight alpha in sRGB, nonplanar, four bytes per pixel. This copies the
// surface into an independent immutable image; the source may be reused after
// success. Locking is cache synchronization, not proof of producer completion.
FOUNDATION_EXPORT CGImageRef _Nullable MellowCopyCompletedSurfaceImage(
    IOSurfaceRef surface, BOOL sourceBottomLeft, NSError * _Nullable * _Nullable error)
    CF_RETURNS_RETAINED;

// Attach a dedicated layer as a sublayer of an application's view-backed layer.
// Caller sets its frame/contentsScale. All object methods run on the main thread.
// The receipt proves only that a snapshot was made and CATransaction commit was
// called. It does not prove scanout, system GPU registration or driver adoption.
@interface MellowSurfacePresenter : NSObject
- (nullable instancetype)initWithLayer:(CALayer *)layer NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable NSDictionary<NSString *, id> *)presentCompletedSurface:(IOSurfaceRef)surface
    sourceBottomLeft:(BOOL)sourceBottomLeft
    error:(NSError * _Nullable * _Nullable)error;
@end
NS_ASSUME_NONNULL_END
