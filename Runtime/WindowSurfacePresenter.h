// SPDX-License-Identifier: MIT
#pragma once
#if !defined(__APPLE__) || !defined(__OBJC__)
#error WindowSurfacePresenter requires macOS Objective-C++ and the Apple SDK.
#endif
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN
// An explicit application-owned Core Animation presentation path. It consumes
// completed Mellow native-render textures; it does not register a system Metal
// device, implement CAMetalDrawable, or supply a WindowServer GPU driver.
@interface MellowWindowSurfacePresenter : NSObject
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithView:(NSView *)view
                               error:(NSError * _Nullable * _Nullable)error;
// Main thread only. The input must have completed on its owning Mellow render
// device. The method copies that actual GPU readback into a new private surface.
// A successful return means queued for layer drawing, not display/scanout.
- (BOOL)enqueueCompletedTexture:(id<MTLTexture>)texture
                         error:(NSError * _Nullable * _Nullable)error;
// Main thread only. Releases the host connection; outstanding CGL work retains
// its surface until its fence completes or the owning context is destroyed.
- (void)detach;
// Thread-safe snapshot of observed queue/draw/fence state and renderer strings.
- (NSDictionary<NSString *, id> *)copyEvidence;
@end
NS_ASSUME_NONNULL_END
