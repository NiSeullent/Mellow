// Local 7D41 research implementation, 2026. See LICENSE and NOTICE.
#include "XeMemoryIOKit.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

namespace XeMemory {
struct Resource {
    IOBufferMemoryDescriptor *memory {};
    IODMACommand *command {};
    IOMapper *mapper {};
    IOKitContext *context {};
    uint64_t *pages {};
    uint64_t owner {}, bytes {};
    size_t pageCount {};
    bool descriptorAttempted {}, descriptorPrepared {}, charged {};
    bool commandAttempted {}, commandPrepared {}, cleanupUncertain {};
};

// Only called before GPU bind or after the VM backend proved unbind+invalidate.
static Status releaseResource(Resource *resource) {
    // XNU clearMemoryDescriptor(true) discards complete() errors. complete may
    // consume its active count yet fail IOMMU unmap; retry NotReady does not
    // repair that lost cleanup authority. Retain all backing after uncertainty.
    if (resource->cleanupUncertain) return Status::BackendFailure;
    if (resource->command) {
        if (resource->commandAttempted) {
            const auto status = resource->command->complete(true, true);
            if (status != kIOReturnSuccess) {
                resource->cleanupUncertain = true;
                return Status::BackendFailure;
            }
            resource->commandAttempted = resource->commandPrepared = false;
        }
        if (resource->command->getMemoryDescriptor() &&
            (resource->command->clearMemoryDescriptor(false) != kIOReturnSuccess ||
             resource->command->getMemoryDescriptor()))
            return Status::BackendFailure;
        resource->command->release();
        resource->command = nullptr;
    }
    if (resource->descriptorAttempted) {
        if (resource->memory->complete(kIODirectionInOut) != kIOReturnSuccess) {
            resource->cleanupUncertain = true;
            return Status::BackendFailure;
        }
        resource->descriptorAttempted = resource->descriptorPrepared = false;
    }
    if (resource->memory) resource->memory->release();
    if (resource->mapper) resource->mapper->release();
    if (resource->pages) IOFree(resource->pages, resource->pageCount * sizeof(uint64_t));
    if (resource->charged) resource->context->pinnedBytes -= resource->bytes;
    IOFree(resource, sizeof(Resource));
    return Status::Ok;
}
static void describe(Resource *resource, Pin &pin) {
    pin = Pin {resource, resource->pages, resource->pageCount};
}
static Status failedPin(Resource *resource, Pin &pin) {
    if (releaseResource(resource) != Status::Ok) describe(resource, pin);
    return Status::BackendFailure;
}
static Status pinMemory(void *opaque, uint64_t owner, uint64_t bytes, Pin &pin) {
    auto *context = static_cast<IOKitContext *>(opaque);
    if (!context || !context->mapper) return Status::Unavailable;
    if (!owner || !bytes || (bytes & (PageSize - 1)) || bytes > SIZE_MAX ||
        bytes > context->maxAllocationBytes || context->pinnedBytes > context->maxPinnedBytes ||
        bytes > context->maxPinnedBytes - context->pinnedBytes) return Status::Invalid;
    auto *resource = static_cast<Resource *>(IOMalloc(sizeof(Resource)));
    if (!resource) return Status::NoSpace;
    *resource = Resource {};
    resource->owner = owner;
    resource->bytes = bytes;
    resource->pageCount = static_cast<size_t>(bytes / PageSize);
    resource->context = context;
    context->pinnedBytes += bytes;
    resource->charged = true;
    resource->mapper = context->mapper;
    resource->mapper->retain();
    resource->pages = static_cast<uint64_t *>(IOMalloc(resource->pageCount * sizeof(uint64_t)));
    if (!resource->pages) return failedPin(resource, pin);
    resource->memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut, static_cast<vm_size_t>(bytes), PageSize);
    if (!resource->memory || !resource->memory->getBytesNoCopy()) return failedPin(resource, pin);
    bzero(resource->memory->getBytesNoCopy(), static_cast<size_t>(bytes));
    resource->descriptorAttempted = true;
    if (resource->memory->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return failedPin(resource, pin);
    resource->descriptorPrepared = true;
    // Intel xe's MTL descriptor (also selected for ARL IDs) uses 46-bit DMA.
    resource->command = IODMACommand::withSpecification(IODMACommand::OutputHost64,
        46, PageSize, IODMACommand::kMapped, bytes, PageSize, resource->mapper);
    if (!resource->command) return failedPin(resource, pin);
    if (resource->command->setMemoryDescriptor(resource->memory, false) != kIOReturnSuccess)
        return failedPin(resource, pin);
    resource->commandAttempted = true;
    if (resource->command->prepare(0, bytes) != kIOReturnSuccess) return failedPin(resource, pin);
    resource->commandPrepared = true;
    UInt64 offset = 0;
    for (size_t i = 0; i < resource->pageCount; ++i) {
        IODMACommand::Segment64 segment {};
        UInt32 count = 1;
        const UInt64 before = offset;
        if (resource->command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
            count != 1 || segment.fLength != PageSize || offset != before + PageSize)
            return failedPin(resource, pin);
        uint64_t encoded = 0;
        if (encodeSystemPte4K(segment.fIOVMAddr, 0, true, encoded) != Status::Ok)
            return failedPin(resource, pin);
        resource->pages[i] = segment.fIOVMAddr;
    }
    if (offset != bytes) return failedPin(resource, pin);
    describe(resource, pin);
    return Status::Ok;
}
static Resource *checked(const Pin &pin) {
    // Pins are private trusted-kernel handles, never user-supplied pointers.
    auto *resource = static_cast<Resource *>(pin.cookie);
    return resource && resource->pages == pin.dmaPages && resource->pageCount == pin.pageCount
        ? resource : nullptr;
}
static Status unpinMemory(void *opaque, Pin &pin) {
    auto *resource = checked(pin);
    if (!resource || resource->context != opaque) return Status::Invalid;
    const Status result = releaseResource(resource);
    if (result == Status::Ok) pin = Pin {};
    return result;
}
Backend makeIOKitPinBackend(IOKitContext &context) {
    Backend backend {};
    backend.context = &context;
    backend.pin = pinMemory;
    backend.unpin = unpinMemory;
    return backend;
}
Status synchronizeForDevice(const Pin &pin) {
    auto *resource = checked(pin);
    if (!resource || !resource->command || !resource->commandPrepared || resource->cleanupUncertain)
        return Status::Invalid;
    return resource->command->synchronize(kIODirectionOut) == kIOReturnSuccess ? Status::Ok : Status::BackendFailure;
}
Status synchronizeForCpu(const Pin &pin) {
    auto *resource = checked(pin);
    if (!resource || !resource->command || !resource->commandPrepared || resource->cleanupUncertain)
        return Status::Invalid;
    return resource->command->synchronize(kIODirectionIn) == kIOReturnSuccess ? Status::Ok : Status::BackendFailure;
}
void *kernelBuffer(const Pin &pin) {
    auto *resource = checked(pin);
    return resource && resource->memory && resource->commandPrepared && !resource->cleanupUncertain
        ? resource->memory->getBytesNoCopy() : nullptr;
}
void *resolvePinnedBuffer(IOKitContext &context, uint64_t owner, uint64_t bytes, const Pin &pin) {
    if (!owner || !bytes || (bytes & (PageSize - 1)) || bytes > SIZE_MAX || !context.mapper)
        return nullptr;
    auto *resource = checked(pin);
    if (!resource || resource->context != &context || resource->owner != owner ||
        resource->bytes != bytes || resource->pageCount != bytes / PageSize ||
        !resource->pages || !resource->charged || !resource->descriptorAttempted ||
        !resource->descriptorPrepared || !resource->commandAttempted ||
        !resource->commandPrepared || resource->cleanupUncertain || !resource->memory ||
        !resource->command || !resource->mapper || resource->mapper != context.mapper ||
        resource->memory->getLength() != bytes ||
        resource->command->getMemoryDescriptor() != resource->memory)
        return nullptr;
    return resource->memory->getBytesNoCopy();
}
}
