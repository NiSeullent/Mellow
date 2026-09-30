// SPDX-License-Identifier: MIT
#import "MellowAppleRenderMetal.h"
#import <dispatch/dispatch.h>
#include "../../Runtime/RenderObjects.hpp"
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#if !__has_feature(objc_arc)
#error "MellowAppleRenderMetal requires Objective-C ARC"
#endif

namespace {
char renderWorkerKey;
NSString *renderText(const std::string &value) {
    NSString *result=[[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    return result ?: @"Invalid UTF-8 provider diagnostic";
}
NSError *renderFailure(MellowMTL::ErrorCode code, NSString *message) {
    return [NSError errorWithDomain:MellowAppleMetalErrorDomain code:NSInteger(code)
        userInfo:@{NSLocalizedDescriptionKey:message}];
}
NSError *renderError(const MellowMTL::Error &error) { return renderFailure(error.code,renderText(error.message)); }
void renderAssign(NSError **out, NSError *error) { if (out) *out=error; }
[[noreturn]] void renderUnsupported(NSString *message) {
    @throw [NSException exceptionWithName:MellowAppleMetalUnsupportedException reason:message userInfo:nil];
}
std::string renderUTF8(NSString *text) {
    NSData *bytes=[text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    return bytes ? std::string(static_cast<const char *>(bytes.bytes),bytes.length) : std::string();
}
// Entry discovery only; both entries and the complete source are validated by
// the production RenderShaderJit parser before returning a usable library.
bool renderEntries(const std::string &source, std::string &vertex, std::string &fragment) {
    if (source.empty() || source.size()>MellowRT::RenderShaderJit::MaxSourceBytes) return false;
    std::vector<std::string> tokens;
    for (size_t i=0;i<source.size();) {
        if (source.compare(i,2,"//")==0) { while (i<source.size() && source[i]!='\n') ++i; continue; }
        if (source.compare(i,2,"/*")==0) { size_t end=source.find("*/",i+2); if (end==std::string::npos) return false; i=end+2; continue; }
        const char c=source[i];
        if ((c>='A' && c<='Z') || (c>='a' && c<='z') || c=='_') {
            size_t first=i++;
            while (i<source.size()) {
                char n=source[i]; if (!((n>='A' && n<='Z') || (n>='a' && n<='z') || (n>='0' && n<='9') || n=='_')) break;
                ++i;
            }
            tokens.push_back(source.substr(first,i-first));
        } else { if (c!=' ' && c!='\r' && c!='\n' && c!='\t') tokens.emplace_back(1,c); ++i; }
        if (tokens.size()>8192) return false;
    }
    for (size_t i=0;i+2<tokens.size();++i) {
        if (tokens[i]=="vertex" || tokens[i]=="fragment") {
            if (tokens[i+1]!="float4") return false;
            auto &entry=tokens[i]=="vertex" ? vertex : fragment;
            if (!entry.empty()) return false;
            entry=tokens[i+2];
        }
    }
    return !vertex.empty() && !fragment.empty();
}
}

@class MRADevice, MRATexture, MRALibrary, MRAFunction, MRAPipeline, MRAQueue, MRACommand, MRAEncoder;
@interface MRAObject : NSObject
@property(nonatomic,copy,nullable) NSString *label;
@end
@implementation MRAObject
- (void)doesNotRecognizeSelector:(SEL)selector {
    renderUnsupported([NSString stringWithFormat:@"%@ does not implement %@ in the Mellow render subset",
        NSStringFromClass(self.class),NSStringFromSelector(selector)]);
}
@end
@interface MRADevice : MRAObject {
@public
    std::shared_ptr<MellowMTL::RenderDevice> core;
    dispatch_queue_t worker;
}
@end
@interface MRATexture : MRAObject {
@public
    MRADevice *owner;
    std::shared_ptr<MellowMTL::RenderTexture> core;
}
@end
@interface MRALibrary : MRAObject {
@public
    MRADevice *owner;
    std::shared_ptr<MellowMTL::RenderLibrary> core;
    std::shared_ptr<MellowMTL::RenderFunction> vertex,fragment;
}
@end
@interface MRAFunction : MRAObject {
@public
    MRADevice *owner;
    MRALibrary *library;
    std::shared_ptr<MellowMTL::RenderFunction> core;
}
@end
@interface MRAPipeline : MRAObject {
@public
    MRADevice *owner;
    MRAFunction *vertex,*fragment;
    std::shared_ptr<MellowMTL::RenderPipeline> core;
}
@end
@interface MRAQueue : MRAObject {
@public
    MRADevice *owner;
    std::shared_ptr<MellowMTL::RenderCommandQueue> core;
}
@end
@interface MRACommand : MRAObject {
@public
    MRAQueue *owner;
    std::shared_ptr<MellowMTL::RenderCommandBuffer> core;
@private
    std::mutex stateMutex;
    std::condition_variable completed;
    MTLCommandBufferStatus commandStatus;
    NSError *commandError;
    NSMutableArray *handlers;
    NSMutableArray<MRATexture *> *retainedTextures;
    bool committed,encoderActive,done;
}
- (instancetype)initWithQueue:(MRAQueue *)queue;
- (BOOL)canEncode;
- (BOOL)canEndEncoding;
- (void)encoderEnded;
- (void)recordFailure:(NSError *)error;
- (NSArray<NSDictionary<NSString *,id> *> *)copyExecutionEvidence;
@end
@interface MRAEncoder : MRAObject {
@public
    MRACommand *owner;
    std::shared_ptr<MellowMTL::RenderEncoder> core;
@private
    bool ended,hasVertexParameters,hasFragmentParameters;
    std::array<float,4> vertexParameters,fragmentParameters;
}
- (void)requireActive;
@end

@implementation MRADevice
- (NSString *)name { return [@"Mellow render / " stringByAppendingString:renderText(core->hardware().renderer)]; }
- (BOOL)supportsFamily:(MTLGPUFamily)family { (void)family; return NO; }
- (BOOL)supportsFeatureSet:(MTLFeatureSet)feature { (void)feature; return NO; }
- (id<MTLCommandQueue>)newCommandQueue {
    MRAQueue *queue=[MRAQueue new]; queue->owner=self; queue->core=core->newCommandQueue(); return (id<MTLCommandQueue>)queue;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    if (!descriptor) renderUnsupported(@"A render texture descriptor is required");
    const MTLTextureSwizzleChannels swizzle=descriptor.swizzle;
    if (!descriptor || descriptor.textureType!=MTLTextureType2D || descriptor.pixelFormat!=MTLPixelFormatBGRA8Unorm ||
        descriptor.depth!=1 || descriptor.mipmapLevelCount!=1 || descriptor.arrayLength!=1 || descriptor.sampleCount!=1 ||
        !descriptor.width || !descriptor.height || descriptor.width>MellowRT::OpenGLProvider::MaxDimension ||
        descriptor.height>MellowRT::OpenGLProvider::MaxDimension || descriptor.storageMode!=MTLStorageModeShared ||
        descriptor.cpuCacheMode!=MTLCPUCacheModeDefaultCache || descriptor.resourceOptions!=MTLResourceStorageModeShared ||
        descriptor.usage!=MTLTextureUsageRenderTarget || swizzle.red!=MTLTextureSwizzleRed || swizzle.green!=MTLTextureSwizzleGreen ||
        swizzle.blue!=MTLTextureSwizzleBlue || swizzle.alpha!=MTLTextureSwizzleAlpha)
        renderUnsupported(@"Render textures require shared default options, render-target-only usage, BGRA8Unorm 2D, one mip/slice/sample and dimensions 1..2048");
    try {
        MellowMTL::Error error; auto texture=core->newIOSurfaceTexture(uint32_t(descriptor.width),uint32_t(descriptor.height),error);
        if (!texture) renderUnsupported(renderError(error).localizedDescription);
        MRATexture *wrapper=[MRATexture new]; wrapper->owner=self; wrapper->core=std::move(texture);
        return (id<MTLTexture>)wrapper;
    } catch (const std::exception &error) { renderUnsupported(renderText(error.what())); }
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)out {
    renderAssign(out,nil);
    if (!source || options) { renderAssign(out,renderFailure(MellowMTL::ErrorCode::Unsupported,@"Render source requires nil compilation options")); return nil; }
    try {
        const std::string text=renderUTF8(source); std::string v,f;
        if (!renderEntries(text,v,f)) { renderAssign(out,renderFailure(MellowMTL::ErrorCode::Compilation,@"Expected exactly one bounded vertex float4 and fragment float4 entry")); return nil; }
        MellowMTL::Error error; auto library=core->newLibraryWithSource(text,error);
        if (!library) { renderAssign(out,renderError(error)); return nil; }
        auto vertex=library->newFunction(v,MellowRT::RenderShaderJit::Stage::Vertex,error);
        if (!vertex) { renderAssign(out,renderError(error)); return nil; }
        auto fragment=library->newFunction(f,MellowRT::RenderShaderJit::Stage::Fragment,error);
        if (!fragment) { renderAssign(out,renderError(error)); return nil; }
        MRALibrary *wrapper=[MRALibrary new]; wrapper->owner=self; wrapper->core=std::move(library);
        wrapper->vertex=std::move(vertex); wrapper->fragment=std::move(fragment); return (id<MTLLibrary>)wrapper;
    } catch (const std::exception &error) { renderAssign(out,renderFailure(MellowMTL::ErrorCode::Compilation,renderText(error.what()))); return nil; }
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)out {
    renderAssign(out,nil);
    if (!descriptor || ![descriptor.vertexFunction isKindOfClass:MRAFunction.class] ||
        ![descriptor.fragmentFunction isKindOfClass:MRAFunction.class]) {
        renderAssign(out,renderFailure(MellowMTL::ErrorCode::WrongDevice,@"Pipeline functions must belong to the Mellow render adapter")); return nil;
    }
    for (NSUInteger i=1;i<8;++i) if (descriptor.colorAttachments[i].pixelFormat!=MTLPixelFormatInvalid) {
        renderAssign(out,renderFailure(MellowMTL::ErrorCode::Unsupported,@"Only color attachment zero is supported")); return nil;
    }
    auto color=descriptor.colorAttachments[0];
    if (descriptor.vertexDescriptor || descriptor.sampleCount!=1 || descriptor.alphaToCoverageEnabled ||
        descriptor.alphaToOneEnabled || !descriptor.rasterizationEnabled ||
        descriptor.depthAttachmentPixelFormat!=MTLPixelFormatInvalid || descriptor.stencilAttachmentPixelFormat!=MTLPixelFormatInvalid ||
        color.pixelFormat!=MTLPixelFormatBGRA8Unorm || color.blendingEnabled || color.writeMask!=MTLColorWriteMaskAll ||
        descriptor.inputPrimitiveTopology!=MTLPrimitiveTopologyClassUnspecified || descriptor.supportIndirectCommandBuffers ||
        descriptor.maxVertexAmplificationCount!=1) {
        renderAssign(out,renderFailure(MellowMTL::ErrorCode::Unsupported,@"Pipeline requires BGRA8 color0, no blending/depth/stencil/vertex layout/tessellation, sample1 and default topology")); return nil;
    }
    try {
        MRAFunction *v=(MRAFunction *)descriptor.vertexFunction,*f=(MRAFunction *)descriptor.fragmentFunction;
        MellowMTL::Error error; auto pipeline=core->newRenderPipeline(v->core,f->core,error);
        if (!pipeline) { renderAssign(out,renderError(error)); return nil; }
        MRAPipeline *wrapper=[MRAPipeline new]; wrapper->owner=self; wrapper->vertex=v; wrapper->fragment=f; wrapper->core=std::move(pipeline);
        return (id<MTLRenderPipelineState>)wrapper;
    } catch (const std::exception &error) { renderAssign(out,renderFailure(MellowMTL::ErrorCode::Compilation,renderText(error.what()))); return nil; }
}
@end
@implementation MRATexture
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (NSUInteger)width { return core->width(); }
- (NSUInteger)height { return core->height(); }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return MTLPixelFormatBGRA8Unorm; }
- (MTLTextureUsage)usage { return MTLTextureUsageRenderTarget; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared; }
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
    if (!bytes || level || region.origin.x || region.origin.y || region.origin.z ||
        region.size.width!=self.width || region.size.height!=self.height || region.size.depth!=1 ||
        row<self.width*4 || row>std::numeric_limits<size_t>::max()/self.height)
        renderUnsupported(@"Only a complete mip0/slice0 BGRA8 readback region with a bounded row stride is supported");
    const size_t lastWrittenBytes=(self.height-1)*row+self.width*4;
    if (reinterpret_cast<uintptr_t>(bytes)>std::numeric_limits<uintptr_t>::max()-lastWrittenBytes)
        renderUnsupported(@"Texture readback destination address interval overflows uintptr_t");
    MellowMTL::Error error; const auto rgba=core->read(error);
    if (rgba.size()!=self.width*self.height*4) renderUnsupported(renderError(error).localizedDescription);
    auto *destination=static_cast<uint8_t *>(bytes);
    for (size_t y=0;y<self.height;++y) for (size_t x=0;x<self.width;++x) {
        const auto *source=rgba.data()+(y*self.width+x)*4;
        auto *pixel=destination+y*row+x*4;
        pixel[0]=source[2]; pixel[1]=source[1]; pixel[2]=source[0]; pixel[3]=source[3];
    }
}
@end
@implementation MRALibrary
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (NSArray<NSString *> *)functionNames { return @[renderText(vertex->name()),renderText(fragment->name())]; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    const auto entry=renderUTF8(name); auto function=entry==vertex->name() ? vertex : entry==fragment->name() ? fragment : nullptr;
    if (!function) return nil;
    MRAFunction *wrapper=[MRAFunction new]; wrapper->owner=owner; wrapper->library=self; wrapper->core=std::move(function);
    return (id<MTLFunction>)wrapper;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)out {
    (void)name; (void)values; renderAssign(out,renderFailure(MellowMTL::ErrorCode::Unsupported,@"Render function constants are unsupported")); return nil;
}
@end
@implementation MRAFunction
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (NSString *)name { return renderText(core->name()); }
- (MTLFunctionType)functionType { return core->stage()==MellowRT::RenderShaderJit::Stage::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment; }
@end
@implementation MRAPipeline
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
@end
@implementation MRAQueue
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)[[MRACommand alloc] initWithQueue:self]; }
@end
@implementation MRACommand
- (instancetype)initWithQueue:(MRAQueue *)queue {
    if ((self=[super init])) { owner=queue; core=queue->core->commandBuffer(); commandStatus=MTLCommandBufferStatusNotEnqueued; handlers=[NSMutableArray new]; retainedTextures=[NSMutableArray new]; }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)owner->owner; }
- (id<MTLCommandQueue>)commandQueue { return (id<MTLCommandQueue>)owner; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferStatus)status { std::lock_guard<std::mutex> lock(stateMutex); return commandStatus; }
- (NSError *)error { std::lock_guard<std::mutex> lock(stateMutex); return commandError; }
- (BOOL)canEncode { std::lock_guard<std::mutex> lock(stateMutex); return !committed && !commandError; }
- (BOOL)canEndEncoding { std::lock_guard<std::mutex> lock(stateMutex); return !committed; }
- (void)encoderEnded { std::lock_guard<std::mutex> lock(stateMutex); encoderActive=false; }
- (void)recordFailure:(NSError *)error { std::lock_guard<std::mutex> lock(stateMutex); if (!committed && !commandError) commandError=error; }
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)pass {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (committed || commandError || encoderActive) {
        if (!committed && !commandError) commandError=renderFailure(MellowMTL::ErrorCode::InvalidState,@"Only one active render encoder is permitted"); return nil;
    }
    if (!pass) { commandError=renderFailure(MellowMTL::ErrorCode::InvalidArgument,@"A render pass is required"); return nil; }
    for (NSUInteger i=1;i<8;++i) if (pass.colorAttachments[i].texture || pass.colorAttachments[i].resolveTexture) {
        commandError=renderFailure(MellowMTL::ErrorCode::Unsupported,@"Only color attachment zero is supported"); return nil;
    }
    // Metal exposes indexed attachment access without a public count query.
    // Inspect the four slots used by Dawn's Metal backend (48f5ceee), matching
    // Runtime/NativeMetalRender.mm. There is no MTLMaxRenderPassSampleBuffers
    // declaration in the macOS 15.5 SDK used for our native build.
    constexpr NSUInteger counterAttachmentCount = 4;
    for (NSUInteger i=0;i<counterAttachmentCount;++i) if (pass.sampleBufferAttachments[i].sampleBuffer) {
        commandError=renderFailure(MellowMTL::ErrorCode::Unsupported,@"Render counter sample buffers are unsupported"); return nil;
    }
    auto color=pass.colorAttachments[0];
    if (![color.texture isKindOfClass:MRATexture.class] || color.resolveTexture || color.level || color.slice || color.depthPlane ||
        color.loadAction!=MTLLoadActionClear || color.storeAction!=MTLStoreActionStore || color.storeActionOptions ||
        pass.depthAttachment.texture || pass.stencilAttachment.texture || pass.visibilityResultBuffer || pass.renderTargetArrayLength>1 ||
        pass.rasterizationRateMap || [pass getSamplePositions:nullptr count:0]!=0 ||
        (pass.renderTargetWidth && pass.renderTargetWidth!=color.texture.width) ||
        (pass.renderTargetHeight && pass.renderTargetHeight!=color.texture.height)) {
        commandError=renderFailure(MellowMTL::ErrorCode::Unsupported,@"Pass requires a single mip0/slice0 BGRA8 texture, clear/store, no resolve/depth/stencil/query/layered rendering"); return nil;
    }
    const MTLClearColor clear=color.clearColor;
    for (const double component : {clear.red,clear.green,clear.blue,clear.alpha}) if (!std::isfinite(component) || component<0.0 || component>1.0) {
        commandError=renderFailure(MellowMTL::ErrorCode::InvalidArgument,@"Clear color components must be finite within 0..1"); return nil;
    }
    MellowMTL::RenderPassDescriptor descriptor;
    descriptor.colorTexture=((MRATexture *)color.texture)->core;
    descriptor.clearColor={float(clear.red),float(clear.green),float(clear.blue),float(clear.alpha)};
    MellowMTL::Error error; auto encoder=core->renderCommandEncoder(descriptor,error);
    if (!encoder) { commandError=renderError(error); return nil; }
    MRATexture *texture=(MRATexture *)color.texture;
    if (![retainedTextures containsObject:texture]) [retainedTextures addObject:texture];
    MRAEncoder *wrapper=[MRAEncoder new]; wrapper->owner=self; wrapper->core=std::move(encoder);
    encoderActive=true; return (id<MTLRenderCommandEncoder>)wrapper;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder { [self recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,@"Render and OpenCL compute devices are separate")]; return nil; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoder { [self recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,@"Render blit is unsupported")]; return nil; }
- (void)doesNotRecognizeSelector:(SEL)selector { [self recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,NSStringFromSelector(selector))]; [super doesNotRecognizeSelector:selector]; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    std::lock_guard<std::mutex> lock(stateMutex); if (!handler || committed) renderUnsupported(@"Register a nonnull completion handler before commit"); [handlers addObject:[handler copy]];
}
- (void)commit {
    NSArray *callbacks; NSError *encodingError;
    {
        std::lock_guard<std::mutex> lock(stateMutex); if (committed) renderUnsupported(@"Render commands may be committed only once");
        committed=true; commandStatus=MTLCommandBufferStatusCommitted;
        if (encoderActive && !commandError) commandError=renderFailure(MellowMTL::ErrorCode::InvalidState,@"End the render encoder before commit");
        encodingError=commandError; callbacks=[handlers copy]; [handlers removeAllObjects];
    }
    // The command owns each actual C++ texture and pipeline through driver
    // completion; this block retains its queue/device and copied callbacks.
    dispatch_async(owner->owner->worker,^{ @autoreleasepool {
        NSError *executionError=encodingError;
        @try { if (!executionError) {
            try { MellowMTL::Error error; if (!self->core->commit(error)) executionError=renderError(error); }
            catch (const std::exception &error) { executionError=renderFailure(MellowMTL::ErrorCode::Execution,renderText(error.what())); }
            catch (...) { executionError=renderFailure(MellowMTL::ErrorCode::Execution,@"Render worker raised an unknown C++ exception"); }
        } } @catch (NSException *exception) { executionError=renderFailure(MellowMTL::ErrorCode::Execution,exception.reason ?: @"Render worker exception"); }
        { std::lock_guard<std::mutex> lock(self->stateMutex); self->commandError=executionError; self->commandStatus=executionError ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted; }
        @try {
            for (MTLCommandBufferHandler callback in callbacks) {
                @try {
                    try { callback((id<MTLCommandBuffer>)self); }
                    catch (const std::exception &error) { NSLog(@"Mellow render callback raised %s",error.what()); }
                    catch (...) { NSLog(@"Mellow render callback raised an unknown C++ exception"); }
                } @catch (NSException *exception) { NSLog(@"Mellow render callback raised %@",exception); }
            }
        } @finally { { std::lock_guard<std::mutex> lock(self->stateMutex); self->done=true; } self->completed.notify_all(); }
    }});
}
- (void)waitUntilCompleted {
    std::unique_lock<std::mutex> lock(stateMutex); if (!committed) renderUnsupported(@"Commit before waiting for render completion");
    if (!done && dispatch_get_specific(&renderWorkerKey)==(__bridge void *)owner->owner)
        renderUnsupported(@"A device completion handler cannot wait for unfinished render work on the same device worker");
    completed.wait(lock,[thisSelf=self] { return thisSelf->done; });
}
- (NSArray<NSDictionary<NSString *,id> *> *)copyExecutionEvidence {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (commandStatus!=MTLCommandBufferStatusCompleted && commandStatus!=MTLCommandBufferStatusError) return nil;
    NSMutableArray *frames=[NSMutableArray new];
    for (const auto &frame : core->executions()) [frames addObject:@{
        @"width":@(frame.width), @"height":@(frame.height), @"epoch":@(frame.epoch), @"sequence":@(frame.sequence),
        @"renderSubmitted":@(frame.renderSubmitted), @"fenceSignaled":@(frame.fenceSignaled),
        @"readbackCompleted":@(frame.readbackCompleted), @"resourcesReleased":@(frame.resourcesReleased),
        @"ioSurfaceWritten":@(frame.ioSurfaceWritten), @"ioSurfaceID":@(frame.ioSurfaceID),
        @"swapCompleted":@(frame.swapCompleted), @"displayScanoutVerified":@(frame.displayScanoutVerified),
        @"error":renderText(frame.error)}];
    return [frames copy];
}
@end
@implementation MRAEncoder
- (id<MTLDevice>)device { return (id<MTLDevice>)owner->owner->owner; }
- (void)dealloc { if (!ended) { [owner recordFailure:renderFailure(MellowMTL::ErrorCode::InvalidState,@"Render encoder released before endEncoding")]; [owner encoderEnded]; } }
- (void)requireActive {
    if (ended || ![owner canEncode]) {
        [owner recordFailure:renderFailure(MellowMTL::ErrorCode::InvalidState,@"Render encoder is no longer active")];
        renderUnsupported(@"Render encoder is no longer active");
    }
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline {
    [self requireActive];
    if (![pipeline isKindOfClass:MRAPipeline.class]) { [owner recordFailure:renderFailure(MellowMTL::ErrorCode::WrongDevice,@"Expected a Mellow render pipeline")]; return; }
    MellowMTL::Error error; if (!core->setRenderPipeline(((MRAPipeline *)pipeline)->core,error)) [owner recordFailure:renderError(error)];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [self requireActive];
    if (!bytes || length!=sizeof(vertexParameters) || index) { [owner recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,@"Vertex bytes require float4 at buffer zero")]; return; }
    std::memcpy(vertexParameters.data(),bytes,length); hasVertexParameters=true;
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [self requireActive];
    if (!bytes || length!=sizeof(fragmentParameters) || index) { [owner recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,@"Fragment bytes require float4 at buffer zero")]; return; }
    std::memcpy(fragmentParameters.data(),bytes,length); hasFragmentParameters=true;
}
- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)first vertexCount:(NSUInteger)count {
    [self requireActive];
    if (!hasVertexParameters || !hasFragmentParameters || vertexParameters!=fragmentParameters ||
        primitive!=MTLPrimitiveTypeTriangle || first || count!=3) {
        [owner recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,@"Render draw requires matching float4 vertex/fragment buffer0 and triangle vertexStart0 vertexCount3")]; return;
    }
    MellowMTL::Error error;
    if (!core->setSharedParameters(vertexParameters,error) || !core->drawPrimitives(MellowMTL::PrimitiveType::Triangle,0,3,error)) [owner recordFailure:renderError(error)];
}
- (void)endEncoding {
    if (![owner canEndEncoding]) renderUnsupported(@"End the render encoder before committing");
    MellowMTL::Error error;
    if (!core->endEncoding(error)) [owner recordFailure:renderError(error)];
    else { ended=true; [owner encoderEnded]; }
}
- (void)doesNotRecognizeSelector:(SEL)selector { [owner recordFailure:renderFailure(MellowMTL::ErrorCode::Unsupported,NSStringFromSelector(selector))]; [super doesNotRecognizeSelector:selector]; }
@end

id<MTLDevice> MellowCreateRenderDevice(NSError **out) {
    renderAssign(out,nil);
    try {
        MellowMTL::Error error; auto device=MellowMTL::RenderDevice::createOpenGL(error);
        if (!device) {
            const bool unavailable=error.message=="No accelerated CGL 4.1 pixel format available" ||
                error.message=="Non-accelerated or fallback CGL renderer rejected" || error.message=="CGL OpenGL 4.1 core is required" ||
                error.message=="Software OpenGL renderer rejected";
            renderAssign(out,[NSError errorWithDomain:MellowAppleMetalErrorDomain code:NSInteger(error.code)
                userInfo:@{NSLocalizedDescriptionKey:renderText(error.message),
                    MellowAppleMetalInitializationStatusKey:unavailable ? @"unavailable" : @"failure",
                    MellowAppleMetalBootstrapSubmissionAttemptedKey:@NO}]); return nil;
        }
        MRADevice *wrapper=[MRADevice new]; wrapper->core=std::move(device);
        wrapper->worker=dispatch_queue_create("org.mellow.apple-metal.render",DISPATCH_QUEUE_SERIAL);
        if (!wrapper->worker) { renderAssign(out,renderFailure(MellowMTL::ErrorCode::Execution,@"Cannot create render device worker")); return nil; }
        dispatch_queue_set_specific(wrapper->worker,&renderWorkerKey,(__bridge void *)wrapper,nullptr);
        return (id<MTLDevice>)wrapper;
    } catch (const std::exception &error) { renderAssign(out,renderFailure(MellowMTL::ErrorCode::Execution,renderText(error.what()))); return nil; }
}
NSDictionary<NSString *,id> *MellowCopyRenderAdapterCapabilities(id<MTLDevice> device) {
    if (![device isKindOfClass:MRADevice.class]) return nil;
    const auto info=((MRADevice *)device)->core->hardware();
    return @{@"provider":@"existing-host-cgl-gpu", @"api":@"selector-compatible-bounded-render",
        @"vendor":renderText(info.vendor), @"renderer":renderText(info.renderer), @"version":renderText(info.version),
        @"shadingLanguageVersion":renderText(info.shadingLanguageVersion), @"pixelFormat":@(info.pixelFormat),
        @"acceleratedPixelFormat":@(info.acceleratedPixelFormat), @"softwareRendererRejected":@(info.softwareRendererRejected),
        @"coreProfile":@(info.coreProfile), @"physicalPCIIdentityVerified":@(info.physicalPciIdentityVerified),
        @"providerInitializationStatus":@"ready", @"bootstrapSubmissionAttempted":@NO,
        @"pipelineBuildCount":@(((MRADevice *)device)->core->pipelineBuildCount()),
        @"textureFormat":@"BGRA8Unorm", @"alphaMode":@"straight", @"surfaceOrigin":@"bottom-left", @"getBytesOrigin":@"top-left",
        @"maxTextureDimension":@(MellowRT::OpenGLProvider::MaxDimension), @"computeInterop":@NO,
        @"fullMetalProtocolConformance":@NO, @"metalFamilyAdvertised":@NO,
        @"systemMetalRegistered":@NO, @"windowServerIntegrated":@NO, @"displayScanoutVerified":@NO};
}
NSArray<NSDictionary<NSString *,id> *> *MellowCopyRenderCommandExecutionEvidence(id<MTLCommandBuffer> command) {
    if (![command isKindOfClass:MRACommand.class]) return nil; return [(MRACommand *)command copyExecutionEvidence];
}
IOSurfaceRef MellowGetCompletedRenderTextureIOSurface(id<MTLTexture> texture) {
    if (![texture isKindOfClass:MRATexture.class]) return nullptr; return ((MRATexture *)texture)->core->iosurface();
}
