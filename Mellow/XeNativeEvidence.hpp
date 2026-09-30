// SPDX-License-Identifier: MIT
#pragma once
#include "NativeGpuSession.hpp"
#include "XeContextExecution.hpp"
#include "XeMemoryIOKit.hpp"

namespace XeNativeEvidence {
struct OwnerInspection {
    void *opaque {};
    // Inspect the actual retained service/physical PCI and current reset epoch.
    MellowNativeGpuStatus (*identity)(void *, uint64_t, MellowNativeGpuKernel::Identity &) {};
    // Same real monotonic time domain as ExecutionBackend::nowMicros.
    uint64_t (*nowMicros)(void *) {};
};
// Concrete native fixed-job binding: invokes existing GuC/context execution,
// reads the GPU-written fence, and synchronizes/copies its actual IOKit output.
// It supplies no firmware, fake mapper, topology, image or hardware owner.
// All referenced driver resources and immutable ZeBin bytes outlive close().
// Allocate off-stack (~16 KiB); serialize with the real owner's IRQ/reset/VM.
// The prepared context and six allocations belong to this exact clientOwner.
// Input/output must use real XeMemory IOKit pins. Before publication this binding
// writes input[i] = (i * 2654435761U) ^ 0xa5a55a5aU and output bytes 0xcd for
// count words, then synchronizes both pins for the GPU. ExecutionBackend's
// stageHeaps must preserve these input/output bytes while copying the four heaps.
// No output value is computed here; readback copies only actual completed DMA.
class Binding {
public:
    Binding(XeMemory::VirtualMemory &, XeGuC::Transport &, XeFence::Timeline &,
            const XeZebin::Image &, const XeContext::LiveContext &,
            const XeMemory::Handle (&)[6], const XeDispatch::Policy &,
            XeContext::ExecutionBackend, OwnerInspection);
    Binding(const Binding &) = delete;
    Binding &operator=(const Binding &) = delete;
    MellowNativeGpuKernel::DriverOps operations();
    bool held() const { return execution_.contextHeld() || heldUses() != 0; }
    unsigned heldUses() const;
private:
    XeMemory::VirtualMemory &vm_;
    XeFence::Timeline &fence_;
    const XeZebin::Image &image_;
    XeContext::LiveContext context_;
    XeMemory::Handle handles_[6] {};
    XeDispatch::Policy policy_;
    XeContext::ExecutionBackend backend_;
    OwnerInspection owner_;
    XeContext::EvidenceExecution execution_;
    bool uses_[6] {}, attempted_ {}, closed_ {};
    MellowNativeGpuKernel::Job job_ {};
    bool releaseUses();
    bool stageBuffers(uint32_t count);
    bool sameJob(uint64_t, const MellowNativeGpuKernel::Job &) const;
    static MellowNativeGpuStatus identity(void *, uint64_t, MellowNativeGpuKernel::Identity &);
    static uint64_t now(void *);
    static MellowNativeGpuStatus submit(void *, uint64_t, uint32_t, uint32_t, uint64_t, MellowNativeGpuKernel::Job &);
    static MellowNativeGpuStatus poll(void *, uint64_t, const MellowNativeGpuKernel::Job &, MellowNativeGpuKernel::Observation &);
    static MellowNativeGpuStatus readback(void *, uint64_t, const MellowNativeGpuKernel::Job &, uint32_t *, uint32_t);
    static MellowNativeGpuStatus close(void *, uint64_t, const MellowNativeGpuKernel::Job &);
};
}
