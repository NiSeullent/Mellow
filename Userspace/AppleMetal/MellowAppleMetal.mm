// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#import "MellowAppleMetal.h"
#import <dispatch/dispatch.h>
#include "../../Runtime/MetalObjects.hpp"
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>

#if !__has_feature(objc_arc)
#error "MellowAppleMetal requires Objective-C ARC"
#endif

NSString * const MellowAppleMetalErrorDomain = @"org.mellow.apple-metal";
NSExceptionName const MellowAppleMetalUnsupportedException = @"MellowAppleMetalUnsupportedOperation";
NSString * const MellowAppleMetalInitializationStatusKey = @"MellowProviderInitializationStatus";
NSString * const MellowAppleMetalBootstrapSubmissionAttemptedKey = @"MellowBootstrapSubmissionAttempted";

namespace {
NSError *errorFor(const MellowMTL::Error &error) {
    NSString *message = [[NSString alloc] initWithBytes:error.message.data()
        length:error.message.size() encoding:NSUTF8StringEncoding];
    return [NSError errorWithDomain:MellowAppleMetalErrorDomain code:NSInteger(error.code)
        userInfo:@{NSLocalizedDescriptionKey: message ?: @"Mellow runtime error"}];
}
NSError *failure(MellowMTL::ErrorCode code, NSString *message) {
    return [NSError errorWithDomain:MellowAppleMetalErrorDomain code:NSInteger(code)
        userInfo:@{NSLocalizedDescriptionKey: message}];
}
void assignError(NSError **out, NSError *error) { if (out) *out = error; }
[[noreturn]] void unsupported(NSString *message) {
    @throw [NSException exceptionWithName:MellowAppleMetalUnsupportedException reason:message userInfo:nil];
}
char deviceWorkerKey;
std::string utf8(NSString *text) {
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    return data ? std::string(static_cast<const char *>(data.bytes), data.length) : std::string();
}
// Discover the single entry for the existing production parser. This scanner
// does not validate MSL or emit code; Library::newFunction validates the entire
// original source, including all syntax, comments, types and bounded AST rules.
std::string discoverEntry(const std::string &source) {
    if (source.empty() || source.size() > MellowRT::ShaderJit::MaxSourceBytes) return {};
    std::vector<std::string> tokens;
    for (size_t i=0;i<source.size();) {
        if (source.compare(i,2,"//")==0) {
            while (i<source.size() && source[i]!='\n') ++i;
            continue;
        }
        if (source.compare(i,2,"/*")==0) {
            const size_t end=source.find("*/",i+2);
            if (end==std::string::npos) return {};
            i=end+2; continue;
        }
        const char c=source[i];
        if ((c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_') {
            const size_t start=i++;
            while (i<source.size()) {
                const char next=source[i];
                if (!((next>='a' && next<='z') || (next>='A' && next<='Z') ||
                      (next>='0' && next<='9') || next=='_')) break;
                ++i;
            }
            tokens.push_back(source.substr(start,i-start));
        } else { if (c!=' ' && c!='\t' && c!='\r' && c!='\n') tokens.emplace_back(1,c); ++i; }
        if (tokens.size()>MellowRT::ShaderJit::MaxTokens) return {};
    }
    std::string entry;
    for (size_t i=0;i+2<tokens.size();++i) if (tokens[i]=="kernel" && tokens[i+1]=="void") {
        if (!entry.empty()) return {};
        entry=tokens[i+2];
    }
    return entry;
}
}

@class MMADevice, MMABuffer, MMALibrary, MMAFunction, MMAPipeline, MMAQueue, MMACommand, MMAEncoder;

// These classes intentionally do not declare full Apple protocol conformance.
// The exported typed entry uses only the documented subset of Metal selectors.
@interface MMAObject : NSObject
@property(nonatomic,copy,nullable) NSString *label;
@end
@implementation MMAObject
- (void)doesNotRecognizeSelector:(SEL)selector {
    unsupported([NSString stringWithFormat:@"%@ does not implement %@ in the Mellow compute subset",
        NSStringFromClass(self.class), NSStringFromSelector(selector)]);
}
@end

@interface MMADevice : MMAObject {
@public
    std::shared_ptr<MellowMTL::Device> core;
    dispatch_queue_t worker;
}
@end
@interface MMABuffer : MMAObject {
@public
    MMADevice *owner;
    std::shared_ptr<MellowMTL::Buffer> core;
    std::vector<uint32_t> sharedWords;
}
- (BOOL)upload:(NSError **)error;
- (BOOL)download:(NSError **)error;
@end
@interface MMALibrary : MMAObject {
@public
    MMADevice *owner;
    std::shared_ptr<MellowMTL::Library> core;
    std::shared_ptr<MellowMTL::Function> entry;
}
@end
@interface MMAFunction : MMAObject {
@public
    MMADevice *owner;
    MMALibrary *library;
    std::shared_ptr<MellowMTL::Function> core;
}
@end
@interface MMAPipeline : MMAObject {
@public
    MMADevice *owner;
    MMAFunction *function;
    std::shared_ptr<MellowMTL::ComputePipeline> core;
}
@end
@interface MMAQueue : MMAObject {
@public
    MMADevice *owner;
    std::shared_ptr<MellowMTL::CommandQueue> core;
}
@end
@interface MMACommand : MMAObject {
@public
    MMAQueue *owner;
    std::shared_ptr<MellowMTL::CommandBuffer> core;
@private
    std::mutex stateMutex;
    std::condition_variable completed;
    MTLCommandBufferStatus commandStatus;
    NSError *commandError;
    NSMutableArray<MMABuffer *> *buffers;
    NSMutableArray *handlers;
    bool committed, encoderActive, done;
}
- (instancetype)initWithQueue:(MMAQueue *)queue;
- (void)recordFailure:(NSError *)error;
- (BOOL)canEncode;
- (BOOL)canEndEncoding;
- (void)encoderEnded;
- (void)trackBuffer:(MMABuffer *)buffer;
- (NSArray<NSDictionary<NSString *,id> *> *)copyExecutionEvidence;
@end
@interface MMAEncoder : MMAObject {
@public
    MMACommand *owner;
    std::shared_ptr<MellowMTL::ComputeEncoder> core;
@private
    bool ended;
    MMABuffer *boundBuffer;
}
@end

@implementation MMADevice
- (NSString *)name { return [@"Mellow / " stringByAppendingString:@(core->hardware().name.c_str())]; }
- (NSUInteger)maxBufferLength { return MellowRT::OpenCLProvider::MaxElements*sizeof(uint32_t); }
- (BOOL)supportsFamily:(MTLGPUFamily)family { (void)family; return NO; }
- (BOOL)supportsFeatureSet:(MTLFeatureSet)features { (void)features; return NO; }
- (id<MTLCommandQueue>)newCommandQueue {
    MMAQueue *queue=[MMAQueue new]; queue->owner=self; queue->core=core->newCommandQueue();
    return (id<MTLCommandQueue>)queue;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    return [self newBufferWithBytes:nullptr length:length options:options];
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
    if (options!=MTLResourceStorageModeShared || !length || length%sizeof(uint32_t) || length>self.maxBufferLength)
        unsupported(@"Buffers require default shared/untracked-flag-free options and 4..16384 bytes in uint32 multiples");
    try {
        std::vector<uint32_t> initial(length/sizeof(uint32_t),0);
        if (bytes) std::memcpy(initial.data(),bytes,length);
        MellowMTL::Error error; auto buffer=core->newBuffer(initial,error);
        if (!buffer) unsupported(errorFor(error).localizedDescription);
        MMABuffer *wrapper=[MMABuffer new]; wrapper->owner=self;
        wrapper->core=std::move(buffer); wrapper->sharedWords=std::move(initial);
        return (id<MTLBuffer>)wrapper;
    } catch (const std::exception &error) { unsupported(@(error.what())); }
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)out {
    assignError(out,nil);
    if (!source || options) {
        assignError(out,failure(MellowMTL::ErrorCode::Unsupported,@"Only source input with nil compilation options is supported")); return nil;
    }
    try {
        const std::string text=utf8(source), name=discoverEntry(text);
        if (name.empty()) {
            assignError(out,failure(MellowMTL::ErrorCode::Compilation,@"Expected one bounded kernel void entry")); return nil;
        }
        MellowMTL::Error error; auto lib=core->newLibraryWithSource(text,error);
        if (!lib) { assignError(out,errorFor(error)); return nil; }
        auto entry=lib->newFunction(name,error);
        if (!entry) { assignError(out,errorFor(error)); return nil; }
        MMALibrary *wrapper=[MMALibrary new]; wrapper->owner=self;
        wrapper->core=std::move(lib); wrapper->entry=std::move(entry);
        return (id<MTLLibrary>)wrapper;
    } catch (const std::exception &error) {
        assignError(out,failure(MellowMTL::ErrorCode::Compilation,@(error.what()))); return nil;
    }
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)input error:(NSError **)out {
    assignError(out,nil);
    if (![input isKindOfClass:MMAFunction.class] || ((MMAFunction *)input)->owner!=self) {
        assignError(out,failure(MellowMTL::ErrorCode::WrongDevice,@"Function must belong to this Mellow device")); return nil;
    }
    try {
        MMAFunction *function=(MMAFunction *)input; MellowMTL::Error error;
        auto pipeline=core->newComputePipeline(function->core,error);
        if (!pipeline) { assignError(out,errorFor(error)); return nil; }
        MMAPipeline *wrapper=[MMAPipeline new]; wrapper->owner=self;
        wrapper->function=function; wrapper->core=std::move(pipeline);
        return (id<MTLComputePipelineState>)wrapper;
    } catch (const std::exception &error) {
        assignError(out,failure(MellowMTL::ErrorCode::Compilation,@(error.what()))); return nil;
    }
}
@end

@implementation MMABuffer
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (NSUInteger)length { return sharedWords.size()*sizeof(uint32_t); }
- (void *)contents { return sharedWords.data(); }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared; }
- (BOOL)upload:(NSError **)out {
    MellowMTL::Error error;
    if (!core->write(sharedWords,error)) { assignError(out,errorFor(error)); return NO; }
    return YES;
}
- (BOOL)download:(NSError **)out {
    const auto result=core->read();
    if (result.size()!=sharedWords.size()) {
        assignError(out,failure(MellowMTL::ErrorCode::Execution,@"Runtime readback length changed")); return NO;
    }
    std::memcpy(sharedWords.data(),result.data(),self.length); // Stable contents pointer.
    return YES;
}
@end
@implementation MMALibrary
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (NSArray<NSString *> *)functionNames { return @[@(entry->name().c_str())]; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    if (!name || utf8(name)!=entry->name()) return nil;
    MMAFunction *wrapper=[MMAFunction new]; wrapper->owner=owner;
    wrapper->library=self; wrapper->core=entry; return (id<MTLFunction>)wrapper;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)out {
    (void)name; (void)values;
    assignError(out,failure(MellowMTL::ErrorCode::Unsupported,@"Function specialization is not implemented")); return nil;
}
@end
@implementation MMAFunction
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (NSString *)name { return @(core->name().c_str()); }
- (MTLFunctionType)functionType { return MTLFunctionTypeKernel; }
@end
@implementation MMAPipeline
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
// This adapter admits exactly one thread per group, independently of a device's
// physical SIMD width. Unsupported group geometry is rejected before dispatch.
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1; }
- (NSUInteger)threadExecutionWidth {
    unsupported(@"The existing provider does not expose a Metal physical SIMD width");
}
@end
@implementation MMAQueue
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)[[MMACommand alloc] initWithQueue:self]; }
@end

@implementation MMACommand
- (instancetype)initWithQueue:(MMAQueue *)queue {
    if ((self=[super init])) {
        owner=queue; core=queue->core->commandBuffer();
        commandStatus=MTLCommandBufferStatusNotEnqueued;
        buffers=[NSMutableArray new]; handlers=[NSMutableArray new];
    }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)owner->owner; }
- (id<MTLCommandQueue>)commandQueue { return (id<MTLCommandQueue>)owner; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferStatus)status { std::lock_guard<std::mutex> lock(stateMutex); return commandStatus; }
- (NSError *)error { std::lock_guard<std::mutex> lock(stateMutex); return commandError; }
- (void)recordFailure:(NSError *)error {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!committed && !commandError) commandError=error;
}
- (BOOL)canEncode { std::lock_guard<std::mutex> lock(stateMutex); return !committed && !commandError; }
- (BOOL)canEndEncoding { std::lock_guard<std::mutex> lock(stateMutex); return !committed; }
- (void)encoderEnded { std::lock_guard<std::mutex> lock(stateMutex); encoderActive=false; }
- (void)trackBuffer:(MMABuffer *)buffer { if (![buffers containsObject:buffer]) [buffers addObject:buffer]; }
- (NSArray<NSDictionary<NSString *,id> *> *)copyExecutionEvidence {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (commandStatus!=MTLCommandBufferStatusCompleted && commandStatus!=MTLCommandBufferStatusError) return nil;
    NSMutableArray *events=[NSMutableArray new];
    for (const auto &event : core->executions()) {
        [events addObject:@{@"epoch":@(event.epoch), @"sequence":@(event.sequence),
            @"gpuStart":@(event.gpuStart), @"gpuEnd":@(event.gpuEnd),
            @"submissionAttempted":@(event.submissionAttempted), @"submitted":@(event.submitted),
            @"executionCompleted":@(event.executionCompleted), @"runtimePlanned":@(event.runtimePlanned),
            @"eventOwnershipVerified":@(event.eventOwnershipVerified), @"profilingVerified":@(event.profilingVerified),
            @"resourcesReleased":@(event.resourcesReleased), @"resultsVerified":@(event.resultsVerified),
            @"runtimeCompletionAccepted":@(event.runtimeCompletionAccepted)}];
    }
    return [events copy];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (committed || commandError || encoderActive) {
        if (!committed && !commandError) commandError=failure(MellowMTL::ErrorCode::InvalidState,@"Only one active compute encoder is permitted");
        return nil;
    }
    MellowMTL::Error error; auto encoder=core->computeCommandEncoder(error);
    if (!encoder) { commandError=errorFor(error); return nil; }
    MMAEncoder *wrapper=[MMAEncoder new]; wrapper->owner=self; wrapper->core=std::move(encoder);
    encoderActive=true; return (id<MTLComputeCommandEncoder>)wrapper;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
    if (type!=MTLDispatchTypeSerial) {
        [self recordFailure:failure(MellowMTL::ErrorCode::Unsupported,@"Concurrent compute encoders are unsupported")]; return nil;
    }
    return [self computeCommandEncoder];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor {
    (void)descriptor;
    [self recordFailure:failure(MellowMTL::ErrorCode::Unsupported,@"Render is not connected to this compute device")]; return nil;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    [self recordFailure:failure(MellowMTL::ErrorCode::Unsupported,@"Blit is not implemented")]; return nil;
}
- (void)doesNotRecognizeSelector:(SEL)selector {
    [self recordFailure:failure(MellowMTL::ErrorCode::Unsupported,NSStringFromSelector(selector))];
    [super doesNotRecognizeSelector:selector];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!handler || committed) unsupported(@"Register a nonnull completion handler before commit");
    [handlers addObject:[handler copy]];
}
- (void)commit {
    NSArray<MMABuffer *> *retainedBuffers;
    NSArray *completionHandlers;
    NSError *encodingError;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (committed) unsupported(@"Command buffers may be committed only once");
        committed=true; commandStatus=MTLCommandBufferStatusCommitted;
        if (encoderActive && !commandError) commandError=failure(MellowMTL::ErrorCode::InvalidState,@"End the encoder before commit");
        encodingError=commandError;
        retainedBuffers=[buffers copy]; completionHandlers=[handlers copy];
        [handlers removeAllObjects]; // Break cycles through user callback captures.
    }
    // The block strongly retains the command, queue/device and all bound buffers
    // through real C++ driver completion, copied readback and completion handlers.
    // All adapter queues share this device worker so staging copies stay ordered.
    dispatch_async(owner->owner->worker, ^{
        @autoreleasepool {
            NSError *executionError=encodingError;
            @try { if (!executionError) {
                try {
                    for (MMABuffer *buffer in retainedBuffers)
                        if (![buffer upload:&executionError]) break;
                    if (!executionError) {
                        MellowMTL::Error error;
                        if (!self->core->commit(error)) executionError=errorFor(error);
                    }
                    if (!executionError) for (MMABuffer *buffer in retainedBuffers)
                        if (![buffer download:&executionError]) break;
                } catch (const std::exception &error) {
                    executionError=failure(MellowMTL::ErrorCode::Execution,@(error.what()));
                } catch (...) {
                    executionError=failure(MellowMTL::ErrorCode::Execution,@"GPU worker raised an unknown C++ exception");
                }
            } } @catch (NSException *exception) {
                executionError=failure(MellowMTL::ErrorCode::Execution,exception.reason ?: @"GPU worker raised an exception");
            }
            {
                std::lock_guard<std::mutex> lock(self->stateMutex);
                self->commandError=executionError;
                self->commandStatus=executionError ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
            }
            @try {
                for (MTLCommandBufferHandler handler in completionHandlers) {
                    @try {
                        try { handler((id<MTLCommandBuffer>)self); }
                        catch (const std::exception &error) { NSLog(@"Mellow completion handler raised %s",error.what()); }
                        catch (...) { NSLog(@"Mellow completion handler raised an unknown C++ exception"); }
                    }
                    @catch (NSException *exception) { NSLog(@"Mellow completion handler raised %@",exception); }
                }
            } @finally {
                {
                    std::lock_guard<std::mutex> lock(self->stateMutex);
                    self->done=true;
                }
                self->completed.notify_all();
            }
        }
    });
}
- (void)waitUntilCompleted {
    std::unique_lock<std::mutex> lock(stateMutex);
    if (!committed) unsupported(@"Commit before waitUntilCompleted");
    if (!done && dispatch_get_specific(&deviceWorkerKey)==(__bridge void *)owner->owner)
        unsupported(@"A device completion handler cannot wait for unfinished work on the same device worker");
    completed.wait(lock,[thisSelf=self] { return thisSelf->done; });
}
@end

@implementation MMAEncoder
- (id<MTLDevice>)device { return (id<MTLDevice>)owner->owner->owner; }
- (void)dealloc {
    if (!ended) {
        [owner recordFailure:failure(MellowMTL::ErrorCode::InvalidState,@"Encoder was released before endEncoding")];
        [owner encoderEnded];
    }
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline {
    if (![owner canEncode]) unsupported(@"Command buffer cannot encode in its current state");
    if (![pipeline isKindOfClass:MMAPipeline.class]) {
        [owner recordFailure:failure(MellowMTL::ErrorCode::WrongDevice,@"Expected a Mellow compute pipeline")]; return;
    }
    MellowMTL::Error error;
    if (!core->setComputePipeline(((MMAPipeline *)pipeline)->core,error)) [owner recordFailure:errorFor(error)];
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (![owner canEncode]) unsupported(@"Command buffer cannot encode in its current state");
    if (offset || index || ![buffer isKindOfClass:MMABuffer.class]) {
        [owner recordFailure:failure(MellowMTL::ErrorCode::Unsupported,@"Only a Mellow buffer at index zero, offset zero is supported")]; return;
    }
    MellowMTL::Error error;
    if (!core->setBuffer(((MMABuffer *)buffer)->core,0,error)) [owner recordFailure:errorFor(error)];
    else boundBuffer=(MMABuffer *)buffer;
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)group {
    if (![owner canEncode]) unsupported(@"Command buffer cannot encode in its current state");
    if (grid.height!=1 || grid.depth!=1 || group.width!=1 || group.height!=1 || group.depth!=1) {
        [owner recordFailure:failure(MellowMTL::ErrorCode::Unsupported,@"Only exact 1D grids with one thread per group are supported")]; return;
    }
    MellowMTL::Error error;
    if (!core->dispatchThreads(grid.width,error)) [owner recordFailure:errorFor(error)];
    else [owner trackBuffer:boundBuffer];
}
- (void)endEncoding {
    if (![owner canEndEncoding]) unsupported(@"End the encoder before committing its command buffer");
    MellowMTL::Error error;
    if (!core->endEncoding(error)) [owner recordFailure:errorFor(error)];
    else { ended=true; [owner encoderEnded]; }
}
- (void)doesNotRecognizeSelector:(SEL)selector {
    [owner recordFailure:failure(MellowMTL::ErrorCode::Unsupported,NSStringFromSelector(selector))];
    [super doesNotRecognizeSelector:selector];
}
@end

id<MTLDevice> MellowCreateDevice(NSUInteger index, NSError **out) {
    assignError(out,nil);
    MellowRT::OpenCLInitialization initialization;
    try {
        MellowMTL::Error error; auto core=MellowMTL::Device::createOpenCL(index,error,&initialization);
        if (!core) {
            NSError *runtimeError=errorFor(error);
            NSMutableDictionary *details=[runtimeError.userInfo mutableCopy];
            details[MellowAppleMetalInitializationStatusKey]=
                initialization.status==MellowRT::OpenCLInitializationStatus::Unavailable ? @"unavailable" : @"failure";
            details[MellowAppleMetalBootstrapSubmissionAttemptedKey]=@(initialization.bootstrapSubmissionAttempted);
            assignError(out,[NSError errorWithDomain:runtimeError.domain code:runtimeError.code userInfo:details]); return nil;
        }
        MMADevice *wrapper=[MMADevice new]; wrapper->core=std::move(core);
        wrapper->worker=dispatch_queue_create("org.mellow.apple-metal.compute",DISPATCH_QUEUE_SERIAL);
        if (!wrapper->worker) {
            assignError(out,[NSError errorWithDomain:MellowAppleMetalErrorDomain code:NSInteger(MellowMTL::ErrorCode::Execution)
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot create device worker", MellowAppleMetalInitializationStatusKey:@"failure",
                    MellowAppleMetalBootstrapSubmissionAttemptedKey:@(initialization.bootstrapSubmissionAttempted)}]); return nil;
        }
        dispatch_queue_set_specific(wrapper->worker,&deviceWorkerKey,(__bridge void *)wrapper,nullptr);
        return (id<MTLDevice>)wrapper;
    } catch (const std::exception &error) {
        assignError(out,[NSError errorWithDomain:MellowAppleMetalErrorDomain code:NSInteger(MellowMTL::ErrorCode::Execution)
            userInfo:@{NSLocalizedDescriptionKey:@(error.what()), MellowAppleMetalInitializationStatusKey:@"failure",
                MellowAppleMetalBootstrapSubmissionAttemptedKey:@(initialization.bootstrapSubmissionAttempted)}]); return nil;
    }
}
NSDictionary<NSString *,id> *MellowCopyAdapterCapabilities(id<MTLDevice> device) {
    if (![device isKindOfClass:MMADevice.class]) return nil;
    const auto &info=((MMADevice *)device)->core->hardware();
    const auto &bootstrap=((MMADevice *)device)->core->bootstrapEvidence();
    return @{@"provider":@"existing-host-opencl-gpu", @"api":@"selector-compatible-bounded-compute",
        @"openclPlatform":@(info.platform.c_str()), @"openclPlatformVendor":@(info.platformVendor.c_str()),
        @"openclDeviceName":@(info.name.c_str()), @"openclDeviceVendor":@(info.vendor.c_str()),
        @"openclDriver":@(info.driver.c_str()), @"openclVersion":@(info.version.c_str()),
        @"reportedVendorID":@(info.reportedVendorId),
        @"reportedDeviceID":info.deviceIdFromIntelExtension ? @(info.reportedDeviceId) : NSNull.null,
        @"deviceIDFromIntelExtension":@(info.deviceIdFromIntelExtension), @"physicalPCIIdentityVerified":@NO,
        @"providerInitializationStatus":@"ready", @"bootstrapSubmissionAttempted":@(bootstrap.submissionAttempted),
        @"bootstrapSubmitted":@(bootstrap.submitted), @"bootstrapResultsVerified":@(bootstrap.resultsVerified),
        @"bootstrapProfilingVerified":@(bootstrap.profilingVerified),
        @"bootstrapEventOwnershipVerified":@(bootstrap.eventOwnershipVerified),
        @"bootstrapResourcesReleased":@(bootstrap.resourcesReleased),
        @"maxBufferBytes":@(MellowRT::OpenCLProvider::MaxElements*sizeof(uint32_t)),
        @"bufferIndex":@0, @"bufferOffset":@0, @"threadsPerGroup":@1,
        @"fullMetalProtocolConformance":@NO, @"metalFamilyAdvertised":@NO,
        @"systemMetalRegistered":@NO, @"windowServerIntegrated":@NO};
}
NSArray<NSDictionary<NSString *,id> *> *MellowCopyCommandExecutionEvidence(id<MTLCommandBuffer> command) {
    if (![command isKindOfClass:MMACommand.class]) return nil;
    return [(MMACommand *)command copyExecutionEvidence];
}
