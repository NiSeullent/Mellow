#import "NativeMetalCompute.h"

#if defined(__APPLE__)
#if !__has_feature(objc_arc)
#error NativeMetalCompute.mm requires Objective-C ARC.
#endif

#import <dispatch/dispatch.h>
#include "MetalObjects.hpp"
#include <cctype>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

NSErrorDomain const MellowNativeComputeErrorDomain = @"org.mellow.NativeMetalCompute";
NSExceptionName const MellowNativeComputeUnsupportedException = @"MellowNativeComputeUnsupportedSelector";

@class MNCDevice, MNCBuffer, MNCLibrary, MNCFunction, MNCPipeline;
@class MNCQueue, MNCCommand, MNCEncoder;

namespace {
NSError *nativeError(const MellowMTL::Error &error) {
    NSString *message = [[NSString alloc] initWithBytes:error.message.data()
                                               length:error.message.size() encoding:NSUTF8StringEncoding];
    return [NSError errorWithDomain:MellowNativeComputeErrorDomain
                               code:static_cast<NSInteger>(error.code)
                           userInfo:@{NSLocalizedDescriptionKey: message ?: @"Mellow GPU runtime failed"}];
}
NSError *adapterError(MellowMTL::ErrorCode code, NSString *message) {
    return [NSError errorWithDomain:MellowNativeComputeErrorDomain code:static_cast<NSInteger>(code)
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}
void setError(NSError *__autoreleasing *out, NSError *error) { if (out) *out = error; }
[[noreturn]] void invalid(NSString *reason) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
}
[[noreturn]] void unsupported(id object, SEL selector) {
    @throw [NSException exceptionWithName:MellowNativeComputeUnsupportedException
        reason:[NSString stringWithFormat:@"%@ does not implement %@ in the opt-in compute subset",
                NSStringFromClass([object class]), NSStringFromSelector(selector)] userInfo:nil];
}
bool utf8(NSString *text, std::string &output) {
    if (!text) return false;
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    if (!data || !data.length || data.length > MellowRT::ShaderJit::MaxSourceBytes) return false;
    output.assign(static_cast<const char *>(data.bytes), data.length);
    return output.find('\0') == std::string::npos;
}
// This only discovers a name. ShaderJit validates the complete input and ABI;
// the real provider subsequently compiles its translated OpenCL source.
std::string singleKernelEntry(const std::string &source) {
    std::vector<std::string> tokens;
    for (size_t i = 0; i < source.size();) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (std::isspace(c)) { ++i; continue; }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            i = source.find('\n', i + 2); if (i == std::string::npos) break; continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            const size_t end = source.find("*/", i + 2);
            if (end == std::string::npos) return {};
            i = end + 2; continue;
        }
        if (std::isalpha(c) || c == '_') {
            const size_t begin = i++;
            while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')) ++i;
            tokens.push_back(source.substr(begin, i - begin));
        } else { tokens.emplace_back(1, source[i++]); }
    }
    std::string entry;
    for (size_t i = 0; i + 3 < tokens.size(); ++i) {
        if (tokens[i] != "kernel" || tokens[i + 1] != "void" || tokens[i + 3] != "(") continue;
        if (!entry.empty()) return {};
        entry = tokens[i + 2];
    }
    return entry;
}
void requireNative(bool okay, const MellowMTL::Error &error) {
    if (!okay) invalid(nativeError(error).localizedDescription);
}
}

// No <MTLDevice>, <MTLResource>, etc. declarations: these objects expose only
// the checked public selector subset, not every required protocol operation.
@interface MNCObject : NSObject
@property(nonatomic, copy, nullable) NSString *label;
@end

@interface MNCDevice : MNCObject {
@public
    std::shared_ptr<MellowMTL::Device> _native;
    dispatch_queue_t _worker;
}
- (instancetype)initWithNative:(std::shared_ptr<MellowMTL::Device>)native;
- (NSUInteger)maxBufferLength;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newSharedBufferWithLength:(NSUInteger)length bytes:(const void *)bytes options:(MTLResourceOptions)options;
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError *__autoreleasing *)error;
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError *__autoreleasing *)error;
@end

@interface MNCBuffer : MNCObject {
@public
    MNCDevice *_owner;
    std::shared_ptr<MellowMTL::Buffer> _native;
    std::vector<uint32_t> _storage;
    MTLResourceOptions _options;
}
- (instancetype)initWithDevice:(MNCDevice *)device native:(std::shared_ptr<MellowMTL::Buffer>)native
                        words:(std::vector<uint32_t>)words options:(MTLResourceOptions)options;
- (NSUInteger)length;
@end

@interface MNCLibrary : MNCObject {
@public
    MNCDevice *_owner;
    std::shared_ptr<MellowMTL::Library> _native;
    std::shared_ptr<MellowMTL::Function> _function;
    std::shared_ptr<MellowMTL::ComputePipeline> _pipeline;
}
- (instancetype)initWithDevice:(MNCDevice *)device native:(std::shared_ptr<MellowMTL::Library>)native
                      function:(std::shared_ptr<MellowMTL::Function>)function
                      pipeline:(std::shared_ptr<MellowMTL::ComputePipeline>)pipeline;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
- (NSArray<NSString *> *)functionNames;
@end

@interface MNCFunction : MNCObject {
@public
    MNCLibrary *_library;
    std::shared_ptr<MellowMTL::Function> _native;
}
- (instancetype)initWithLibrary:(MNCLibrary *)library;
- (id<MTLDevice>)device;
@end

@interface MNCPipeline : MNCObject {
@public
    MNCFunction *_function;
    std::shared_ptr<MellowMTL::ComputePipeline> _native;
}
- (instancetype)initWithFunction:(MNCFunction *)function;
@end

@interface MNCQueue : MNCObject {
@public
    MNCDevice *_owner;
    std::shared_ptr<MellowMTL::CommandQueue> _native;
    NSMutableArray<MNCCommand *> *_pending;
}
- (instancetype)initWithDevice:(MNCDevice *)device;
- (id<MTLCommandBuffer>)commandBuffer;
- (void)reserve:(MNCCommand *)command;
- (void)submitReady;
@end

@interface MNCCommand : MNCObject {
@public
    __weak MNCQueue *_owner;
    // A queue owns reservations; a reserved command must not own that queue.
    // Unreserved and dispatched commands retain their public queue normally.
    MNCQueue *_queueLease;
    MNCDevice *_device;
    std::shared_ptr<MellowMTL::CommandBuffer> _native;
    NSCondition *_state;
    MTLCommandBufferStatus _status;
    NSError *_failure;
    BOOL _activeEncoder, _hasDispatch, _reserved, _submitted, _handlersFinished, _cancelled;
    NSMutableArray<MNCBuffer *> *_buffers;
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
}
- (instancetype)initWithQueue:(MNCQueue *)queue;
- (id<MTLDevice>)device;
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
- (void)enqueue;
- (void)commit;
- (BOOL)isReady;
- (void)execute;
- (void)queueWasReleased;
- (void)finishWithFailure:(NSError *)failure scheduled:(BOOL)scheduled;
- (void)recordBuffer:(MNCBuffer *)buffer;
- (void)encoderEnded;
- (void)requireEncoding;
@end

@interface MNCEncoder : MNCObject {
@public
    MNCCommand *_command;
    std::shared_ptr<MellowMTL::ComputeEncoder> _native;
    MNCBuffer *_buffer;
    MNCPipeline *_pipeline;
    BOOL _ended;
}
- (instancetype)initWithCommand:(MNCCommand *)command native:(std::shared_ptr<MellowMTL::ComputeEncoder>)native;
- (void)requireActive;
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)group;
- (void)endEncoding;
@end

@implementation MNCObject
- (void)doesNotRecognizeSelector:(SEL)selector { unsupported(self, selector); }
@end

@implementation MNCDevice
- (instancetype)initWithNative:(std::shared_ptr<MellowMTL::Device>)native {
    if ((self = [super init])) {
        _native = std::move(native);
        _worker = dispatch_queue_create("org.mellow.native-compute", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(_worker, (__bridge const void *)self, (__bridge void *)self, nullptr);
    }
    return self;
}
- (NSString *)name { return [NSString stringWithUTF8String:_native->hardware().name.c_str()] ?: @"Mellow OpenCL compute"; }
- (BOOL)supportsFamily:(MTLGPUFamily)family { (void)family; return NO; }
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set { (void)set; return NO; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)count { (void)count; return NO; }
- (NSUInteger)maxBufferLength { return MellowRT::OpenCLProvider::MaxElements * sizeof(uint32_t); }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1, 1, 1); }
- (id<MTLCommandQueue>)newCommandQueue { return (id<MTLCommandQueue>)[[MNCQueue alloc] initWithDevice:self]; }
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    return [self newSharedBufferWithLength:length bytes:nullptr options:options];
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
    if (!bytes) return nil;
    return [self newSharedBufferWithLength:length bytes:bytes options:options];
}
- (id<MTLBuffer>)newSharedBufferWithLength:(NSUInteger)length bytes:(const void *)bytes options:(MTLResourceOptions)options {
    if (options != MTLResourceStorageModeShared &&
        options != (MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked)) unsupported(self, _cmd);
    if (!length || length % sizeof(uint32_t) || length > self.maxBufferLength) return nil;
    try {
        std::vector<uint32_t> words(length / sizeof(uint32_t), 0);
        if (bytes) std::memcpy(words.data(), bytes, length);
        MellowMTL::Error error;
        auto native = _native->newBuffer(words, error);
        if (!native) return nil;
        return (id<MTLBuffer>)[[MNCBuffer alloc] initWithDevice:self native:std::move(native)
                                                      words:std::move(words) options:options];
    } catch (const std::exception &) { return nil; }
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError *__autoreleasing *)out {
    setError(out, nil);
    if (options) { setError(out, adapterError(MellowMTL::ErrorCode::Unsupported, @"Only nil compile options are supported")); return nil; }
    try {
        std::string text;
        if (!utf8(source, text)) { setError(out, adapterError(MellowMTL::ErrorCode::InvalidArgument, @"MSL source must be 1-65536 UTF-8 bytes without NUL")); return nil; }
        const auto entry = singleKernelEntry(text);
        if (entry.empty()) { setError(out, adapterError(MellowMTL::ErrorCode::Compilation, @"Exactly one kernel void entry is required")); return nil; }
        MellowMTL::Error error;
        auto library = _native->newLibraryWithSource(text, error);
        if (!library) { setError(out, nativeError(error)); return nil; }
        auto function = library->newFunction(entry, error);
        if (!function) { setError(out, nativeError(error)); return nil; }
        auto pipeline = _native->newComputePipeline(function, error);
        if (!pipeline) { setError(out, nativeError(error)); return nil; }
        return (id<MTLLibrary>)[[MNCLibrary alloc] initWithDevice:self native:std::move(library)
                                                      function:std::move(function) pipeline:std::move(pipeline)];
    } catch (const std::exception &error) {
        setError(out, adapterError(MellowMTL::ErrorCode::Compilation, [NSString stringWithUTF8String:error.what()] ?: @"Compiler failed")); return nil;
    }
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError *__autoreleasing *)out {
    setError(out, nil);
    if (![(id)function isKindOfClass:[MNCFunction class]] || ((MNCFunction *)function)->_library->_owner != self) {
        setError(out, adapterError(MellowMTL::ErrorCode::WrongDevice, @"Function must belong to this opt-in compute device")); return nil;
    }
    return (id<MTLComputePipelineState>)[[MNCPipeline alloc] initWithFunction:(MNCFunction *)function];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
    options:(MTLPipelineOption)options reflection:(MTLComputePipelineReflection *__autoreleasing *)reflection
    error:(NSError *__autoreleasing *)out {
    if (reflection) *reflection = nil;
    if (options != MTLPipelineOptionNone) { setError(out, adapterError(MellowMTL::ErrorCode::Unsupported, @"Pipeline reflection/options are unsupported")); return nil; }
    return [self newComputePipelineStateWithFunction:function error:out];
}
@end

@implementation MNCBuffer
- (instancetype)initWithDevice:(MNCDevice *)device native:(std::shared_ptr<MellowMTL::Buffer>)native
                        words:(std::vector<uint32_t>)words options:(MTLResourceOptions)options {
    if ((self = [super init])) { _owner = device; _native = std::move(native); _storage = std::move(words); _options = options; }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (NSUInteger)length { return _storage.size() * sizeof(uint32_t); }
- (NSUInteger)allocatedSize { return self.length; }
- (void *)contents { return _storage.data(); }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode {
    return _options == MTLResourceStorageModeShared ? MTLHazardTrackingModeDefault : MTLHazardTrackingModeTracked;
}
- (MTLResourceOptions)resourceOptions { return _options; }
- (id<MTLHeap>)heap { return nil; }
- (BOOL)isAliasable { return NO; }
@end

@implementation MNCLibrary
- (instancetype)initWithDevice:(MNCDevice *)device native:(std::shared_ptr<MellowMTL::Library>)native
                      function:(std::shared_ptr<MellowMTL::Function>)function
                      pipeline:(std::shared_ptr<MellowMTL::ComputePipeline>)pipeline {
    if ((self = [super init])) { _owner = device; _native = std::move(native); _function = std::move(function); _pipeline = std::move(pipeline); }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (NSArray<NSString *> *)functionNames { return @[[NSString stringWithUTF8String:_function->name().c_str()]]; }
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    if (!name || ![name isEqualToString:self.functionNames.firstObject]) return nil;
    return (id<MTLFunction>)[[MNCFunction alloc] initWithLibrary:self];
}
@end

@implementation MNCFunction
- (instancetype)initWithLibrary:(MNCLibrary *)library {
    if ((self = [super init])) { _library = library; _native = library->_function; }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)_library->_owner; }
- (NSString *)name { return [NSString stringWithUTF8String:_native->name().c_str()]; }
- (MTLFunctionType)functionType { return MTLFunctionTypeKernel; }
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary { return @{}; }
@end

@implementation MNCPipeline
- (instancetype)initWithFunction:(MNCFunction *)function {
    if ((self = [super init])) { _function = function; _native = function->_library->_pipeline; }
    return self;
}
- (id<MTLDevice>)device { return _function.device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
// threadExecutionWidth is intentionally unavailable: the existing provider
// does not expose the GPU's SIMD width, and a fabricated width would mislead.
@end

@implementation MNCQueue
- (instancetype)initWithDevice:(MNCDevice *)device {
    if ((self = [super init])) { _owner = device; _native = device->_native->newCommandQueue(); _pending = [NSMutableArray array]; }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)_owner; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)[[MNCCommand alloc] initWithQueue:self]; }
- (void)reserve:(MNCCommand *)command {
    @synchronized (self) {
        [_pending addObject:command];
        command->_queueLease = nil;
    }
}
- (void)submitReady {
    @synchronized (self) {
        while (_pending.count && [_pending.firstObject isReady]) {
            MNCCommand *command = _pending.firstObject;
            // Restore the strong queue owner only after leaving the pending
            // ownership graph. The asynchronous block retains the command.
            [_pending removeObjectAtIndex:0];
            command->_queueLease = self;
            dispatch_async(_owner->_worker, ^{ @autoreleasepool { [command execute]; } });
        }
    }
}
- (void)dealloc {
    // An abandoned reservation cannot keep its queue alive. Commands keep
    // device/backend/resource owners independently, and committed waiters get
    // an explicit cancellation rather than waiting for a queue that is gone.
    for (MNCCommand *command in _pending) [command queueWasReleased];
}
@end

@implementation MNCCommand
- (instancetype)initWithQueue:(MNCQueue *)queue {
    if ((self = [super init])) {
        _owner = queue; _queueLease = queue; _device = queue->_owner;
        _native = queue->_native->commandBuffer(); _state = [NSCondition new];
        _status = MTLCommandBufferStatusNotEnqueued; _buffers = [NSMutableArray array];
        _scheduledHandlers = [NSMutableArray array]; _completedHandlers = [NSMutableArray array];
    }
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)_device; }
- (id<MTLCommandQueue>)commandQueue {
    MNCQueue *queue = _owner;
    if (!queue) invalid(@"The reserved command's queue was released and its reservation was cancelled");
    return (id<MTLCommandQueue>)queue;
}
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferErrorOption)errorOptions { return MTLCommandBufferErrorOptionNone; }
- (MTLCommandBufferStatus)status { [_state lock]; const auto status = _status; [_state unlock]; return status; }
- (NSError *)error { [_state lock]; NSError *error = _failure; [_state unlock]; return error; }
- (void)requireEncoding {
    [_state lock]; const BOOL okay = !_submitted && !_cancelled; [_state unlock];
    if (!okay) invalid(@"Encoding is forbidden after commit or reservation cancellation");
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    [self requireEncoding];
    if (_activeEncoder) invalid(@"End the active encoder before creating another");
    MellowMTL::Error error;
    auto native = _native->computeCommandEncoder(error);
    requireNative(static_cast<bool>(native), error);
    _activeEncoder = YES;
    return (id<MTLComputeCommandEncoder>)[[MNCEncoder alloc] initWithCommand:self native:std::move(native)];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
    if (type != MTLDispatchTypeSerial) unsupported(self, _cmd);
    return [self computeCommandEncoder];
}
- (void)recordBuffer:(MNCBuffer *)buffer {
    if (![_buffers containsObject:buffer]) [_buffers addObject:buffer];
    _hasDispatch = YES;
}
- (void)encoderEnded { _activeEncoder = NO; }
- (void)enqueue {
    MNCQueue *queue __attribute__((objc_precise_lifetime)) = _owner;
    if (!queue) invalid(@"Cannot enqueue after the command queue was released");
    [_state lock];
    if (_submitted || _cancelled) { [_state unlock]; invalid(@"Cannot enqueue a committed or cancelled command buffer"); }
    if (_reserved) { [_state unlock]; return; }
    _reserved = YES; _status = MTLCommandBufferStatusEnqueued;
    [_state unlock];
    [queue reserve:self];
}
- (BOOL)isReady { [_state lock]; const BOOL ready = _submitted; [_state unlock]; return ready; }
- (void)commit {
    MNCQueue *queue __attribute__((objc_precise_lifetime)) = _owner;
    if (!queue) invalid(@"Cannot commit after the reserved command queue was released");
    [_state lock];
    if (_submitted || _cancelled || _activeEncoder || !_hasDispatch) {
        [_state unlock]; invalid(@"Commit requires encoded work, ended encoders, and an uncommitted command buffer");
    }
    const BOOL needsReservation = !_reserved;
    _reserved = YES; _submitted = YES; _status = MTLCommandBufferStatusCommitted;
    [_state unlock];
    if (needsReservation) [queue reserve:self];
    [queue submitReady];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {
    [_state lock];
    if (_submitted || _cancelled || !handler) { [_state unlock]; invalid(@"Add a nonnil scheduled handler before commit or cancellation"); }
    [_scheduledHandlers addObject:[handler copy]]; [_state unlock];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    [_state lock];
    if (_submitted || _cancelled || !handler) { [_state unlock]; invalid(@"Add a nonnil completed handler before commit or cancellation"); }
    [_completedHandlers addObject:[handler copy]]; [_state unlock];
}
- (void)waitUntilScheduled {
    if (dispatch_get_specific((__bridge const void *)_device)) invalid(@"Waiting on the device submission worker would deadlock");
    [_state lock];
    if (!_submitted) { [_state unlock]; invalid(@"Commit before waiting for scheduling"); }
    while (_status < MTLCommandBufferStatusScheduled) [_state wait];
    [_state unlock];
}
- (void)waitUntilCompleted {
    if (dispatch_get_specific((__bridge const void *)_device)) invalid(@"Waiting on the device submission worker would deadlock");
    [_state lock];
    if (!_submitted) { [_state unlock]; invalid(@"Commit before waiting for completion"); }
    while (!_handlersFinished) [_state wait];
    [_state unlock];
}
- (void)execute {
    NSError *failure = nil;
    BOOL scheduled = NO;
    try {
        MellowMTL::Error error;
        for (MNCBuffer *buffer in _buffers) {
            if (!buffer->_native->write(buffer->_storage, error)) { failure = nativeError(error); break; }
        }
        if (!failure && !_native->commit(error)) failure = nativeError(error);
        const auto &executions = _native->executions();
        for (const auto &execution : executions) scheduled |= execution.submitted;
        if (!failure && (_native->status() != MellowMTL::CommandStatus::Completed || executions.empty()))
            failure = adapterError(MellowMTL::ErrorCode::Execution, @"GPU runtime returned no completed dispatch evidence");
        if (!failure) {
            for (const auto &execution : executions) {
                if (!execution.submitted || !execution.executionCompleted || !execution.eventOwnershipVerified ||
                    !execution.profilingVerified || !execution.resourcesReleased) {
                    failure = adapterError(MellowMTL::ErrorCode::Execution, @"GPU dispatch completion, ownership, profiling or release evidence is missing"); break;
                }
            }
        }
        if (!failure) {
            // The portable runtime also checks ownership and resource release.
            // Recheck output size before touching stable public contents memory.
            std::vector<std::vector<uint32_t>> outputs;
            for (MNCBuffer *buffer in _buffers) {
                auto words = buffer->_native->read();
                if (words.size() != buffer->_storage.size()) {
                    failure = adapterError(MellowMTL::ErrorCode::Execution, @"Verified GPU readback has an unexpected buffer length"); break;
                }
                outputs.push_back(std::move(words));
            }
            if (!failure) {
                NSUInteger index = 0;
                for (MNCBuffer *buffer in _buffers) {
                    std::memcpy(buffer->_storage.data(), outputs[index++].data(), buffer.length);
                }
            }
        }
    } catch (const std::exception &error) {
        failure = adapterError(MellowMTL::ErrorCode::Execution, [NSString stringWithUTF8String:error.what()] ?: @"GPU submission failed");
    }
    [self finishWithFailure:failure scheduled:scheduled];
}
- (void)queueWasReleased {
    NSError *failure = adapterError(MellowMTL::ErrorCode::InvalidState, @"Command queue released before its reserved command could be submitted");
    [_state lock];
    _cancelled = YES;
    const BOOL committed = _submitted;
    if (!committed) {
        _failure = failure; _status = MTLCommandBufferStatusError; _handlersFinished = YES;
        [_scheduledHandlers removeAllObjects]; [_completedHandlers removeAllObjects]; [_state broadcast];
    }
    [_state unlock];
    if (committed) {
        dispatch_async(_device->_worker, ^{ @autoreleasepool { [self finishWithFailure:failure scheduled:NO]; } });
    }
}
- (void)finishWithFailure:(NSError *)failure scheduled:(BOOL)scheduled {
    [_state lock];
    _failure = failure ? [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
        userInfo:@{NSLocalizedDescriptionKey: failure.localizedDescription, NSUnderlyingErrorKey: failure}] : nil;
    _status = failure ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    NSArray *scheduledHandlers = scheduled ? [_scheduledHandlers copy] : @[];
    NSArray *completedHandlers = [_completedHandlers copy];
    [_scheduledHandlers removeAllObjects]; [_completedHandlers removeAllObjects];
    [_state broadcast]; [_state unlock];
    // Do not hold state locks while calling client code. A throwing client
    // handler must not strand waiters or prevent the remaining handlers.
    for (id object in scheduledHandlers) {
        MTLCommandBufferHandler handler = (MTLCommandBufferHandler)object;
        try {
            @try { handler((id<MTLCommandBuffer>)self); } @catch (NSException *exception) { (void)exception; }
        } catch (...) { /* Client callback failures do not strand completion. */ }
    }
    for (id object in completedHandlers) {
        MTLCommandBufferHandler handler = (MTLCommandBufferHandler)object;
        try {
            @try { handler((id<MTLCommandBuffer>)self); } @catch (NSException *exception) { (void)exception; }
        } catch (...) { /* Client callback failures do not strand completion. */ }
    }
    [_state lock]; _handlersFinished = YES; [_state broadcast]; [_state unlock];
}
@end

@implementation MNCEncoder
- (instancetype)initWithCommand:(MNCCommand *)command native:(std::shared_ptr<MellowMTL::ComputeEncoder>)native {
    if ((self = [super init])) { _command = command; _native = std::move(native); }
    return self;
}
- (id<MTLDevice>)device { return _command.device; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
- (void)requireActive { [_command requireEncoding]; if (_ended) invalid(@"Compute encoder has ended"); }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline {
    [self requireActive];
    if (![(id)pipeline isKindOfClass:[MNCPipeline class]] || ((MNCPipeline *)pipeline)->_function->_library->_owner != _command->_device)
        invalid(@"Compute pipeline must belong to this opt-in device");
    MellowMTL::Error error;
    requireNative(_native->setComputePipeline(((MNCPipeline *)pipeline)->_native, error), error);
    _pipeline = (MNCPipeline *)pipeline;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self requireActive];
    if (offset != 0 || index != 0) unsupported(self, _cmd);
    if (![(id)buffer isKindOfClass:[MNCBuffer class]] || ((MNCBuffer *)buffer)->_owner != _command->_device)
        invalid(@"Buffer must belong to this opt-in device");
    MellowMTL::Error error;
    requireNative(_native->setBuffer(((MNCBuffer *)buffer)->_native, 0, error), error);
    _buffer = (MNCBuffer *)buffer;
}
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self requireActive];
    if (!_buffer || offset != 0 || index != 0) unsupported(self, _cmd);
}
- (void)setBuffers:(const id<MTLBuffer> *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range {
    if (!buffers || !offsets || range.location != 0 || range.length != 1) unsupported(self, _cmd);
    [self setBuffer:buffers[0] offset:offsets[0] atIndex:0];
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)group {
    [self requireActive];
    if (grid.height != 1 || grid.depth != 1 || group.width != 1 || group.height != 1 || group.depth != 1) unsupported(self, _cmd);
    if (!_buffer || !_pipeline) invalid(@"Bind a pipeline and buffer before dispatch");
    MellowMTL::Error error;
    requireNative(_native->dispatchThreads(grid.width, error), error);
    [_command recordBuffer:_buffer];
}
- (void)dispatchThreadgroups:(MTLSize)grid threadsPerThreadgroup:(MTLSize)group {
    // With the only supported group size {1,1,1}, grids are identical.
    [self dispatchThreads:grid threadsPerThreadgroup:group];
}
- (void)endEncoding {
    [self requireActive];
    MellowMTL::Error error;
    requireNative(_native->endEncoding(error), error);
    _ended = YES; [_command encoderEnded];
}
@end

id<MTLDevice> MellowCreateNativeComputeDevice(NSUInteger index, NSError *__autoreleasing *out) {
    setError(out, nil);
    try {
        MellowMTL::Error error;
        auto native = MellowMTL::Device::createOpenCL(index, error);
        if (!native) { setError(out, nativeError(error)); return nil; }
        return (id<MTLDevice>)[[MNCDevice alloc] initWithNative:std::move(native)];
    } catch (const std::exception &error) {
        setError(out, adapterError(MellowMTL::ErrorCode::Execution, [NSString stringWithUTF8String:error.what()] ?: @"GPU initialization failed")); return nil;
    }
}

BOOL MellowIsNativeComputeAdapter(id object) { return [object isKindOfClass:[MNCDevice class]]; }

namespace {
id<MTLLibrary> airLibrary(id<MTLDevice> device, NSData *bitcode, NSString *assembly, NSString *entry,
                         NSString *llvmPath, NSError *__autoreleasing *out) {
    setError(out, nil);
    if (!MellowIsNativeComputeAdapter(device)) {
        setError(out, adapterError(MellowMTL::ErrorCode::WrongDevice, @"AIR input requires an opt-in Mellow compute device")); return nil;
    }
    try {
        std::string entryText, path, text;
        if (!utf8(entry, entryText) || entryText.size() > 128 ||
            (!assembly && (!utf8(llvmPath, path) || path.front() != '/'))) {
            setError(out, adapterError(MellowMTL::ErrorCode::InvalidArgument, @"AIR entry or LLVM path is invalid")); return nil;
        }
        MNCDevice *owner = (MNCDevice *)device;
        MellowMTL::Error error;
        std::shared_ptr<MellowMTL::Library> library;
        if (assembly) {
            if (!utf8(assembly, text)) { setError(out, adapterError(MellowMTL::ErrorCode::InvalidArgument, @"AIR text must be 1-65536 UTF-8 bytes without NUL")); return nil; }
            library = owner->_native->newLibraryWithAirText(text, error);
        } else {
            if (!bitcode || !bitcode.length || bitcode.length > MellowRT::ShaderJit::MaxSourceBytes) {
                setError(out, adapterError(MellowMTL::ErrorCode::InvalidArgument, @"AIR bitcode must be 1-65536 bytes")); return nil;
            }
            const auto *bytes = static_cast<const uint8_t *>(bitcode.bytes);
            library = owner->_native->newLibraryWithAir(std::vector<uint8_t>(bytes, bytes + bitcode.length), path, error);
        }
        if (!library) { setError(out, nativeError(error)); return nil; }
        auto function = library->newFunction(entryText, error);
        if (!function) { setError(out, nativeError(error)); return nil; }
        auto pipeline = owner->_native->newComputePipeline(function, error);
        if (!pipeline) { setError(out, nativeError(error)); return nil; }
        return (id<MTLLibrary>)[[MNCLibrary alloc] initWithDevice:owner native:std::move(library)
                                                      function:std::move(function) pipeline:std::move(pipeline)];
    } catch (const std::exception &error) {
        setError(out, adapterError(MellowMTL::ErrorCode::Compilation, [NSString stringWithUTF8String:error.what()] ?: @"AIR compilation failed")); return nil;
    }
}
}

id<MTLLibrary> MellowNativeComputeNewLibraryWithAIR(id<MTLDevice> device, NSData *bitcode, NSString *entry,
    NSString *llvmPath, NSError *__autoreleasing *out) { return airLibrary(device, bitcode, nil, entry, llvmPath, out); }

id<MTLLibrary> MellowNativeComputeNewLibraryWithAIRText(id<MTLDevice> device, NSString *assembly, NSString *entry,
    NSError *__autoreleasing *out) { return airLibrary(device, nil, assembly, entry, nil, out); }
#endif
