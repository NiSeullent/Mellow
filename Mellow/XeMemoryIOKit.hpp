// Local 7D41 research implementation, 2026. See LICENSE and NOTICE.
#pragma once
#include "XeMemory.hpp"
#include <IOKit/IOMapper.h>

namespace XeMemory {
// Retain the admitted PCI device's actual iommu-parent mapper. A mapper lookup
// that falls back to the system mapper is not device ownership evidence.
// Null is refused, never silently replaced with identity mapping or a mapper
// from another device. Caller owns/retains mapper and physical admission.
struct IOKitContext {
    IOKitContext() = default;
    IOKitContext(const IOKitContext &) = delete;
    IOKitContext &operator=(const IOKitContext &) = delete;
    IOMapper *mapper {};
    uint64_t maxAllocationBytes {64ULL * 1024 * 1024};
    uint64_t maxPinnedBytes {256ULL * 1024 * 1024};
    uint64_t pinnedBytes {};
};
Backend makeIOKitPinBackend(IOKitContext &context);
// All adapter calls and quota access must be serialized in sleepable client
// context, never interrupt context or a gated work-loop action: DMA prepare can
// block. The context and its retained mapper must outlive every returned pin.
// Synchronize covers IODMACommand bounce-buffer copies; it is NOT a GPU engine
// cache flush, an MMIO ordering barrier, or evidence of GPU job completion.
// Failed descriptor or DMA completion preserves its pin and charge permanently
// until an independent cleanup owner can recover the underlying mapping. A
// later NotReady/clear success or GPU reset never supplies that missing proof.
Status synchronizeForDevice(const Pin &pin);
Status synchronizeForCpu(const Pin &pin);
void *kernelBuffer(const Pin &pin);
// Resolve only an exact, still-prepared private pin created by this context.
// This verifies owner, complete allocation extent and retained mapper identity;
// pin.cookie remains a trusted kernel handle, never an arbitrary client pointer.
void *resolvePinnedBuffer(IOKitContext &context, uint64_t owner, uint64_t bytes, const Pin &pin);
// Inspect a direct shared pin without preparing, completing, synchronizing,
// allocating or writing its memory. Requires the caller's sleepable owner lock
// throughout; segment enumeration changes the command's internal cursor. The
// original CPU extent, descriptor, prepared range and preparation ID must stay
// unchanged, with a valid charge under the context's current quotas. Each
// freshly enumerated 4K DMA page must equal the stored pin and
// translate to the original descriptor's physical first and last byte through
// the same retained 4K mapper. A bounce descriptor or any mismatch returns null
// while retaining the entire pin and its quota charge. The generic resolver
// above continues to support pins whose command uses a bounce buffer.
// This is a bounded page-identity inspection, not proof of CPU cache coherence,
// active device/IOMMU ownership, GPU IOTLB health or GPU completion. The physical
// owner must independently prove those before and after using the result.
// pin.cookie remains a private trusted kernel handle, never a client address.
void *resolveDirectPinnedBuffer(IOKitContext &context, uint64_t owner, uint64_t bytes,
                               const Pin &pin);
// This adapter allocates and pins real IOKit-owned system-memory pages. It does
// NOT create GPU page tables, publish a context root, invalidate GPU TLBs, handle
// GPU interrupts or provide bind/unbind/fenceComplete callbacks.
}
