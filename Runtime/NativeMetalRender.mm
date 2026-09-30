// SPDX-License-Identifier: MIT
#import "NativeMetalRender.h"
#include "RenderObjects.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if !__has_feature(objc_arc)
#error NativeMetalRender.mm must be compiled with -fobjc-arc.
#endif

// Public API contracts:
// https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html
// https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Mem-Obj/Mem-Obj.html
// https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html
// https://developer.apple.com/documentation/metal/mtlrenderpassdescriptor/getsamplepositions:count:
// These partial objects intentionally do not declare <MTLDevice>, <MTLTexture>,
// etc. Implementing a selector subset is not full public protocol conformance.
NSString * const MellowNativeRenderErrorDomain = @"org.mellow.native-render";
NSString * const MellowNativeRenderUnsupportedSelectorException = @"MellowNativeRenderUnsupportedSelector";

@class MellowNativeRenderDevice, MellowNativeRenderTexture;
@class MellowNativeRenderFunction, MellowNativeRenderCommand, MellowNativeRenderEncoder;

static NSString *RenderString(const std::string &value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size()
                                  encoding:NSUTF8StringEncoding] ?: @"Non-UTF-8 provider diagnostic";
}
static NSError *RenderError(MellowNativeRenderErrorCode code, NSString *message) {
    return [NSError errorWithDomain:MellowNativeRenderErrorDomain code:code
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}
static NSError *RenderError(const MellowMTL::Error &error) {
    switch (error.code) {
        case MellowMTL::ErrorCode::InvalidArgument: return RenderError(MellowNativeRenderInvalidArgument, RenderString(error.message));
        case MellowMTL::ErrorCode::InvalidState: return RenderError(MellowNativeRenderInvalidState, RenderString(error.message));
        case MellowMTL::ErrorCode::WrongDevice: return RenderError(MellowNativeRenderWrongDevice, RenderString(error.message));
        case MellowMTL::ErrorCode::Unsupported: return RenderError(MellowNativeRenderUnsupported, RenderString(error.message));
        case MellowMTL::ErrorCode::Compilation: return RenderError(MellowNativeRenderCompilation, RenderString(error.message));
        case MellowMTL::ErrorCode::None:
        case MellowMTL::ErrorCode::Execution: return RenderError(MellowNativeRenderExecution, RenderString(error.message));
    }
    return RenderError(MellowNativeRenderExecution, @"Unknown render error");
}
static void StoreError(NSError **destination, NSError *error) {
    if (destination) *destination = error;
}
static bool SourceBytes(NSString *source, std::string &out, size_t maximum) {
    if (![source isKindOfClass:[NSString class]] || !source.length || source.length > maximum) return false;
    NSData *bytes = [source dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    if (!bytes || !bytes.length || bytes.length > maximum ||
        std::memchr(bytes.bytes, 0, bytes.length)) return false;
    out.assign(static_cast<const char *>(bytes.bytes), bytes.length);
    return true;
}

@interface MellowNativeRenderObject : NSObject {
@public
    MellowNativeRenderDevice *_owner;
    NSError *_lastError;
    NSString *_label;
}
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (void)rejectSelector:(SEL)selector;
@end

@interface MellowNativeRenderDevice : MellowNativeRenderObject {
@public
    std::shared_ptr<MellowMTL::RenderDevice> _render;
    uint64_t _nextTicket;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor;
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
- (id<MTLCommandQueue>)newCommandQueue;
@end

@interface MellowNativeRenderTexture : MellowNativeRenderObject {
@public
    std::shared_ptr<MellowMTL::RenderTexture> _texture;
    uint64_t _latestTicket, _completedSequence;
    BOOL _contentValid;
}
- (NSData *)snapshot:(NSError **)error;
@end

@interface MellowNativeRenderLibrary : MellowNativeRenderObject {
@public
    std::shared_ptr<MellowMTL::RenderLibrary> _library;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
@end

@interface MellowNativeRenderFunction : MellowNativeRenderObject {
@public
    std::shared_ptr<MellowMTL::RenderFunction> _function;
}
@end

@interface MellowNativeRenderPipeline : MellowNativeRenderObject {
@public
    std::shared_ptr<MellowMTL::RenderPipeline> _pipeline;
}
@end

@interface MellowNativeRenderQueue : MellowNativeRenderObject {
@public
    std::shared_ptr<MellowMTL::RenderCommandQueue> _queue;
}
- (id<MTLCommandBuffer>)commandBuffer;
@end

@interface MellowNativeRenderCommand : MellowNativeRenderObject {
@public
    MellowNativeRenderQueue *_queueObject;
    std::shared_ptr<MellowMTL::RenderCommandBuffer> _work;
    NSMutableArray<MellowNativeRenderTexture *> *_targets;
    std::vector<uint64_t> _tickets;
    BOOL _failed, _committed, _completed, _encoding;
    NSError *_terminalError;
}
- (void)fail:(NSError *)error;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor;
- (void)commit;
- (void)waitUntilCompleted;
- (MTLCommandBufferStatus)status;
- (NSError *)error;
@end

@interface MellowNativeRenderEncoder : MellowNativeRenderObject {
@public
    MellowNativeRenderCommand *_command;
    std::shared_ptr<MellowMTL::RenderEncoder> _encoder;
    std::array<unsigned char, 16> _vertexBytes, _fragmentBytes;
    BOOL _vertexBound, _fragmentBound, _ended, _drew;
}
- (BOOL)active;
- (void)fail:(NSError *)error;
- (void)bindBytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index vertex:(BOOL)vertex;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline;
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count;
- (void)endEncoding;
@end

@implementation MellowNativeRenderObject
- (NSString *)label { @synchronized (_owner ?: self) { return _label; } }
- (void)setLabel:(NSString *)label { @synchronized (_owner ?: self) { _label = [label copy]; } }
- (void)rejectSelector:(SEL)selector {
    _lastError = RenderError(MellowNativeRenderUnsupported,
                            [@"Unsupported native render selector: " stringByAppendingString:NSStringFromSelector(selector)]);
    @throw [NSException exceptionWithName:MellowNativeRenderUnsupportedSelectorException
                                  reason:_lastError.localizedDescription userInfo:@{@"error": _lastError}];
}
- (void)doesNotRecognizeSelector:(SEL)selector {
    @synchronized (_owner ?: self) { [self rejectSelector:selector]; }
}
@end

static bool AdmittedTexture(MTLTextureDescriptor *d) {
    const MTLTextureSwizzleChannels s = d.swizzle;
    return d.textureType == MTLTextureType2D && d.pixelFormat == MTLPixelFormatRGBA8Unorm &&
           d.width && d.width <= MellowRT::OpenGLProvider::MaxDimension &&
           d.height && d.height <= MellowRT::OpenGLProvider::MaxDimension &&
           d.depth == 1 && d.arrayLength == 1 && d.mipmapLevelCount == 1 && d.sampleCount == 1 &&
           d.storageMode == MTLStorageModeShared && d.cpuCacheMode == MTLCPUCacheModeDefaultCache &&
           d.hazardTrackingMode == MTLHazardTrackingModeDefault && d.usage == MTLTextureUsageRenderTarget &&
           s.red == MTLTextureSwizzleRed && s.green == MTLTextureSwizzleGreen &&
           s.blue == MTLTextureSwizzleBlue && s.alpha == MTLTextureSwizzleAlpha;
}
static bool AdmittedPipeline(MTLRenderPipelineDescriptor *d) {
    MTLRenderPipelineDescriptor *defaults = [MTLRenderPipelineDescriptor new];
    if (d.vertexDescriptor || d.rasterSampleCount != 1 || !d.rasterizationEnabled ||
        d.alphaToCoverageEnabled || d.alphaToOneEnabled ||
        d.depthAttachmentPixelFormat != MTLPixelFormatInvalid ||
        d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid ||
        d.inputPrimitiveTopology != MTLPrimitiveTopologyClassUnspecified ||
        d.supportIndirectCommandBuffers || d.maxVertexAmplificationCount != 1 ||
        d.vertexLinkedFunctions || d.fragmentLinkedFunctions || d.binaryArchives.count ||
        d.supportAddingVertexBinaryFunctions || d.supportAddingFragmentBinaryFunctions ||
        d.maxVertexCallStackDepth != defaults.maxVertexCallStackDepth ||
        d.maxFragmentCallStackDepth != defaults.maxFragmentCallStackDepth ||
        d.tessellationPartitionMode != defaults.tessellationPartitionMode ||
        d.maxTessellationFactor != defaults.maxTessellationFactor ||
        d.tessellationFactorScaleEnabled != defaults.tessellationFactorScaleEnabled ||
        d.tessellationFactorFormat != defaults.tessellationFactorFormat ||
        d.tessellationControlPointIndexType != defaults.tessellationControlPointIndexType ||
        d.tessellationFactorStepFunction != defaults.tessellationFactorStepFunction ||
        d.tessellationOutputWindingOrder != defaults.tessellationOutputWindingOrder) return false;
    for (NSUInteger i = 0; i < 31; ++i)
        if (d.vertexBuffers[i].mutability != MTLMutabilityDefault ||
            d.fragmentBuffers[i].mutability != MTLMutabilityDefault) return false;
    for (NSUInteger i = 0; i < 8; ++i) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat != (i ? MTLPixelFormatInvalid : MTLPixelFormatRGBA8Unorm) ||
            a.blendingEnabled || a.writeMask != MTLColorWriteMaskAll ||
            a.rgbBlendOperation != MTLBlendOperationAdd || a.alphaBlendOperation != MTLBlendOperationAdd ||
            a.sourceRGBBlendFactor != MTLBlendFactorOne || a.sourceAlphaBlendFactor != MTLBlendFactorOne ||
            a.destinationRGBBlendFactor != MTLBlendFactorZero || a.destinationAlphaBlendFactor != MTLBlendFactorZero)
            return false;
    }
    return true;
}

@implementation MellowNativeRenderDevice
- (NSString *)name { return @"Mellow opt-in render selectors / CGL"; }
- (BOOL)supportsFamily:(MTLGPUFamily)family { (void)family; return NO; }
- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet { (void)featureSet; return NO; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)count { return count == 1; }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    @synchronized (self) {
        _lastError = nil;
        if (![descriptor isKindOfClass:[MTLTextureDescriptor class]]) {
            _lastError = RenderError(MellowNativeRenderInvalidArgument, @"A native texture descriptor is required"); return nil;
        }
        MTLTextureDescriptor *d = [descriptor copy];
        if (!AdmittedTexture(d)) {
            _lastError = RenderError(MellowNativeRenderUnsupported, @"Only 2D shared RGBA8Unorm single-level render-target textures with default cache/hazard tracking and RGBA swizzle are admitted"); return nil;
        }
        try {
            MellowMTL::Error error;
            auto texture = _render->newTexture(static_cast<uint32_t>(d.width), static_cast<uint32_t>(d.height), error);
            if (!texture) { _lastError = RenderError(error); return nil; }
            MellowNativeRenderTexture *object = [MellowNativeRenderTexture new];
            object->_owner = self; object->_texture = std::move(texture);
            return (id<MTLTexture>)object;
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); return nil; }
    }
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
    @synchronized (self) {
        StoreError(error, nil); _lastError = nil;
        if (options) { _lastError = RenderError(MellowNativeRenderUnsupported, @"Compile options are unsupported; pass nil"); StoreError(error, _lastError); return nil; }
        try {
            std::string bytes;
            if (!SourceBytes(source, bytes, MellowRT::RenderShaderJit::MaxSourceBytes)) {
                _lastError = RenderError(MellowNativeRenderInvalidArgument, @"Source must contain 1-65536 UTF-8 bytes without NUL"); StoreError(error, _lastError); return nil;
            }
            MellowMTL::Error e; auto library = _render->newLibraryWithSource(bytes, e);
            if (!library) { _lastError = RenderError(e); StoreError(error, _lastError); return nil; }
            MellowNativeRenderLibrary *object = [MellowNativeRenderLibrary new];
            object->_owner = self; object->_library = std::move(library);
            return (id<MTLLibrary>)object;
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); StoreError(error, _lastError); return nil; }
    }
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    @synchronized (self) {
        StoreError(error, nil); _lastError = nil;
        if (![descriptor isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
            _lastError = RenderError(MellowNativeRenderInvalidArgument, @"A native render pipeline descriptor is required"); StoreError(error, _lastError); return nil;
        }
        MTLRenderPipelineDescriptor *d = [descriptor copy];
        if (!AdmittedPipeline(d)) {
            _lastError = RenderError(MellowNativeRenderUnsupported, @"Pipeline must use the bounded vertex_id triangle path, one RGBA8 target, default unblended state, no depth/stencil, MSAA, linked functions or indirect work"); StoreError(error, _lastError); return nil;
        }
        if (![(id)d.vertexFunction isKindOfClass:[MellowNativeRenderFunction class]] ||
            ![(id)d.fragmentFunction isKindOfClass:[MellowNativeRenderFunction class]]) {
            _lastError = RenderError(MellowNativeRenderWrongDevice, @"Both functions must come from this render adapter"); StoreError(error, _lastError); return nil;
        }
        MellowNativeRenderFunction *v = (MellowNativeRenderFunction *)d.vertexFunction;
        MellowNativeRenderFunction *f = (MellowNativeRenderFunction *)d.fragmentFunction;
        if (v->_owner != self || f->_owner != self) {
            _lastError = RenderError(MellowNativeRenderWrongDevice, @"Functions belong to another device"); StoreError(error, _lastError); return nil;
        }
        try {
            MellowMTL::Error e; auto pipeline = _render->newRenderPipeline(v->_function, f->_function, e);
            if (!pipeline) { _lastError = RenderError(e); StoreError(error, _lastError); return nil; }
            MellowNativeRenderPipeline *object = [MellowNativeRenderPipeline new];
            object->_owner = self; object->_pipeline = std::move(pipeline); object->_label = [d.label copy];
            return (id<MTLRenderPipelineState>)object;
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); StoreError(error, _lastError); return nil; }
    }
}
- (id<MTLCommandQueue>)newCommandQueue {
    @synchronized (self) {
        _lastError = nil;
        try {
            MellowNativeRenderQueue *object = [MellowNativeRenderQueue new];
            object->_owner = self; object->_queue = _render->newCommandQueue();
            return (id<MTLCommandQueue>)object;
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); return nil; }
    }
}
@end

@implementation MellowNativeRenderTexture
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (NSUInteger)width { return _texture->width(); }
- (NSUInteger)height { return _texture->height(); }
- (NSUInteger)depth { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return MTLPixelFormatRGBA8Unorm; }
- (MTLTextureUsage)usage { return MTLTextureUsageRenderTarget; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeDefault; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache; }
- (NSData *)snapshot:(NSError **)error {
    @synchronized (_owner) {
        StoreError(error, nil); _lastError = nil;
        if (!_contentValid || !_completedSequence || _texture->contentSequence() != _completedSequence) {
            _lastError = RenderError(MellowNativeRenderInvalidState, @"Texture has no current successfully completed GPU content"); StoreError(error, _lastError); return nil;
        }
        try {
            MellowMTL::Error e; auto bytes = _texture->read(e);
            const size_t expected = static_cast<size_t>(_texture->width()) * _texture->height() * 4;
            if (e.code != MellowMTL::ErrorCode::None || bytes.size() != expected) {
                _lastError = e.code == MellowMTL::ErrorCode::None ? RenderError(MellowNativeRenderExecution, @"GPU readback length mismatch") : RenderError(e);
                StoreError(error, _lastError); return nil;
            }
            return [NSData dataWithBytes:bytes.data() length:bytes.size()];
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); StoreError(error, _lastError); return nil; }
    }
}
- (void)getBytes:(void *)destination bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
    @synchronized (_owner) {
        const NSUInteger stride = self.width * 4;
        if (!destination || level || region.origin.x || region.origin.y || region.origin.z ||
            region.size.width != self.width || region.size.height != self.height || region.size.depth != 1 ||
            row < stride || row > std::numeric_limits<NSUInteger>::max() / self.height) {
            _lastError = RenderError(MellowNativeRenderUnsupported, @"getBytes requires a nonnull destination, full 2D region, mipmap 0 and a non-overflowing RGBA row stride");
        } else {
            NSError *e = nil; NSData *snapshot = [self snapshot:&e];
            if (snapshot) {
                for (NSUInteger y = 0; y < self.height; ++y)
                    std::memcpy(static_cast<unsigned char *>(destination) + y * row,
                                static_cast<const unsigned char *>(snapshot.bytes) + y * stride, stride);
                return;
            }
            _lastError = e;
        }
        @throw [NSException exceptionWithName:NSInvalidArgumentException reason:_lastError.localizedDescription
                                    userInfo:@{@"error": _lastError}];
    }
}
@end

@implementation MellowNativeRenderLibrary
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    @synchronized (_owner) {
        _lastError = nil;
        try {
            std::string entry;
            if (!SourceBytes(name, entry, 128)) { _lastError = RenderError(MellowNativeRenderInvalidArgument, @"A bounded UTF-8 function name is required"); return nil; }
            MellowMTL::Error vertexError, fragmentError;
            auto vertex = _library->newFunction(entry, MellowRT::RenderShaderJit::Stage::Vertex, vertexError);
            auto fragment = _library->newFunction(entry, MellowRT::RenderShaderJit::Stage::Fragment, fragmentError);
            if (static_cast<bool>(vertex) == static_cast<bool>(fragment)) {
                _lastError = RenderError(MellowNativeRenderCompilation,
                                        vertex ? @"Ambiguous source stage" : RenderString(vertexError.message + "; " + fragmentError.message)); return nil;
            }
            MellowNativeRenderFunction *object = [MellowNativeRenderFunction new];
            object->_owner = _owner; object->_function = vertex ? std::move(vertex) : std::move(fragment);
            return (id<MTLFunction>)object;
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); return nil; }
    }
}
@end
@implementation MellowNativeRenderFunction
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (NSString *)name { return RenderString(_function->name()); }
- (MTLFunctionType)functionType {
    return _function->stage() == MellowRT::RenderShaderJit::Stage::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
}
@end
@implementation MellowNativeRenderPipeline
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
@end

@implementation MellowNativeRenderQueue
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (id<MTLCommandBuffer>)commandBuffer {
    @synchronized (_owner) {
        _lastError = nil;
        try {
            MellowNativeRenderCommand *object = [MellowNativeRenderCommand new];
            object->_owner = _owner; object->_queueObject = self; object->_work = _queue->commandBuffer();
            object->_targets = [NSMutableArray array];
            return (id<MTLCommandBuffer>)object;
        } catch (const std::exception &e) { _lastError = RenderError(MellowNativeRenderExecution, RenderString(e.what())); return nil; }
    }
}
@end

static bool AdmittedPass(MTLRenderPassDescriptor *d) {
    MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[0];
    if (a.loadAction != MTLLoadActionClear || a.storeAction != MTLStoreActionStore ||
        a.storeActionOptions != MTLStoreActionOptionNone || a.level || a.slice || a.depthPlane ||
        a.resolveTexture || a.resolveLevel || a.resolveSlice || a.resolveDepthPlane ||
        d.depthAttachment.texture || d.depthAttachment.resolveTexture ||
        d.stencilAttachment.texture || d.stencilAttachment.resolveTexture || d.visibilityResultBuffer ||
        d.renderTargetArrayLength != 1 || d.renderTargetWidth || d.renderTargetHeight ||
        d.defaultRasterSampleCount != 1 || d.tileWidth || d.tileHeight || d.imageblockSampleLength ||
        d.threadgroupMemoryLength || [d getSamplePositions:nullptr count:0] ||
        d.rasterizationRateMap) return false;
    for (NSUInteger i = 1; i < 8; ++i)
        if (d.colorAttachments[i].texture || d.colorAttachments[i].resolveTexture) return false;
    for (NSUInteger i = 0; i < 4; ++i)
        if (d.sampleBufferAttachments[i].sampleBuffer) return false;
    for (double value : {a.clearColor.red, a.clearColor.green, a.clearColor.blue, a.clearColor.alpha})
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) return false;
    return true;
}

@implementation MellowNativeRenderCommand
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (id<MTLCommandQueue>)commandQueue { return (id<MTLCommandQueue>)_queueObject; }
- (void)fail:(NSError *)error {
    _lastError = error;
    if (_completed) return; // A rejected later call cannot undo a completed GPU result.
    if (!_terminalError) _terminalError = error;
    _failed = YES;
    for (NSUInteger i = 0; i < _targets.count && i < _tickets.size(); ++i) {
        MellowNativeRenderTexture *texture = _targets[i];
        if (texture->_latestTicket == _tickets[i]) texture->_contentValid = NO;
    }
}
- (void)rejectSelector:(SEL)selector {
    [self fail:RenderError(MellowNativeRenderUnsupported, [@"Unsupported command selector: " stringByAppendingString:NSStringFromSelector(selector)])];
    [super rejectSelector:selector];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor {
    @synchronized (_owner) {
        if (_failed || _committed || _completed || _encoding) {
            [self fail:RenderError(MellowNativeRenderInvalidState, @"Command buffer cannot start another encoder")]; return nil;
        }
        if (![descriptor isKindOfClass:[MTLRenderPassDescriptor class]]) {
            [self fail:RenderError(MellowNativeRenderInvalidArgument, @"A native render pass descriptor is required")]; return nil;
        }
        MTLRenderPassDescriptor *d = [descriptor copy];
        if (!AdmittedPass(d)) { [self fail:RenderError(MellowNativeRenderUnsupported, @"Only one finite Clear/Store RGBA target without resolves, depth/stencil, counters, tiles or array rendering is admitted")]; return nil; }
        if (![(id)d.colorAttachments[0].texture isKindOfClass:[MellowNativeRenderTexture class]]) {
            [self fail:RenderError(MellowNativeRenderWrongDevice, @"Render target must be a Mellow native render texture")]; return nil;
        }
        MellowNativeRenderTexture *texture = (MellowNativeRenderTexture *)d.colorAttachments[0].texture;
        if (texture->_owner != _owner) { [self fail:RenderError(MellowNativeRenderWrongDevice, @"Render target belongs to another device")]; return nil; }
        if (_targets.count >= 16 || _owner->_nextTicket == std::numeric_limits<uint64_t>::max()) {
            [self fail:RenderError(MellowNativeRenderUnsupported, @"Render pass/ticket bound exceeded")]; return nil;
        }
        try {
            const MTLClearColor c = d.colorAttachments[0].clearColor;
            MellowMTL::Error e;
            auto encoder = _work->renderCommandEncoder({texture->_texture, {static_cast<float>(c.red), static_cast<float>(c.green), static_cast<float>(c.blue), static_cast<float>(c.alpha)}}, e);
            if (!encoder) { [self fail:RenderError(e)]; return nil; }
            const uint64_t ticket = ++_owner->_nextTicket;
            texture->_latestTicket = ticket; texture->_contentValid = NO;
            _tickets.push_back(ticket); [_targets addObject:texture];
            MellowNativeRenderEncoder *object = [MellowNativeRenderEncoder new];
            object->_owner = _owner; object->_command = self; object->_encoder = std::move(encoder);
            _encoding = YES;
            return (id<MTLRenderCommandEncoder>)object;
        } catch (const std::exception &e) { [self fail:RenderError(MellowNativeRenderExecution, RenderString(e.what()))]; return nil; }
    }
}
- (void)commit {
    @synchronized (_owner) {
        if (_failed) return;
        if (_committed || _completed || _encoding || !_targets.count) {
            [self fail:RenderError(MellowNativeRenderInvalidState, @"Commit requires ended, nonempty encoders and can be called once")]; return;
        }
        if (_tickets.size() != _targets.count) {
            [self fail:RenderError(MellowNativeRenderInvalidState, @"Render target/ticket count mismatch")]; return;
        }
        // Only each target's final pass in this command owns its newest ticket.
        // Reject stale work before it can overwrite a newer command's GPU result.
        for (NSUInteger i = 0; i < _targets.count; ++i) {
            MellowNativeRenderTexture *texture = _targets[i];
            BOOL laterPass = NO;
            for (NSUInteger j = i + 1; j < _targets.count; ++j)
                if (_targets[j] == texture) { laterPass = YES; break; }
            if (!laterPass && texture->_latestTicket != _tickets[i]) {
                [self fail:RenderError(MellowNativeRenderInvalidState, @"Render target was superseded before commit")]; return;
            }
        }
        _committed = YES;
        try {
            MellowMTL::Error e;
            if (!_work->commit(e)) { [self fail:RenderError(e)]; return; }
            const auto &frames = _work->executions();
            if (_work->status() != MellowMTL::CommandStatus::Completed ||
                frames.size() != _targets.count || _tickets.size() != _targets.count) {
                [self fail:RenderError(MellowNativeRenderExecution, @"Render command/frame count mismatch")]; return;
            }
            for (const auto &frame : frames) {
                if (!frame.renderSubmitted || !frame.fenceSignaled || !frame.readbackCompleted ||
                    !frame.resourcesReleased || !frame.epoch || !frame.sequence || frame.swapCompleted) {
                    [self fail:RenderError(MellowNativeRenderExecution, @"CGL submission/fence/readback completion is incomplete")]; return;
                }
            }
            for (NSUInteger i = 0; i < _targets.count; ++i) {
                MellowNativeRenderTexture *texture = _targets[i];
                if (texture->_latestTicket == _tickets[i]) {
                    if (texture->_texture->contentSequence() != frames[i].sequence) {
                        [self fail:RenderError(MellowNativeRenderExecution, @"Texture completion sequence mismatch")]; return;
                    }
                    texture->_completedSequence = frames[i].sequence; texture->_contentValid = YES;
                }
            }
            _completed = YES; _lastError = nil;
        } catch (const std::exception &e) { [self fail:RenderError(MellowNativeRenderExecution, RenderString(e.what()))]; }
    }
}
- (void)waitUntilCompleted {
    @synchronized (_owner) {
        if (_failed) return;
        try {
            MellowMTL::Error e;
            if (!_completed || !_work->waitUntilCompleted(e))
                [self fail:e.code == MellowMTL::ErrorCode::None ? RenderError(MellowNativeRenderInvalidState, @"Command was not completed") : RenderError(e)];
        } catch (const std::exception &e) { [self fail:RenderError(MellowNativeRenderExecution, RenderString(e.what()))]; }
    }
}
- (MTLCommandBufferStatus)status {
    @synchronized (_owner) {
        if (_completed) return MTLCommandBufferStatusCompleted;
        if (_failed) return MTLCommandBufferStatusError;
        return _committed ? MTLCommandBufferStatusCommitted : MTLCommandBufferStatusNotEnqueued;
    }
}
- (NSError *)error { @synchronized (_owner) { return _terminalError; } }
@end

@implementation MellowNativeRenderEncoder
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (BOOL)active {
    if (!_ended && !_command->_failed && !_command->_committed && _command->_encoding) return YES;
    [self fail:RenderError(MellowNativeRenderInvalidState, @"Render encoder is not active")]; return NO;
}
- (void)fail:(NSError *)error { _lastError = error; [_command fail:error]; }
- (void)rejectSelector:(SEL)selector {
    [self fail:RenderError(MellowNativeRenderUnsupported, [@"Unsupported encoder selector: " stringByAppendingString:NSStringFromSelector(selector)])];
    [super rejectSelector:selector];
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline {
    @synchronized (_owner) {
        if (![self active]) return;
        if (![(id)pipeline isKindOfClass:[MellowNativeRenderPipeline class]]) {
            [self fail:RenderError(MellowNativeRenderWrongDevice, @"Pipeline must come from this native render adapter")]; return;
        }
        MellowNativeRenderPipeline *p = (MellowNativeRenderPipeline *)pipeline;
        if (p->_owner != _owner) { [self fail:RenderError(MellowNativeRenderWrongDevice, @"Pipeline belongs to another device")]; return; }
        MellowMTL::Error e;
        if (!_encoder->setRenderPipeline(p->_pipeline, e)) [self fail:RenderError(e)];
    }
}
- (void)bindBytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index vertex:(BOOL)vertex {
    if (![self active]) return;
    if (!bytes || length != 16 || index) {
        [self fail:RenderError(MellowNativeRenderUnsupported, @"Each stage requires exactly 16 copied float4 bytes at index 0")]; return;
    }
    std::array<float, 4> values;
    static_assert(sizeof(values) == 16, "Render float4 layout must be 16 bytes");
    std::memcpy(values.data(), bytes, 16);
    for (float value : values) if (!std::isfinite(value)) {
        [self fail:RenderError(MellowNativeRenderInvalidArgument, @"Stage parameters must be finite")]; return;
    }
    std::memcpy((vertex ? _vertexBytes : _fragmentBytes).data(), bytes, 16);
    if (vertex) _vertexBound = YES; else _fragmentBound = YES;
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    @synchronized (_owner) { [self bindBytes:bytes length:length index:index vertex:YES]; }
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    @synchronized (_owner) { [self bindBytes:bytes length:length index:index vertex:NO]; }
}
- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
    @synchronized (_owner) {
        if (![self active]) return;
        if (_drew || primitive != MTLPrimitiveTypeTriangle || start || count != 3) {
            [self fail:RenderError(MellowNativeRenderUnsupported, @"Only one Triangle draw with vertexStart 0 and vertexCount 3 per pass is admitted")]; return;
        }
        if (!_vertexBound || !_fragmentBound || _vertexBytes != _fragmentBytes) {
            [self fail:RenderError(MellowNativeRenderInvalidArgument, @"Vertex and fragment buffer(0) must be bound separately and match byte-for-byte")]; return;
        }
        std::array<float, 4> params;
        std::memcpy(params.data(), _vertexBytes.data(), 16);
        try {
            MellowMTL::Error e;
            if (!_encoder->setSharedParameters(params, e) ||
                !_encoder->drawPrimitives(MellowMTL::PrimitiveType::Triangle, 0, 3, e)) {
                [self fail:RenderError(e)]; return;
            }
            _drew = YES;
        } catch (const std::exception &e) { [self fail:RenderError(MellowNativeRenderExecution, RenderString(e.what()))]; }
    }
}
- (void)endEncoding {
    @synchronized (_owner) {
        if (![self active]) return;
        MellowMTL::Error e;
        if (!_encoder->endEncoding(e)) { [self fail:RenderError(e)]; return; }
        _ended = YES; _command->_encoding = NO;
    }
}
- (void)dealloc {
    @synchronized (_owner) {
        if (_command && !_ended) {
            [_command fail:RenderError(MellowNativeRenderInvalidState, @"Render encoder was abandoned")];
            _command->_encoding = NO;
        }
        _encoder.reset(); // Invokes the production encoder's abandonment contract.
    }
}
@end

id<MTLDevice> MellowCreateNativeRenderDevice(NSError **error) {
    StoreError(error, nil);
    try {
        MellowMTL::Error e; auto render = MellowMTL::RenderDevice::createOpenGL(e, false);
        if (!render) { StoreError(error, RenderError(e)); return nil; }
        MellowNativeRenderDevice *device = [MellowNativeRenderDevice new]; device->_render = std::move(render);
        return (id<MTLDevice>)device;
    } catch (const std::exception &e) { StoreError(error, RenderError(MellowNativeRenderExecution, RenderString(e.what()))); return nil; }
}
NSData *MellowCopyNativeRenderTextureRGBA8(id<MTLTexture> texture, NSUInteger *width,
                                         NSUInteger *height, uint64_t *sequence, NSError **error) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (sequence) *sequence = 0;
    StoreError(error, nil);
    if (![(id)texture isKindOfClass:[MellowNativeRenderTexture class]]) {
        StoreError(error, RenderError(MellowNativeRenderWrongDevice, @"Snapshot requires a Mellow native render texture")); return nil;
    }
    MellowNativeRenderTexture *t = (MellowNativeRenderTexture *)texture;
    @synchronized (t->_owner) {
        NSData *snapshot = [t snapshot:error];
        if (!snapshot) return nil;
        if (width) *width = t.width;
        if (height) *height = t.height;
        if (sequence) *sequence = t->_completedSequence;
        return snapshot;
    }
}
NSError *MellowNativeRenderLastError(id object) {
    if (![object isKindOfClass:[MellowNativeRenderObject class]])
        return RenderError(MellowNativeRenderWrongDevice, @"Object is outside the native render adapter");
    MellowNativeRenderObject *o = (MellowNativeRenderObject *)object;
    @synchronized (o->_owner ?: o) { return o->_lastError; }
}
NSDictionary<NSString *, id> *MellowNativeRenderDeviceEvidence(id<MTLDevice> device, NSError **error) {
    StoreError(error, nil);
    if (![(id)device isKindOfClass:[MellowNativeRenderDevice class]]) {
        StoreError(error, RenderError(MellowNativeRenderWrongDevice, @"Evidence requires a Mellow native render device")); return nil;
    }
    MellowNativeRenderDevice *d = (MellowNativeRenderDevice *)device;
    @synchronized (d) {
        const auto info = d->_render->hardware();
        return @{@"provider": @"CGL", @"vendor": RenderString(info.vendor), @"renderer": RenderString(info.renderer),
                 @"version": RenderString(info.version), @"accelerated_pixel_format": @(info.acceleratedPixelFormat),
                 @"core_profile": @(info.coreProfile), @"software_renderer_rejected": @(info.softwareRendererRejected),
                 @"pipeline_build_count": @(d->_render->pipelineBuildCount()),
                 @"partial_native_selectors": @YES, @"complete_metal_protocols_verified": @NO,
                 @"physical_pci_identity_verified": @NO, @"native_unsupported_gpu_driver_verified": @NO,
                 @"system_mtl_device_registered": @NO, @"windowserver_acceleration_verified": @NO,
                 @"display_scanout_verified": @NO};
    }
}
