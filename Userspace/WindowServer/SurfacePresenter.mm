// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#import "SurfacePresenter.h"
#import <IOKit/IOReturn.h>
#include "SurfaceSnapshot.hpp"
#include <stdint.h>
#if !__has_feature(objc_arc)
#error Compile SurfacePresenter.mm with -fobjc-arc.
#endif

NSErrorDomain const MellowSurfacePresentationErrorDomain = @"org.mellow.SurfacePresentation";
namespace {
void fail(NSError **error, MellowSurfacePresentationError code, NSString *message,
          int32_t nativeStatus = 0) {
    if (error) *error = [NSError errorWithDomain:MellowSurfacePresentationErrorDomain
        code:code userInfo:@{NSLocalizedDescriptionKey:message, @"nativeStatus":@(nativeStatus)}];
}
struct SurfaceReference {
    IOSurfaceRef value;
    explicit SurfaceReference(IOSurfaceRef surface) : value(surface) { CFRetain(value); }
    ~SurfaceReference() { CFRelease(value); }
};
}

CGImageRef MellowCopyCompletedSurfaceImage(IOSurfaceRef surface, BOOL bottomLeft,
                                          NSError **error) {
    if (error) *error = nil;
    if (!surface) {
        fail(error, MellowSurfacePresentationInvalidArgument, @"An IOSurface is required.");
        return nullptr;
    }
    SurfaceReference hold(surface);
    if (IOSurfaceGetPlaneCount(surface) != 0 || IOSurfaceGetPixelFormat(surface) != 0x42475241U) {
        fail(error, MellowSurfacePresentationInvalidSurface, @"Expected nonplanar BGRA8.");
        return nullptr;
    }
    Mellow::Presentation::SnapshotLayout layout;
    if (Mellow::Presentation::validateSnapshotLayout(IOSurfaceGetWidth(surface),
            IOSurfaceGetHeight(surface), IOSurfaceGetBytesPerElement(surface),
            IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface), layout)
            != Mellow::Presentation::SnapshotStatus::Ok) {
        fail(error, MellowSurfacePresentationInvalidSurface, @"Invalid or oversized IOSurface layout.");
        return nullptr;
    }
    const IOReturn lockStatus = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    if (lockStatus != kIOReturnSuccess) {
        fail(error, MellowSurfacePresentationLockFailed, @"IOSurface read lock failed.",
             static_cast<int32_t>(lockStatus));
        return nullptr;
    }
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, static_cast<CFIndex>(layout.imageBytes));
    bool copied = false;
    if (data) {
        CFDataSetLength(data, static_cast<CFIndex>(layout.imageBytes));
        copied = Mellow::Presentation::copySnapshotRows(IOSurfaceGetBaseAddress(surface),
            layout.sourceBytes, layout, bottomLeft != NO, CFDataGetMutableBytePtr(data),
            static_cast<size_t>(CFDataGetLength(data))) == Mellow::Presentation::SnapshotStatus::Ok;
    }
    const IOReturn unlockStatus = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    if (!copied || unlockStatus != kIOReturnSuccess) {
        if (data) CFRelease(data);
        fail(error, unlockStatus != kIOReturnSuccess ? MellowSurfacePresentationUnlockFailed
             : MellowSurfacePresentationAllocationFailed,
             unlockStatus != kIOReturnSuccess ? @"IOSurface unlock failed." : @"Surface snapshot failed.",
             static_cast<int32_t>(unlockStatus));
        return nullptr;
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = nullptr;
    if (provider && colorSpace) {
        const CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Little |
            static_cast<CGBitmapInfo>(kCGImageAlphaFirst);
        image = CGImageCreate(layout.width, layout.height, 8, 32, layout.imageRowBytes,
            colorSpace, bitmapInfo, provider, nullptr, false, kCGRenderingIntentDefault);
    }
    if (colorSpace) CGColorSpaceRelease(colorSpace);
    if (provider) CGDataProviderRelease(provider);
    if (!image) fail(error, MellowSurfacePresentationAllocationFailed, @"CGImage snapshot creation failed.");
    return image;
}

@implementation MellowSurfacePresenter {
    CALayer *_layer;
    uint64_t _sequence;
}
- (instancetype)initWithLayer:(CALayer *)layer {
    if (!layer || ![NSThread isMainThread]) return nil;
    self = [super init];
    if (self) _layer = layer;
    return self;
}
- (NSDictionary<NSString *, id> *)presentCompletedSurface:(IOSurfaceRef)surface
    sourceBottomLeft:(BOOL)bottomLeft error:(NSError **)error {
    if (error) *error = nil;
    if (![NSThread isMainThread]) {
        fail(error, MellowSurfacePresentationWrongThread, @"Layer presentation requires the main thread.");
        return nil;
    }
    if (_sequence == UINT64_MAX) {
        fail(error, MellowSurfacePresentationSequenceExhausted, @"Presentation sequence is exhausted.");
        return nil;
    }
    CGImageRef image = MellowCopyCompletedSurfaceImage(surface, bottomLeft, error);
    if (!image) return nil;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _layer.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
    ++_sequence;
    return @{@"scope":@"application-layer", @"sequence":@(_sequence),
        @"surfaceID":@(IOSurfaceGetID(surface)), @"width":@(IOSurfaceGetWidth(surface)),
        @"height":@(IOSurfaceGetHeight(surface)), @"sourceBottomLeft":@(bottomLeft),
        @"snapshotCreated":@YES, @"transactionCommitCalled":@YES,
        @"scanoutObserved":@NO, @"systemMetalRegistration":@NO,
        @"systemWindowServerAcceleration":@NO};
}
@end
