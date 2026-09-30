// SPDX-License-Identifier: MIT
// Real Metal-selector render calls, explicit CGL provider and optional app view.
#import <AppKit/AppKit.h>
#import "MellowAppleRenderMetal.h"
#import "../WindowServer/SurfacePresenter.h"
#include "../WindowServer/RenderFixtureOracle.hpp"
#include "../../tests/render_fixture.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <limits>

namespace {
unsigned checks,negativeChecks;
void check(bool condition, const char *message) { ++checks; if (!condition) throw std::runtime_error(message); }
void reject(bool condition, const char *message) { ++negativeChecks; check(condition,message); }
bool unsupported(void (^operation)(void)) {
    @try { operation(); } @catch (NSException *exception) { return [exception.name isEqualToString:MellowAppleMetalUnsupportedException]; }
    return false;
}
void emit(NSDictionary *receipt) {
    NSData *json=[NSJSONSerialization dataWithJSONObject:receipt options:NSJSONWritingPrettyPrinted error:nil];
    if (!json) { std::fprintf(stderr,"Cannot serialize render receipt\n"); return; }
    std::fwrite(json.bytes,1,json.length,stdout); std::fputc('\n',stdout);
}
MTLRenderPassDescriptor *passFor(id<MTLTexture> texture) {
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=texture; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore; pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
    return pass;
}
void failed(id<MTLCommandBuffer> command, const char *message) {
    __block unsigned callbacks=0;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) { if (completed.status==MTLCommandBufferStatusError && completed.error) ++callbacks; }];
    [command commit]; [command waitUntilCompleted];
    reject(command.status==MTLCommandBufferStatusError && command.error!=nil && callbacks==1,message);
    NSArray *events=MellowCopyRenderCommandExecutionEvidence(command);
    reject(events!=nil && events.count==0,"Invalid render encoding submitted GPU work");
}
std::vector<uint8_t> copiedRGBA(id<MTLTexture> texture) {
    std::vector<uint8_t> bgra(texture.width*texture.height*4),rgba(bgra.size());
    [texture getBytes:bgra.data() bytesPerRow:texture.width*4 fromRegion:MTLRegionMake2D(0,0,texture.width,texture.height) mipmapLevel:0];
    for (size_t i=0;i<bgra.size();i+=4) { rgba[i]=bgra[i+2]; rgba[i+1]=bgra[i+1]; rgba[i+2]=bgra[i]; rgba[i+3]=bgra[i+3]; }
    return rgba;
}
void matchSurfaceImage(CGImageRef image, const std::vector<uint8_t> &rgba) {
    check(CGImageGetWidth(image)==64 && CGImageGetHeight(image)==48 && CGImageGetBytesPerRow(image)==64*4 &&
          CGImageGetAlphaInfo(image)==kCGImageAlphaFirst,"Presenter changed image layout/straight alpha");
    CFDataRef bytes=CGDataProviderCopyData(CGImageGetDataProvider(image)); check(bytes!=nullptr,"Cannot copy completed snapshot bytes");
    bool matches=CFDataGetLength(bytes)==static_cast<CFIndex>(rgba.size()); const UInt8 *bgra=CFDataGetBytePtr(bytes);
    for (size_t i=0;matches && i<rgba.size();i+=4)
        matches=bgra[i]==rgba[i+2] && bgra[i+1]==rgba[i+1] && bgra[i+2]==rgba[i] && bgra[i+3]==rgba[i+3];
    CFRelease(bytes); check(matches,"Borrowed IOSurface differs from independently checked texture bytes");
}
void pumpView(NSTimeInterval seconds=0.05) {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while (deadline.timeIntervalSinceNow>0) {
        NSEvent *event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:deadline inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event) [NSApp sendEvent:event]; [NSApp updateWindows];
    }
}
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSMutableDictionary *receipt=[@{@"status":@"NOT_RUN", @"acceptance":@"apple-opt-in-render",
            @"systemMetalRegistered":@NO, @"windowServerIntegrated":@NO, @"displayScanoutVerified":@NO,
            @"fullMetalProtocolConformance":@NO, @"physicalPCIIdentityVerified":@NO,
            @"providerInitializationStatus":@"unavailable", @"bootstrapSubmissionAttempted":@NO,
            @"renderSubmissionAttempted":@NO,
            @"independentReadbackVerified":@NO, @"gpuFenceVerified":@NO, @"gpuResourcesReleased":@NO,
            @"ioSurfaceWritten":@NO, @"framesVerified":@0, @"verifiedPixels":@0,
            @"completionHandlersVerified":@0, @"retainedResourceLifetimeVerified":@NO,
            @"applicationSnapshotQueued":@NO, @"snapshotAlphaMode":@"straight"} mutableCopy];
        NSWindow *window=nil; MellowSurfacePresenter *presenter=nil; int result=1;
        try { @try {
            bool present=false;
            if (argc==2 && !std::strcmp(argv[1],"--present")) present=true;
            else if (argc!=1) throw std::runtime_error("Usage: mellow-metal-render-acceptance [--present]");
            receipt[@"presentRequested"]=@(present);
            const NSOperatingSystemVersion version=NSProcessInfo.processInfo.operatingSystemVersion;
            receipt[@"osVersion"]=NSProcessInfo.processInfo.operatingSystemVersionString;
#if !defined(__x86_64__)
            receipt[@"reason"]=@"Acceptance requires a native x86_64 host"; emit(receipt); return 77;
#endif
            if (version.majorVersion!=15 && version.majorVersion!=26) {
                receipt[@"reason"]=@"Acceptance requires macOS 15 or 26"; emit(receipt); return 77;
            }
            NSError *error=nil; id<MTLDevice> device=MellowCreateRenderDevice(&error);
            if (!device) {
                NSString *outcome=error.userInfo[MellowAppleMetalInitializationStatusKey] ?: @"failure";
                const bool unavailable=[outcome isEqualToString:@"unavailable"];
                receipt[@"providerInitializationStatus"]=outcome;
                receipt[@"reason"]=error.localizedDescription ?: @"No accelerated CGL device";
                receipt[@"status"]=unavailable ? @"NOT_RUN" : @"FAILED"; emit(receipt); return unavailable ? 77 : 1;
            }
            receipt[@"adapterCapabilities"]=MellowCopyRenderAdapterCapabilities(device); receipt[@"providerInitializationStatus"]=@"ready";
            if (present) {
                [NSApplication sharedApplication];
                check([NSApp setActivationPolicy:NSApplicationActivationPolicyRegular],"Cannot activate the presentation application");
                [NSApp finishLaunching];
                window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,640,480)
                    styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
                check(window!=nil,"Cannot allocate the application window"); window.releasedWhenClosed=NO;
                window.title=@"Mellow opt-in CGL / Metal-selector render"; window.contentView.wantsLayer=YES;
                CALayer *layer=[CALayer layer]; layer.frame=window.contentView.bounds; layer.contentsScale=window.backingScaleFactor;
                [window.contentView.layer addSublayer:layer]; presenter=[[MellowSurfacePresenter alloc] initWithLayer:layer];
                check(presenter!=nil,"Cannot create main-thread surface presenter");
                [window center]; [window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
                pumpView(); check(window.isVisible,"Application window did not become visible");
            }
            NSString *source=[NSString stringWithUTF8String:MellowRenderFixture];
            id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error]; check(library!=nil && error==nil,"Render MSL translation failed");
            id<MTLFunction> vertex=[library newFunctionWithName:@"triangleVertex"],fragment=[library newFunctionWithName:@"gradientFragment"];
            check(vertex.functionType==MTLFunctionTypeVertex && fragment.functionType==MTLFunctionTypeFragment,"Wrong render stage functions");
            MTLRenderPipelineDescriptor *descriptor=[MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction=vertex; descriptor.fragmentFunction=fragment; descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
            id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error]; check(pipeline!=nil && error==nil,"Actual CGL pipeline build failed");
            MTLRenderPipelineDescriptor *badDescriptor=[descriptor copy]; badDescriptor.colorAttachments[0].blendingEnabled=YES; error=nil;
            reject([device newRenderPipelineStateWithDescriptor:badDescriptor error:&error]==nil && error!=nil,"Unsupported blending was ignored");
            badDescriptor=nil; descriptor.vertexFunction=nil; descriptor.fragmentFunction=nil; descriptor=nil; library=nil; vertex=nil; fragment=nil;
            MTLTextureDescriptor *textureDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:48 mipmapped:NO];
            textureDescriptor.storageMode=MTLStorageModeShared; textureDescriptor.resourceOptions=MTLResourceStorageModeShared; textureDescriptor.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> texture=[device newTextureWithDescriptor:textureDescriptor]; check(texture!=nil && texture.pixelFormat==MTLPixelFormatBGRA8Unorm,"Cannot create BGRA8 IOSurface texture");
            reject(MellowGetCompletedRenderTextureIOSurface(texture)==nullptr,"Unrendered texture exposed completed IOSurface");
            reject(unsupported(^{ (void)copiedRGBA(texture); }),"Unrendered texture fabricated readback");
            MTLTextureDescriptor *badTexture=[textureDescriptor copy]; badTexture.mipmapLevelCount=2;
            reject(unsupported(^{ (void)[device newTextureWithDescriptor:badTexture]; }),"Unsupported mip levels were ignored");
            id<MTLCommandQueue> queue=[device newCommandQueue];
            id<MTLCommandBuffer> badLoad=[queue commandBuffer]; MTLRenderPassDescriptor *badPass=passFor(texture); badPass.colorAttachments[0].loadAction=MTLLoadActionLoad;
            reject([badLoad renderCommandEncoderWithDescriptor:badPass]==nil,"Load action was silently replaced with clear"); failed(badLoad,"Invalid load pass completed");
            id<MTLCommandBuffer> differentStages=[queue commandBuffer]; id<MTLRenderCommandEncoder> encoder=[differentStages renderCommandEncoderWithDescriptor:passFor(texture)];
            [encoder setRenderPipelineState:pipeline]; std::array<float,4> p={0,0,.5f,1.f/64.f},q=p; q[2]=.75f;
            [encoder setVertexBytes:p.data() length:sizeof(p) atIndex:0]; [encoder setFragmentBytes:q.data() length:sizeof(q) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding]; failed(differentStages,"Distinct vertex/fragment parameters were silently merged"); encoder=nil;
            id<MTLCommandBuffer> compute=[queue commandBuffer]; reject([compute computeCommandEncoder]==nil,"CGL render device fabricated compute interop"); failed(compute,"Unsupported compute completed");
            id<MTLCommandBuffer> endedCommand=[queue commandBuffer]; encoder=[endedCommand renderCommandEncoderWithDescriptor:passFor(texture)];
            [encoder setRenderPipelineState:pipeline]; [encoder setVertexBytes:p.data() length:sizeof(p) atIndex:0];
            [encoder setFragmentBytes:p.data() length:sizeof(p) atIndex:0]; [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding];
            reject(unsupported(^{ [encoder setVertexBytes:p.data() length:sizeof(p) atIndex:0]; }),"Ended render encoder accepted wrapper vertex bytes");
            reject(unsupported(^{ [encoder setFragmentBytes:p.data() length:sizeof(p) atIndex:0]; }),"Ended render encoder accepted wrapper fragment bytes");
            reject(unsupported(^{ [encoder setRenderPipelineState:pipeline]; }),"Ended render encoder accepted a pipeline");
            reject(unsupported(^{ [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }),"Ended render encoder accepted draw");
            failed(endedCommand,"Ended encoder mutations were ignored"); encoder=nil; endedCommand=nil;
            badPass.colorAttachments[0].texture=nil; badPass=nil; badLoad=nil; differentStages=nil; compute=nil;

            NSMutableArray *samples=[NSMutableArray new]; __block unsigned callbacks=0; size_t verifiedPixels=0;
            for (unsigned iteration=0;iteration<2;++iteration) {
                if (iteration==1) texture=[device newTextureWithDescriptor:textureDescriptor];
                const std::array<float,4> parameters={iteration ? -.025f : .03f, iteration ? .02f : -.02f,
                    float((arc4random()%192)+32)/255.f,1.f/64.f};
                id<MTLCommandBuffer> command=[queue commandBuffer]; MTLRenderPassDescriptor *pass=passFor(texture);
                encoder=[command renderCommandEncoderWithDescriptor:pass]; pass.colorAttachments[0].texture=nil; pass=nil;
                check(encoder!=nil,"Cannot encode render pass"); [encoder setRenderPipelineState:pipeline];
                [encoder setVertexBytes:parameters.data() length:sizeof(parameters) atIndex:0]; [encoder setFragmentBytes:parameters.data() length:sizeof(parameters) atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding]; encoder=nil;
                check(MellowCopyRenderCommandExecutionEvidence(command)==nil,"Unsubmitted render fabricated completion");
                __weak id<MTLRenderPipelineState> weakPipeline=pipeline; __weak id<MTLCommandQueue> weakQueue=queue;
                __weak id<MTLTexture> weakTexture=texture;
                if (iteration==1) {
                    pipeline=nil; queue=nil; texture=nil;
                    check(weakPipeline==nil && weakQueue!=nil && weakTexture!=nil,"Retained render object lifetime is incorrect");
                }
                __block bool callbackVerified=false,callbackAsync=false,sameWorkerWaitRejected=false; __block id<MTLCommandBuffer> nested=nil;
                const auto submittingThread=std::this_thread::get_id();
                [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                    ++callbacks; callbackAsync=std::this_thread::get_id()!=submittingThread;
                    id<MTLTexture> retainedTexture=weakTexture;
                    callbackVerified=completed.status==MTLCommandBufferStatusCompleted && !completed.error &&
                        retainedTexture!=nil && MellowGetCompletedRenderTextureIOSurface(retainedTexture)!=nullptr &&
                        Mellow::Presentation::verifyRenderFixture(copiedRGBA(retainedTexture),parameters).passed;
                    nested=[completed.commandQueue commandBuffer]; [nested commit]; sameWorkerWaitRejected=unsupported(^{ [nested waitUntilCompleted]; });
                }];
                receipt[@"renderSubmissionAttempted"]=@YES; [command commit]; [command waitUntilCompleted];
                check(command.status==MTLCommandBufferStatusCompleted && command.error==nil && callbackVerified && callbackAsync && callbacks==iteration+1,
                      "Render completion preceded actual GPU readback or missed retained resources");
                reject(sameWorkerWaitRejected,"Same-worker render wait was not rejected"); [nested waitUntilCompleted]; reject(nested.status==MTLCommandBufferStatusError,"Nested empty render command completed");
                reject(unsupported(^{ [command commit]; }),"Render command submitted twice");
                texture=weakTexture; check(texture!=nil,"Command lost its completed Objective-C texture resource");
                const auto rgba=copiedRGBA(texture); const auto proof=Mellow::Presentation::verifyRenderFixture(rgba,parameters);
                check(proof.passed && proof.pixels==64*48,"GPU pixels failed independent triangle/gradient verification"); verifiedPixels+=proof.pixels;
                NSArray<NSDictionary *> *frames=MellowCopyRenderCommandExecutionEvidence(command); check(frames.count==1,"Wrong actual render frame count");
                NSDictionary *frame=frames[0]; IOSurfaceRef surface=MellowGetCompletedRenderTextureIOSurface(texture);
                check(surface!=nullptr && [frame[@"renderSubmitted"] boolValue] && [frame[@"fenceSignaled"] boolValue] &&
                      [frame[@"readbackCompleted"] boolValue] && [frame[@"resourcesReleased"] boolValue] && [frame[@"ioSurfaceWritten"] boolValue] &&
                      [frame[@"ioSurfaceID"] unsignedIntValue]==IOSurfaceGetID(surface),"Actual CGL GPU fence/IOSurface completion is missing");
                check(![frame[@"displayScanoutVerified"] boolValue] && ![frame[@"swapCompleted"] boolValue],"Offscreen render claimed swap or scanout");
                reject(unsupported(^{ [texture getBytes:reinterpret_cast<void *>(std::numeric_limits<uintptr_t>::max()-7)
                    bytesPerRow:64*4 fromRegion:MTLRegionMake2D(0,0,64,48) mipmapLevel:0]; }),"Overflowing texture destination interval was admitted");
                error=nil; CGImageRef image=MellowCopyCompletedSurfaceImage(surface,YES,&error); check(image!=nullptr && error==nil,"Cannot snapshot completed borrowed IOSurface");
                try { matchSurfaceImage(image,rgba); } catch (...) { CGImageRelease(image); throw; } CGImageRelease(image);
                NSMutableDictionary *sample=[frame mutableCopy]; sample[@"verifiedPixels"]=@(proof.pixels); sample[@"foregroundPixels"]=@(proof.foreground);
                sample[@"parameters"]=@[@(parameters[0]),@(parameters[1]),@(parameters[2]),@(parameters[3])];
                if (present) {
                    NSDictionary *presentation=[presenter presentCompletedSurface:surface sourceBottomLeft:YES error:&error];
                    check(presentation!=nil && error==nil && [presentation[@"snapshotCreated"] boolValue] &&
                          [presentation[@"transactionCommitCalled"] boolValue] && [presentation[@"surfaceID"] unsignedIntValue]==IOSurfaceGetID(surface) &&
                          ![presentation[@"scanoutObserved"] boolValue] && ![presentation[@"systemMetalRegistration"] boolValue] &&
                          ![presentation[@"systemWindowServerAcceleration"] boolValue],"Main-thread application presentation receipt failed");
                    sample[@"presentation"]=presentation; pumpView(); check(window.isVisible,"Application presentation window is hidden");
                }
                [samples addObject:sample];
            }
            if (present) { pumpView(2.0); check(window.isVisible,"Completed frame window is hidden"); }
            receipt[@"status"]=@"PASSED"; receipt[@"executions"]=samples; receipt[@"framesVerified"]=@2; receipt[@"verifiedPixels"]=@(verifiedPixels);
            receipt[@"completionHandlersVerified"]=@2; receipt[@"independentReadbackVerified"]=@YES; receipt[@"gpuFenceVerified"]=@YES;
            receipt[@"gpuResourcesReleased"]=@YES; receipt[@"ioSurfaceWritten"]=@YES; receipt[@"retainedResourceLifetimeVerified"]=@YES;
            receipt[@"applicationSnapshotQueued"]=@(present); result=0;
        } @catch (NSException *exception) { throw std::runtime_error((exception.reason ?: exception.name).UTF8String); }
        } catch (const std::exception &error) { receipt[@"status"]=@"FAILED"; receipt[@"reason"]=@(error.what()); }
        if (window) [window orderOut:nil]; receipt[@"checks"]=@(checks); receipt[@"negativeChecks"]=@(negativeChecks); emit(receipt); return result;
    }
}
