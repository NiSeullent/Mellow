// SPDX-License-Identifier: MIT
#pragma once
#import "MellowAppleMetal.h"
#import <IOSurface/IOSurface.h>
NS_ASSUME_NONNULL_BEGIN
#ifdef __cplusplus
extern "C" {
#endif
// Separate opt-in CGL render device. No inferred sharing with MellowCreateDevice.
FOUNDATION_EXPORT id<MTLDevice> _Nullable MellowCreateRenderDevice(
    NSError * _Nullable * _Nullable error) NS_RETURNS_RETAINED;
FOUNDATION_EXPORT NSDictionary<NSString *,id> * _Nullable MellowCopyRenderAdapterCapabilities(
    id<MTLDevice> device) NS_RETURNS_RETAINED;
FOUNDATION_EXPORT NSArray<NSDictionary<NSString *,id> *> * _Nullable MellowCopyRenderCommandExecutionEvidence(
    id<MTLCommandBuffer> commandBuffer) NS_RETURNS_RETAINED;
// Borrowed completed BGRA8 straight-alpha surface, row zero bottom-left. Retain
// the texture and exclude further GPU writes while the consumer reads/copies it.
// Null before successful completion and after an invalidated/failed GPU write.
// Call the existing SurfacePresenter on the main thread with sourceBottomLeft=YES.
FOUNDATION_EXPORT IOSurfaceRef _Nullable MellowGetCompletedRenderTextureIOSurface(id<MTLTexture> texture)
    CF_RETURNS_NOT_RETAINED;
#ifdef __cplusplus
}
#endif
NS_ASSUME_NONNULL_END
