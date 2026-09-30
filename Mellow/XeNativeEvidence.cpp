// SPDX-License-Identifier: MIT
#include "XeNativeEvidence.hpp"
#include <libkern/libkern.h>
namespace XeNativeEvidence {
namespace NG = MellowNativeGpuKernel;
static MellowNativeGpuStatus convert(XeContext::ExecutionStatus status) {
    switch (status) {
    case XeContext::ExecutionStatus::Ok: return MellowNativeGpuStatusOk;
    case XeContext::ExecutionStatus::Pending: return MellowNativeGpuStatusPending;
    case XeContext::ExecutionStatus::Busy: return MellowNativeGpuStatusBusy;
    case XeContext::ExecutionStatus::Timeout: return MellowNativeGpuStatusTimeout;
    case XeContext::ExecutionStatus::Invalid: return MellowNativeGpuStatusInvalid;
    case XeContext::ExecutionStatus::Unavailable: return MellowNativeGpuStatusUnavailable;
    case XeContext::ExecutionStatus::Quarantined: return MellowNativeGpuStatusQuarantined;
    }
    return MellowNativeGpuStatusQuarantined;
}
Binding::Binding(XeMemory::VirtualMemory &vm, XeGuC::Transport &guc, XeFence::Timeline &fence,
                 const XeZebin::Image &image, const XeContext::LiveContext &context,
                 const XeMemory::Handle (&handles)[6], const XeDispatch::Policy &policy,
                 XeContext::ExecutionBackend backend, OwnerInspection owner)
    : vm_(vm), fence_(fence), image_(image), context_(context), policy_(policy), backend_(backend), owner_(owner),
      execution_(vm, guc, fence, backend) {
    for (size_t i = 0; i < 6; ++i) handles_[i] = handles[i];
}
NG::DriverOps Binding::operations() { return {this, identity, now, submit, poll, readback, close}; }
unsigned Binding::heldUses() const { unsigned count = 0; for (bool held : uses_) count += held; return count; }
bool Binding::releaseUses() {
    bool released = true;
    for (size_t i = 6; i > 0; --i) if (uses_[i - 1]) {
        if (vm_.releaseUse(context_.owner, handles_[i - 1]) == XeMemory::Status::Ok) uses_[i - 1] = false;
        else released = false;
    }
    return released;
}
bool Binding::stageBuffers(uint32_t count) {
    if (!count || count > MELLOW_NATIVE_GPU_MAX_WORDS || !uses_[4] || !uses_[5] ||
        !backend_.admitted || !backend_.admitted(backend_.opaque, context_, policy_) ||
        !backend_.freshStopped || !backend_.freshStopped(backend_.opaque, context_)) return false;
    XeFence::Observation initial;
    if (fence_.lastPublished() != 0 || fence_.observe(context_.epoch, initial) != XeFence::Status::Ok ||
        !initial.acquireOrdered || initial.raw != 0 || initial.sequence != 0 ||
        initial.owner != context_.owner || initial.context != context_.id || initial.epoch != context_.epoch ||
        initial.engineClass != 0 || initial.instance != 0 || initial.ggtt != fence_.address()) return false;
    const uint64_t bytes = uint64_t(count) * sizeof(uint32_t);
    const auto *input = vm_.inspect(context_.owner, handles_[4]);
    const auto *output = vm_.inspect(context_.owner, handles_[5]);
    if (!input || !output || handles_[4].slot == handles_[5].slot ||
        input->state != XeMemory::State::Bound || output->state != XeMemory::State::Bound ||
        input->owner != context_.owner || output->owner != context_.owner ||
        input->generation != handles_[4].generation || output->generation != handles_[5].generation ||
        !input->activeUses || !output->activeUses || input->bytes < bytes || output->bytes < bytes ||
        !input->pin.cookie || !output->pin.cookie || !input->pin.dmaPages || !output->pin.dmaPages ||
        !input->pin.pageCount || !output->pin.pageCount ||
        input->bytes % XeMemory::PageSize || output->bytes % XeMemory::PageSize ||
        input->bytes / XeMemory::PageSize != input->pin.pageCount ||
        output->bytes / XeMemory::PageSize != output->pin.pageCount ||
        input->pin.cookie == output->pin.cookie || input->pin.dmaPages == output->pin.dmaPages) return false;
    // The data allocations must not alias any heap which stageHeaps will replace.
    // All six VM uses remain held on every failure, including partial DMA sync.
    for (size_t i = 0; i < 4; ++i) {
        const auto *heap = vm_.inspect(context_.owner, handles_[i]);
        if (!uses_[i] || !heap || heap->state != XeMemory::State::Bound ||
            handles_[i].slot == handles_[4].slot || handles_[i].slot == handles_[5].slot ||
            heap->pin.cookie == input->pin.cookie || heap->pin.cookie == output->pin.cookie) return false;
    }
    auto *inputCpu = static_cast<uint32_t *>(XeMemory::kernelBuffer(input->pin));
    void *outputCpu = XeMemory::kernelBuffer(output->pin);
    const uintptr_t inputAddress = reinterpret_cast<uintptr_t>(inputCpu);
    const uintptr_t outputAddress = reinterpret_cast<uintptr_t>(outputCpu);
    if (!inputCpu || !outputCpu || (inputAddress & (alignof(uint32_t) - 1)) ||
        inputAddress > UINTPTR_MAX - bytes || outputAddress > UINTPTR_MAX - bytes ||
        (inputAddress < outputAddress + bytes && outputAddress < inputAddress + bytes)) return false;
    // This deterministic input is the fixed ABI's owned input construction.
    // The output sentinel is initialization, never an expected-value oracle.
    for (uint32_t i = 0; i < count; ++i) inputCpu[i] = (i * 2654435761U) ^ 0xa5a55a5aU;
    memset(outputCpu, 0xcd, static_cast<size_t>(bytes));
    if (XeMemory::synchronizeForDevice(input->pin) != XeMemory::Status::Ok ||
        XeMemory::synchronizeForDevice(output->pin) != XeMemory::Status::Ok) return false;
    return true;
}
bool Binding::sameJob(uint64_t owner, const NG::Job &job) const {
    return attempted_ && owner == context_.owner && job.generation == job_.generation &&
        job.id == job_.id && job.nonce == job_.nonce && job.count == job_.count;
}
MellowNativeGpuStatus Binding::identity(void *opaque, uint64_t owner, NG::Identity &info) {
    auto &binding = *static_cast<Binding *>(opaque);
    info = {};
    if (binding.closed_ || owner != binding.context_.owner || !binding.owner_.identity ||
        !binding.owner_.nowMicros || !binding.backend_.nowMicros || !binding.backend_.admitted ||
        !binding.backend_.admitted(binding.backend_.opaque, binding.context_, binding.policy_))
        return MellowNativeGpuStatusUnavailable;
    const auto result = binding.owner_.identity(binding.owner_.opaque, owner, info);
    if (result != MellowNativeGpuStatusOk) return result;
    if (info.generation != binding.context_.epoch) return MellowNativeGpuStatusStale;
    if (info.vendorId != 0x8086 || info.deviceId != 0x7d41 || info.gmdArchitecture != 12 || info.gmdRelease != 70)
        return MellowNativeGpuStatusWrongIdentity;
    return MellowNativeGpuStatusOk;
}
uint64_t Binding::now(void *opaque) {
    auto &binding = *static_cast<Binding *>(opaque);
    return binding.owner_.nowMicros ? binding.owner_.nowMicros(binding.owner_.opaque) : 0;
}
MellowNativeGpuStatus Binding::submit(void *opaque, uint64_t owner, uint32_t nonce, uint32_t count,
                                    uint64_t deadline, NG::Job &job) {
    auto &binding = *static_cast<Binding *>(opaque);
    if (binding.attempted_ || binding.closed_ || owner != binding.context_.owner ||
        !nonce || !count || count > MELLOW_NATIVE_GPU_MAX_WORDS) return MellowNativeGpuStatusInvalid;
    NG::Identity actual;
    const auto ready = identity(opaque, owner, actual);
    if (ready != MellowNativeGpuStatusOk) return ready;
    const uint64_t current = now(opaque);
    if (!current || deadline <= current) return MellowNativeGpuStatusTimeout;
    binding.attempted_ = true;
    binding.job_ = {binding.context_.epoch, binding.context_.allocation, nonce, count};
    job = binding.job_;
    if (!job.id) return MellowNativeGpuStatusInvalid;
    // Independent holds preserve exact backing through CPU readback, including
    // after EvidenceExecution releases its scheduler uses on GPU completion.
    for (size_t i = 0; i < 6; ++i) {
        if (binding.vm_.retainUse(owner, binding.handles_[i]) != XeMemory::Status::Ok)
            return MellowNativeGpuStatusQuarantined;
        binding.uses_[i] = true;
    }
    if (!binding.stageBuffers(count)) return MellowNativeGpuStatusQuarantined;
    // Reinspect hardware identity/epoch after potentially blocking DMA copies.
    // EvidenceExecution independently rechecks admission, the empty stopped
    // context, held VM generations and fence before publishing its ring tail.
    const auto stagedReady = identity(opaque, owner, actual);
    if (stagedReady != MellowNativeGpuStatusOk) return stagedReady;
    if (!binding.backend_.freshStopped ||
        !binding.backend_.freshStopped(binding.backend_.opaque, binding.context_))
        return MellowNativeGpuStatusQuarantined;
    const uint64_t stagedTime = now(opaque);
    if (!stagedTime || stagedTime < current) return MellowNativeGpuStatusQuarantined;
    if (stagedTime >= deadline) return MellowNativeGpuStatusTimeout;
    return convert(binding.execution_.begin(binding.image_, binding.context_, binding.handles_, binding.policy_,
        nonce, count, binding.context_.depthStallWorkaround, stagedTime, deadline));
}
MellowNativeGpuStatus Binding::poll(void *opaque, uint64_t owner, const NG::Job &job, NG::Observation &observation) {
    auto &binding = *static_cast<Binding *>(opaque);
    observation = {};
    if (!binding.sameJob(owner, job) || binding.closed_) return MellowNativeGpuStatusInvalid;
    const auto result = convert(binding.execution_.poll(now(opaque)));
    if (result != MellowNativeGpuStatusOk) return result;
    XeFence::Observation fence;
    if (binding.fence_.observe(job.generation, fence) != XeFence::Status::Ok ||
        !fence.acquireOrdered || fence.raw != MELLOW_NATIVE_GPU_FENCE_SEQUENCE ||
        fence.owner != owner || fence.context != binding.context_.id || fence.epoch != job.generation ||
        fence.engineClass != 0 || fence.instance != 0 || fence.ggtt != binding.fence_.address())
        return MellowNativeGpuStatusQuarantined;
    observation.job = job;
    observation.sequence = fence.sequence;
    observation.acquireOrdered = fence.acquireOrdered;
    observation.sampledMicros = now(opaque);
    return MellowNativeGpuStatusOk;
}
MellowNativeGpuStatus Binding::readback(void *opaque, uint64_t owner, const NG::Job &job,
                                      uint32_t *words, uint32_t count) {
    auto &binding = *static_cast<Binding *>(opaque);
    if (!words || count != job.count || count > MELLOW_NATIVE_GPU_MAX_WORDS || !binding.sameJob(owner, job) ||
        binding.execution_.state() != XeContext::ExecutionState::Completed || !binding.uses_[5])
        return MellowNativeGpuStatusInvalid;
    NG::Observation observation;
    auto complete = poll(opaque, owner, job, observation);
    if (complete != MellowNativeGpuStatusOk) return complete;
    const auto *output = binding.vm_.inspect(owner, binding.handles_[5]);
    if (!output || (output->state != XeMemory::State::Bound && output->state != XeMemory::State::Retiring) || !output->activeUses ||
        !output->pin.cookie || !output->pin.dmaPages || !output->pin.pageCount || output->bytes < uint64_t(count) * 4)
        return MellowNativeGpuStatusQuarantined;
    if (XeMemory::synchronizeForCpu(output->pin) != XeMemory::Status::Ok) return MellowNativeGpuStatusQuarantined;
    const void *cpu = XeMemory::kernelBuffer(output->pin);
    if (!cpu) return MellowNativeGpuStatusQuarantined;
    NG::Identity actual;
    auto ready = identity(opaque, owner, actual);
    if (ready != MellowNativeGpuStatusOk) return ready;
    memcpy(words, cpu, size_t(count) * sizeof(uint32_t));
    return MellowNativeGpuStatusOk;
}
MellowNativeGpuStatus Binding::close(void *opaque, uint64_t owner, const NG::Job &job) {
    auto &binding = *static_cast<Binding *>(opaque);
    if (owner != binding.context_.owner || (binding.attempted_ && !binding.sameJob(owner, job)))
        return MellowNativeGpuStatusInvalid;
    if (binding.closed_) return MellowNativeGpuStatusOk;
    if (!binding.backend_.quiesced || !binding.backend_.quiesced(binding.backend_.opaque, binding.context_))
        return MellowNativeGpuStatusBusy;
    if (binding.attempted_ && binding.execution_.state() != XeContext::ExecutionState::Idle) {
        auto result = convert(binding.execution_.close());
        if (result != MellowNativeGpuStatusOk) return result;
    }
    if (binding.fence_.held() && binding.fence_.close() != XeFence::Status::Ok)
        return MellowNativeGpuStatusBusy;
    if (!binding.releaseUses()) return MellowNativeGpuStatusQuarantined;
    binding.closed_ = true;
    return MellowNativeGpuStatusOk;
}
}
