// SPDX-License-Identifier: MIT
// Native, explicitly selected adapter acceptance. No system Metal replacement.
#import "MellowAppleMetal.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
unsigned checks, negativeChecks;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void reject(bool condition, const char *message) { ++negativeChecks; check(condition,message); }
bool raisesUnsupported(void (^operation)(void)) {
    @try { operation(); } @catch (NSException *exception) {
        return [exception.name isEqualToString:MellowAppleMetalUnsupportedException];
    }
    return false;
}
NSArray<NSNumber *> *words(const std::vector<uint32_t> &values) {
    NSMutableArray<NSNumber *> *result=[NSMutableArray arrayWithCapacity:values.size()];
    for (const uint32_t value : values) [result addObject:@(value)];
    return [result copy];
}
void emit(NSDictionary *receipt) {
    NSError *error=nil;
    NSData *data=[NSJSONSerialization dataWithJSONObject:receipt options:NSJSONWritingPrettyPrinted error:&error];
    if (!data) { std::fprintf(stderr,"Cannot serialize acceptance receipt\n"); return; }
    std::fwrite(data.bytes,1,data.length,stdout); std::fputc('\n',stdout);
}
void encode(id<MTLCommandBuffer> command, id<MTLComputePipelineState> pipeline,
            id<MTLBuffer> buffer, unsigned dispatches=1) {
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    check(encoder!=nil,"Cannot create compute encoder");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    for (unsigned i=0;i<dispatches;++i)
        [encoder dispatchThreads:MTLSizeMake(buffer.length/sizeof(uint32_t),1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding];
}
void expectFailed(id<MTLCommandBuffer> command, const char *message) {
    __block unsigned callbacks=0;
    __block bool callbackSawFailure=false;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        ++callbacks;
        callbackSawFailure=completed.status==MTLCommandBufferStatusError && completed.error!=nil;
    }];
    [command commit]; [command waitUntilCompleted];
    reject(command.status==MTLCommandBufferStatusError && command.error!=nil &&
           callbacks==1 && callbackSawFailure,message);
    NSArray *events=MellowCopyCommandExecutionEvidence(command);
    reject(events!=nil && events.count==0,"Rejected encoding submitted GPU work");
}
NSString * const Source=@"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void mellow_apple_acceptance(device uint *values [[buffer(0)]], uint tid [[thread_position_in_grid]]) {\n"
    "    uint original = values[tid];\n"
    "    values[tid] = original * 7u + 3u;\n"
    "}\n";
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSMutableDictionary *receipt=[@{@"status":@"NOT_RUN", @"acceptance":@"apple-opt-in-compute",
            @"systemMetalRegistered":@NO, @"windowServerIntegrated":@NO,
            @"fullMetalProtocolConformance":@NO, @"physicalPCIIdentityVerified":@NO,
            @"providerInitializationStatus":@"unavailable", @"bootstrapSubmissionAttempted":@NO,
            @"independentReadbackVerified":@NO, @"gpuEventOwnershipVerified":@NO,
            @"gpuProfilingVerified":@NO, @"gpuResourcesReleased":@NO,
            @"dispatchesVerified":@0, @"completionHandlersVerified":@0,
            @"retainedResourceLifetimeVerified":@NO} mutableCopy];
        int result=1;
        try {
            @try {
                NSUInteger index=0;
                if (argc>2) throw std::runtime_error("Usage: mellow-compute-acceptance [opencl-gpu-index]");
                if (argc==2) {
                    char *end=nullptr; errno=0;
                    const unsigned long long parsed=std::strtoull(argv[1],&end,10);
                    if (!argv[1][0] || argv[1][0]=='-' || !end || *end || errno ||
                        parsed>std::numeric_limits<NSUInteger>::max())
                        throw std::runtime_error("OpenCL GPU index must be a nonnegative integer");
                    index=NSUInteger(parsed);
                }
                receipt[@"openCLGPUIndex"]=@(index);
                receipt[@"osVersion"]=NSProcessInfo.processInfo.operatingSystemVersionString;
#if !defined(__x86_64__)
                receipt[@"reason"]=@"Acceptance requires an x86_64 host"; emit(receipt); return 77;
#endif
                const NSOperatingSystemVersion version=NSProcessInfo.processInfo.operatingSystemVersion;
                if (version.majorVersion!=15 && version.majorVersion!=26) {
                    receipt[@"reason"]=@"Acceptance requires macOS 15 or 26"; emit(receipt); return 77;
                }
                NSError *error=nil;
                id<MTLDevice> device=MellowCreateDevice(index,&error);
                if (!device) {
                    NSString *outcome=error.userInfo[MellowAppleMetalInitializationStatusKey] ?: @"failure";
                    const BOOL attempted=[error.userInfo[MellowAppleMetalBootstrapSubmissionAttemptedKey] boolValue];
                    const BOOL unavailable=[outcome isEqualToString:@"unavailable"] && !attempted;
                    receipt[@"providerInitializationStatus"]=outcome;
                    receipt[@"bootstrapSubmissionAttempted"]=@(attempted);
                    receipt[@"status"]=unavailable ? @"NOT_RUN" : @"FAILED";
                    receipt[@"reason"]=error.localizedDescription ?: @"No validated OpenCL GPU provider";
                    emit(receipt); return unavailable ? 77 : 1;
                }
                receipt[@"adapterCapabilities"]=MellowCopyAdapterCapabilities(device);
                receipt[@"providerInitializationStatus"]=@"ready";
                receipt[@"bootstrapSubmissionAttempted"]=@YES;
                check(device.maxBufferLength==4096*sizeof(uint32_t),"Unexpected adapter buffer limit");
                reject(![device supportsFamily:MTLGPUFamilyMac2],"Adapter advertised an unimplemented Apple GPU family");

                id<MTLLibrary> library=[device newLibraryWithSource:Source options:nil error:&error];
                check(library!=nil && error==nil,"Source compilation failed");
                check([library.functionNames isEqualToArray:@[@"mellow_apple_acceptance"]],"Wrong validated library entry");
                id<MTLFunction> function=[library newFunctionWithName:@"mellow_apple_acceptance"];
                check(function!=nil && function.functionType==MTLFunctionTypeKernel,"Wrong compute function");
                id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
                check(pipeline!=nil && error==nil,"Actual GPU pipeline build failed");
                reject(raisesUnsupported(^{ (void)pipeline.threadExecutionWidth; }),"Invented physical Metal SIMD width");
                reject([library newFunctionWithName:@"absent"]==nil,"Missing entry returned a function");
                MTLFunctionConstantValues *constants=[MTLFunctionConstantValues new];
                error=nil;
                reject([library newFunctionWithName:@"mellow_apple_acceptance" constantValues:constants error:&error]==nil &&
                       [error.domain isEqualToString:MellowAppleMetalErrorDomain],"Specialization was silently accepted");
                error=nil;
                reject([device newLibraryWithSource:Source options:[MTLCompileOptions new] error:&error]==nil && error!=nil,
                       "Compilation options were silently ignored");
                error=nil;
                reject([device newLibraryWithSource:@"kernel void bad(device float *x [[buffer(0)]], uint tid [[thread_position_in_grid]]) { x[tid] = 1.0; }"
                    options:nil error:&error]==nil && error!=nil,"Unsupported source became a usable library");
                reject(raisesUnsupported(^{ (void)[device newBufferWithLength:0 options:MTLResourceStorageModeShared]; }),"Empty buffer was admitted");
                reject(raisesUnsupported(^{ (void)[device newBufferWithLength:8 options:MTLResourceStorageModePrivate]; }),"Private buffer was silently staged as shared");

                const uint32_t seed=arc4random();
                std::vector<uint32_t> input(256), expected(256);
                for (size_t i=0;i<input.size();++i) {
                    input[i]=seed ^ uint32_t((i+1)*UINT64_C(0x9e3779b9));
                    expected[i]=(input[i]*7u+3u)*7u+3u;
                }
                id<MTLBuffer> buffer=[device newBufferWithBytes:input.data() length:input.size()*sizeof(uint32_t)
                    options:MTLResourceStorageModeShared];
                check(buffer!=nil && buffer.contents!=nullptr && buffer.storageMode==MTLStorageModeShared,"Cannot allocate shared buffer");
                void *const originalPointer=buffer.contents;
                id<MTLCommandQueue> queue=[device newCommandQueue];

                id<MTLCommandBuffer> badBinding=[queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder=[badBinding computeCommandEncoder];
                [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:0 atIndex:1]; [encoder endEncoding];
                expectFailed(badBinding,"Invalid buffer index completed successfully");
                id<MTLCommandBuffer> badOffset=[queue commandBuffer]; encoder=[badOffset computeCommandEncoder];
                [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:4 atIndex:0]; [encoder endEncoding];
                expectFailed(badOffset,"Invalid buffer offset completed successfully");
                id<MTLCommandBuffer> badGrid=[queue commandBuffer]; encoder=[badGrid computeCommandEncoder];
                [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:0 atIndex:0];
                [encoder dispatchThreads:MTLSizeMake(input.size()+1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [encoder endEncoding];
                expectFailed(badGrid,"Nonexact dispatch completed successfully");
                id<MTLCommandBuffer> badGroup=[queue commandBuffer]; encoder=[badGroup computeCommandEncoder];
                [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:0 atIndex:0];
                [encoder dispatchThreads:MTLSizeMake(input.size(),1,1) threadsPerThreadgroup:MTLSizeMake(8,1,1)]; [encoder endEncoding];
                expectFailed(badGroup,"Unsupported group geometry completed successfully");
                id<MTLCommandBuffer> render=[queue commandBuffer];
                reject([render renderCommandEncoderWithDescriptor:[MTLRenderPassDescriptor renderPassDescriptor]]==nil,"Compute device admitted render");
                expectFailed(render,"Unsupported render completed successfully");
                id<MTLCommandBuffer> blit=[queue commandBuffer];
                reject([blit blitCommandEncoder]==nil,"Compute device admitted blit");
                expectFailed(blit,"Unsupported blit completed successfully");
                id<MTLCommandBuffer> abandoned=[queue commandBuffer];
                @autoreleasepool { id<MTLComputeCommandEncoder> temporary=[abandoned computeCommandEncoder]; temporary=nil; }
                expectFailed(abandoned,"Abandoned encoder completed successfully");
                id<MTLCommandBuffer> earlyCommit=[queue commandBuffer]; encoder=[earlyCommit computeCommandEncoder];
                [earlyCommit commit];
                reject(raisesUnsupported(^{ [encoder endEncoding]; }),"Encoder mutated an already committed command");
                [earlyCommit waitUntilCompleted];
                reject(earlyCommit.status==MTLCommandBufferStatusError,"Active encoder commit completed successfully");
                encoder=nil;

                id<MTLCommandBuffer> command=[queue commandBuffer];
                encode(command,pipeline,buffer,2);
                check(MellowCopyCommandExecutionEvidence(command)==nil,"Unsubmitted command fabricated completion events");
                __weak id<MTLCommandQueue> weakQueue=queue;
                __weak id<MTLComputePipelineState> weakPipeline=pipeline;
                library=nil; function=nil; pipeline=nil; queue=nil;
                check(weakQueue!=nil && weakPipeline==nil,"Object retention did not match command/resource lifetime");
                const std::thread::id submittingThread=std::this_thread::get_id();
                __block unsigned callbacks=0;
                __block bool callbackSawCompletion=false, callbackReadback=false, callbackAsynchronous=false;
                __block bool workerWaitRejected=false;
                __block id<MTLCommandBuffer> nested=nil;
                [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                    ++callbacks;
                    callbackSawCompletion=completed.status==MTLCommandBufferStatusCompleted && completed.error==nil;
                    callbackAsynchronous=std::this_thread::get_id()!=submittingThread;
                    callbackReadback=std::memcmp(buffer.contents,expected.data(),expected.size()*sizeof(uint32_t))==0;
                    nested=[completed.commandQueue commandBuffer]; [nested commit];
                    workerWaitRejected=raisesUnsupported(^{ [nested waitUntilCompleted]; });
                }];
                [command commit]; [command waitUntilCompleted];
                check(command.status==MTLCommandBufferStatusCompleted && command.error==nil,"Compute command did not complete");
                check(callbacks==1 && callbackSawCompletion && callbackReadback && callbackAsynchronous,
                      "Completion callback ran before readback or without driver completion");
                reject(workerWaitRejected,"Same-device completion wait was not rejected");
                [nested waitUntilCompleted];
                reject(nested.status==MTLCommandBufferStatusError,"Nested empty command unexpectedly completed");
                reject(raisesUnsupported(^{ [command commit]; }),"Command submitted twice");
                reject(raisesUnsupported(^{ [command addCompletedHandler:^(id<MTLCommandBuffer> unused) { (void)unused; }]; }),
                       "Late completion handler was accepted");
                check(buffer.contents==originalPointer,"Shared contents pointer changed after readback");
                std::vector<uint32_t> actual(input.size());
                std::memcpy(actual.data(),buffer.contents,actual.size()*sizeof(uint32_t));
                check(actual==expected,"Full GPU readback disagrees with the independent uint32 reference");
                NSArray<NSDictionary *> *events=MellowCopyCommandExecutionEvidence(command);
                check(events.count==2,"Wrong actual dispatch event count");
                uint64_t lastSequence=0,lastEnd=0,epoch=0;
                for (NSDictionary *event in events) {
                    check([event[@"submitted"] boolValue] && [event[@"submissionAttempted"] boolValue] &&
                          [event[@"executionCompleted"] boolValue] && [event[@"runtimePlanned"] boolValue] &&
                          [event[@"eventOwnershipVerified"] boolValue] && [event[@"profilingVerified"] boolValue] &&
                          [event[@"resourcesReleased"] boolValue],"GPU event ownership, completion or cleanup is missing");
                    check(![event[@"resultsVerified"] boolValue] && ![event[@"runtimeCompletionAccepted"] boolValue],
                          "Runtime mislabeled independent arithmetic evidence");
                    const uint64_t sequence=[event[@"sequence"] unsignedLongLongValue];
                    const uint64_t start=[event[@"gpuStart"] unsignedLongLongValue],end=[event[@"gpuEnd"] unsignedLongLongValue];
                    const uint64_t currentEpoch=[event[@"epoch"] unsignedLongLongValue];
                    check(sequence>lastSequence && start>=lastEnd && end>start && (!epoch || currentEpoch==epoch),
                          "OpenCL execution events are not ordered within one session");
                    lastSequence=sequence; lastEnd=end; epoch=currentEpoch;
                }
                receipt[@"status"]=@"PASSED"; receipt[@"seed"]=@(seed); receipt[@"shaderSource"]=Source;
                receipt[@"input"]=words(input); receipt[@"independentExpected"]=words(expected); receipt[@"actualReadback"]=words(actual);
                receipt[@"executions"]=events; receipt[@"independentReadbackVerified"]=@YES;
                receipt[@"gpuEventOwnershipVerified"]=@YES; receipt[@"gpuProfilingVerified"]=@YES;
                receipt[@"gpuResourcesReleased"]=@YES; receipt[@"dispatchesVerified"]=@2;
                receipt[@"completionHandlersVerified"]=@1; receipt[@"retainedResourceLifetimeVerified"]=@YES;
                receipt[@"completionCallbackAfterReadbackVerified"]=@YES;
                receipt[@"sameWorkerWaitRejected"]=@(workerWaitRejected);
                result=0;
            } @catch (NSException *exception) {
                throw std::runtime_error((exception.reason ?: exception.name).UTF8String);
            }
        } catch (const std::exception &error) {
            receipt[@"status"]=@"FAILED"; receipt[@"reason"]=@(error.what());
        }
        receipt[@"checks"]=@(checks); receipt[@"negativeChecks"]=@(negativeChecks);
        emit(receipt); return result;
    }
}
