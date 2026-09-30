// SPDX-License-Identifier: MIT
#include "NativeGpuSession.hpp"

namespace MellowNativeGpuKernel {
static bool valid(const Identity &i) {
    return i.serviceRegistryId && i.physicalPciRegistryId && i.generation &&
        (i.vendorId == 0x8086 || i.vendorId == 0x10de) && i.deviceId && i.deviceId <= UINT16_MAX &&
        (i.vendorId != 0x8086 || i.gmdArchitecture);
}
static bool same(const Identity &a, const Identity &b) {
    return a.serviceRegistryId == b.serviceRegistryId && a.physicalPciRegistryId == b.physicalPciRegistryId &&
        a.vendorId == b.vendorId && a.deviceId == b.deviceId &&
        a.gmdArchitecture == b.gmdArchitecture && a.gmdRelease == b.gmdRelease;
}
static bool same(const Job &a, const Job &b) {
    return a.generation == b.generation && a.id == b.id && a.nonce == b.nonce && a.count == b.count;
}
MellowNativeGpuStatus Session::initialize(uint64_t owner, DriverOps ops) {
    if (opened_ || !owner || !ops.identity || !ops.nowMicros || !ops.submit || !ops.poll || !ops.readback || !ops.close)
        return MellowNativeGpuStatusInvalid;
    Identity identity;
    auto result = ops.identity(ops.opaque, owner, identity);
    if (result != MellowNativeGpuStatusOk) return result;
    const uint64_t now = ops.nowMicros(ops.opaque);
    if (!valid(identity) || !now) return MellowNativeGpuStatusWrongIdentity;
    identity_ = identity; owner_ = owner; ops_ = ops; lastTime_ = now;
    opened_ = true; state_ = MellowNativeGpuStateReady;
    return MellowNativeGpuStatusOk;
}
MellowNativeGpuStatus Session::refresh() {
    Identity current;
    auto result = ops_.identity(ops_.opaque, owner_, current);
    if (result != MellowNativeGpuStatusOk) {
        state_ = MellowNativeGpuStateQuarantined;
        return result;
    }
    if (!valid(current) || !same(current, identity_)) {
        state_ = MellowNativeGpuStateQuarantined;
        return MellowNativeGpuStatusWrongIdentity;
    }
    if (current.generation != identity_.generation) {
        state_ = MellowNativeGpuStateQuarantined;
        return MellowNativeGpuStatusStale;
    }
    const uint64_t now = ops_.nowMicros(ops_.opaque);
    if (!now || now < lastTime_) {
        state_ = MellowNativeGpuStateQuarantined;
        return MellowNativeGpuStatusQuarantined;
    }
    lastTime_ = now;
    return MellowNativeGpuStatusOk;
}
bool Session::matches(const MellowNativeGpuRequest &request) const {
    return request.generation == identity_.generation && request.jobId == job_.id &&
        request.nonce == job_.nonce && request.count == job_.count;
}
void Session::fill(uint32_t selector, uint64_t correlation, MellowNativeGpuReply &reply) const {
    reply = {};
    reply.version = MELLOW_NATIVE_GPU_ABI_VERSION; reply.size = sizeof(reply);
    reply.state = state_; reply.correlation = correlation;
    reply.serviceRegistryId = identity_.serviceRegistryId;
    reply.physicalPciRegistryId = identity_.physicalPciRegistryId;
    reply.generation = identity_.generation;
    reply.vendorId = identity_.vendorId; reply.deviceId = identity_.deviceId;
    reply.gmdArchitecture = identity_.gmdArchitecture; reply.gmdRelease = identity_.gmdRelease;
    reply.maxWords = MELLOW_NATIVE_GPU_MAX_WORDS; reply.sampledMicros = sampledMicros_;
    if (selector != MellowNativeGpuQuery) {
        reply.jobId = job_.id; reply.nonce = job_.nonce; reply.count = job_.count;
        reply.fenceSequence = fenceSequence_;
    }
}
MellowNativeGpuStatus Session::poll() {
    if (state_ == MellowNativeGpuStateCompleted) return MellowNativeGpuStatusOk;
    if (state_ == MellowNativeGpuStateFailed) return terminalStatus_;
    if (state_ == MellowNativeGpuStateQuarantined) return MellowNativeGpuStatusQuarantined;
    if (lastTime_ >= deadline_) {
        state_ = MellowNativeGpuStateFailed; terminalStatus_ = MellowNativeGpuStatusTimeout;
        return MellowNativeGpuStatusTimeout;
    }
    Observation observation;
    auto result = ops_.poll(ops_.opaque, owner_, job_, observation);
    if (result == MellowNativeGpuStatusOk) {
        if (!same(observation.job, job_) || !observation.acquireOrdered ||
            observation.sequence != MELLOW_NATIVE_GPU_FENCE_SEQUENCE ||
            !observation.sampledMicros || observation.sampledMicros < lastTime_) {
            state_ = MellowNativeGpuStateQuarantined;
            return MellowNativeGpuStatusQuarantined;
        }
        if (observation.sampledMicros >= deadline_) {
            state_ = MellowNativeGpuStateFailed; terminalStatus_ = MellowNativeGpuStatusTimeout;
            return MellowNativeGpuStatusTimeout;
        }
        fenceSequence_ = observation.sequence; sampledMicros_ = observation.sampledMicros;
        lastTime_ = observation.sampledMicros; state_ = MellowNativeGpuStateCompleted;
        return MellowNativeGpuStatusOk;
    }
    // Elapsed time never cancels accepted/unknown hardware work. A timeout has
    // no readback permission; close still requires the driver's real retirement.
    if (result == MellowNativeGpuStatusPending || result == MellowNativeGpuStatusBusy) {
        return result;
    }
    state_ = result == MellowNativeGpuStatusTimeout ? MellowNativeGpuStateFailed : MellowNativeGpuStateQuarantined;
    terminalStatus_ = result;
    return result;
}
MellowNativeGpuStatus Session::close() {
    if (closed_) return MellowNativeGpuStatusOk;
    if (!opened_) return MellowNativeGpuStatusUnavailable;
    auto result = ops_.close(ops_.opaque, owner_, job_);
    if (result != MellowNativeGpuStatusOk) {
        state_ = MellowNativeGpuStateQuarantined;
        return result;
    }
    closed_ = true; state_ = MellowNativeGpuStateClosed;
    return MellowNativeGpuStatusOk;
}
MellowNativeGpuStatus Session::call(uint32_t selector, const MellowNativeGpuRequest &request, MellowNativeGpuReply &reply) {
    fill(selector, request.correlation, reply);
    auto finish = [&](MellowNativeGpuStatus status) {
        fill(selector, request.correlation, reply); reply.status = status; return status;
    };
    if (request.version != MELLOW_NATIVE_GPU_ABI_VERSION || request.size != sizeof(request) ||
        !request.correlation || request.correlation <= lastCorrelation_ || request.reserved[0] || request.reserved[1] ||
        selector > MellowNativeGpuCloseEvidence) return finish(MellowNativeGpuStatusInvalid);
    if (!opened_ || closed_) return finish(MellowNativeGpuStatusUnavailable);
    if (selector == MellowNativeGpuQuery) {
        if (request.generation || request.jobId || request.nonce || request.count || request.timeoutMicros)
            return finish(MellowNativeGpuStatusInvalid);
    } else if (selector == MellowNativeGpuSubmitEvidence) {
        if (request.generation != identity_.generation) return finish(MellowNativeGpuStatusStale);
        if (request.jobId || !request.nonce || !request.count || request.count > MELLOW_NATIVE_GPU_MAX_WORDS ||
            request.timeoutMicros < MELLOW_NATIVE_GPU_MIN_TIMEOUT_US || request.timeoutMicros > MELLOW_NATIVE_GPU_MAX_TIMEOUT_US)
            return finish(MellowNativeGpuStatusInvalid);
        if (attempted_) return finish(MellowNativeGpuStatusBusy);
    } else if (!attempted_ || request.timeoutMicros || !matches(request)) {
        return finish(request.generation != identity_.generation ? MellowNativeGpuStatusStale : MellowNativeGpuStatusInvalid);
    }
    lastCorrelation_ = request.correlation;
    // Cleanup must remain available after identity/epoch loss. It operates on
    // the stored original owner/job; the driver must prove actual retirement.
    if (selector == MellowNativeGpuCloseEvidence) return finish(close());
    if (state_ == MellowNativeGpuStateQuarantined) return finish(MellowNativeGpuStatusQuarantined);
    auto fresh = refresh();
    if (fresh != MellowNativeGpuStatusOk) return finish(fresh);
    if (selector == MellowNativeGpuQuery) return finish(MellowNativeGpuStatusOk);
    if (selector == MellowNativeGpuSubmitEvidence) {
        if (request.timeoutMicros > UINT64_MAX - lastTime_) return finish(MellowNativeGpuStatusInvalid);
        deadline_ = lastTime_ + request.timeoutMicros; attempted_ = true;
        job_ = {identity_.generation, 0, request.nonce, request.count};
        Job returned = job_;
        auto submitted = ops_.submit(ops_.opaque, owner_, request.nonce, request.count, deadline_, returned);
        // A returned wrong token does not authorize releasing unknown work.
        job_.id = returned.id;
        if (!same(returned, job_) || !job_.id) {
            state_ = MellowNativeGpuStateQuarantined;
            return finish(MellowNativeGpuStatusQuarantined);
        }
        if (submitted != MellowNativeGpuStatusOk && submitted != MellowNativeGpuStatusPending && submitted != MellowNativeGpuStatusBusy) {
            state_ = MellowNativeGpuStateQuarantined;
            return finish(submitted);
        }
        state_ = MellowNativeGpuStatePending;
        // Submission acceptance alone never becomes completed output.
        return finish(MellowNativeGpuStatusPending);
    }
    auto result = poll();
    if (selector != MellowNativeGpuReadEvidence || result != MellowNativeGpuStatusOk) return finish(result);
    fill(selector, request.correlation, reply);
    result = ops_.readback(ops_.opaque, owner_, job_, reply.output, job_.count);
    if (result != MellowNativeGpuStatusOk) {
        state_ = MellowNativeGpuStateQuarantined;
        return finish(result); // Erases any partial readback before replying.
    }
    // No expected-value arithmetic exists on the kernel/provider path.
    for (uint32_t i = job_.count; i < MELLOW_NATIVE_GPU_MAX_WORDS; ++i) reply.output[i] = 0;
    reply.status = MellowNativeGpuStatusOk;
    return MellowNativeGpuStatusOk;
}
}
