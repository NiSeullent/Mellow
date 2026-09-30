// SPDX-License-Identifier: MIT
#include "NativeGpuIOKit.hpp"
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
static_assert(sizeof(void *) == 8, "Native GPU ABI requires a 64-bit Darwin client");
#endif

// Public IPC contracts; the selectors and structures above are Mellow-owned.
// https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOUserClient.h
// https://github.com/apple-oss-distributions/IOKitUser/blob/main/IOKitLib.h
namespace MellowNativeGpuIOKit {
namespace {
using Clock = std::chrono::steady_clock;
bool failure(Error &out, ErrorCode code, const char *message,
             int32_t native = 0, uint32_t driver = 0) {
    out = {code, native, driver, message}; return false;
}
#if defined(__APPLE__)
bool target(const Identity &id) {
    return id.serviceRegistryId && id.physicalPciRegistryId &&
        id.serviceRegistryId != id.physicalPciRegistryId &&
        id.vendorId == 0x8086 && id.deviceId == 0x7d41 &&
        id.gmdArchitecture == 12 && id.gmdRelease == 70;
}
#endif
bool sameJob(const Job &a, const Job &b) {
    return a.generation && a.id && a.generation == b.generation && a.id == b.id &&
        a.nonce == b.nonce && a.count == b.count;
}
bool budget(uint64_t micros) {
    return micros >= MELLOW_NATIVE_GPU_MIN_TIMEOUT_US && micros <= MELLOW_NATIVE_GPU_MAX_TIMEOUT_US;
}
bool driverFailure(const MellowNativeGpuReply &reply, Error &error) {
    switch (reply.status) {
        case MellowNativeGpuStatusInvalid: return failure(error, ErrorCode::InvalidArgument, "Kernel rejected the fixed evidence request", 0, reply.status);
        case MellowNativeGpuStatusUnavailable: return failure(error, ErrorCode::Unavailable, "Native GPU owner is unavailable", 0, reply.status);
        case MellowNativeGpuStatusBusy: return failure(error, ErrorCode::Busy, "Native GPU job or teardown is busy", 0, reply.status);
        case MellowNativeGpuStatusTimeout: return failure(error, ErrorCode::Timeout, "Native GPU job timed out; completion cannot be read", 0, reply.status);
        case MellowNativeGpuStatusQuarantined: return failure(error, ErrorCode::Quarantined, "Native GPU resources remain quarantined", 0, reply.status);
        case MellowNativeGpuStatusStale: return failure(error, ErrorCode::Stale, "Native GPU reset generation or job is stale", 0, reply.status);
        case MellowNativeGpuStatusWrongIdentity: return failure(error, ErrorCode::WrongIdentity, "Native GPU identity differs from the selected provider", 0, reply.status);
        default: return failure(error, ErrorCode::Protocol, "Unexpected native GPU response status", 0, reply.status);
    }
}
}

struct Transport::Impl {
    mutable std::mutex mutex;
    Identity expected {};
    uint64_t generation {}, nextCorrelation {1};
    Job job {};
    bool connected {}, attempted {}, completed {}, readForbidden {}, poisoned {}, closed {};
    Clock::time_point jobDeadline {};
#if defined(__APPLE__)
    io_connect_t connection {IO_OBJECT_NULL};
#endif
    void disconnectLocked() noexcept {
#if defined(__APPLE__)
        if (connection != IO_OBJECT_NULL) { IOServiceClose(connection); connection = IO_OBJECT_NULL; }
#endif
        connected = false; completed = false; readForbidden = true; poisoned = true;
    }
    bool active(Error &error) const {
        if (!connected) return failure(error, ErrorCode::Unavailable, "No native GPU connection is open");
        if (poisoned) return failure(error, ErrorCode::Stale, "Native GPU connection is invalid; disconnect before selecting a new owner");
        return true;
    }
    bool identify(const MellowNativeGpuReply &reply, Error &error) {
        if (reply.serviceRegistryId != expected.serviceRegistryId ||
            reply.physicalPciRegistryId != expected.physicalPciRegistryId ||
            reply.vendorId != expected.vendorId || reply.deviceId != expected.deviceId ||
            reply.gmdArchitecture != expected.gmdArchitecture || reply.gmdRelease != expected.gmdRelease) {
            poisoned = true; completed = false; readForbidden = true;
            return failure(error, ErrorCode::WrongIdentity, "Native GPU response came from another physical provider");
        }
        return true;
    }
    bool call(uint32_t selector, MellowNativeGpuRequest request, MellowNativeGpuReply &reply, Error &error) {
        reply = {};
        if (!active(error)) return false;
        if (!nextCorrelation || nextCorrelation == std::numeric_limits<uint64_t>::max()) {
            poisoned = true; readForbidden = true;
            return failure(error, ErrorCode::Protocol, "Native GPU request correlation counter exhausted");
        }
        request.version = MELLOW_NATIVE_GPU_ABI_VERSION; request.size = sizeof(request);
        request.correlation = nextCorrelation++;
#if defined(__APPLE__)
        size_t length = sizeof(reply);
        const kern_return_t result = IOConnectCallStructMethod(connection, selector,
            &request, sizeof(request), &reply, &length);
        if (result != KERN_SUCCESS) {
            poisoned = true; completed = false; readForbidden = true; reply = {};
            return failure(error, ErrorCode::Transport, "Native GPU IOKit method failed; acceptance may be unknown", result);
        }
        if (length != sizeof(reply) || reply.size != sizeof(reply) ||
            reply.version != MELLOW_NATIVE_GPU_ABI_VERSION || reply.correlation != request.correlation ||
            reply.status > MellowNativeGpuStatusWrongIdentity || reply.state > MellowNativeGpuStateQuarantined ||
            reply.maxWords != MELLOW_NATIVE_GPU_MAX_WORDS || reply.reserved0 ||
            reply.reserved[0] || reply.reserved[1] || reply.reserved[2] ||
            reply.fenceSequence > MELLOW_NATIVE_GPU_FENCE_SEQUENCE) {
            poisoned = true; completed = false; readForbidden = true;
            return failure(error, ErrorCode::Protocol, "Native GPU reply ABI, bounds or correlation is invalid");
        }
        if (!identify(reply, error)) return false;
        const bool readable = selector == MellowNativeGpuReadEvidence &&
            reply.status == MellowNativeGpuStatusOk && reply.state == MellowNativeGpuStateCompleted;
        const uint32_t populated = readable ? reply.count : 0;
        if (populated > MELLOW_NATIVE_GPU_MAX_WORDS ||
            std::any_of(reply.output + populated, reply.output + MELLOW_NATIVE_GPU_MAX_WORDS,
                        [](uint32_t value) { return value != 0; })) {
            poisoned = true; completed = false; readForbidden = true;
            return failure(error, ErrorCode::Protocol, "Native GPU reply exposes output outside a completed bounded read");
        }
        if (generation && reply.generation != generation) {
            poisoned = true; completed = false; readForbidden = true;
            return failure(error, ErrorCode::Stale, "Native GPU reset generation changed");
        }
        return true;
#else
        (void)selector; (void)request;
        return failure(error, ErrorCode::Unavailable, "Native GPU IOKit transport requires Darwin");
#endif
    }
    MellowNativeGpuRequest jobRequest() const {
        MellowNativeGpuRequest request {};
        request.generation = job.generation; request.jobId = job.id;
        request.nonce = job.nonce; request.count = job.count;
        return request;
    }
    bool matchJob(const MellowNativeGpuReply &reply, Error &error) {
        const Job observed {reply.generation, reply.jobId, reply.fenceSequence, reply.nonce, reply.count};
        if (!sameJob(observed, job)) {
            poisoned = true; completed = false; readForbidden = true;
            return failure(error, ErrorCode::Stale, "Native GPU reply does not match this connection's job");
        }
        return true;
    }
    bool localDeadline(Error &error) {
        if (Clock::now() < jobDeadline) return true;
        readForbidden = true; completed = false;
        return failure(error, ErrorCode::Timeout, "Native GPU completion deadline expired; resources still require real teardown");
    }
    Progress pollLocked(Job &out, Error &error) {
        if (!active(error)) return Progress::Failed;
        if (!sameJob(out, job) || closed) {
            failure(error, ErrorCode::InvalidState, "Poll requires this connection's active evidence job"); return Progress::Failed;
        }
        if (readForbidden) {
            failure(error, ErrorCode::InvalidState, "Failed or timed-out native GPU content is not readable"); return Progress::Failed;
        }
        if (completed) { out.fenceSequence = job.fenceSequence; return Progress::Completed; }
        if (!localDeadline(error)) { out.fenceSequence = 0; return Progress::Failed; }
        MellowNativeGpuReply reply {};
        if (!call(MellowNativeGpuPollEvidence, jobRequest(), reply, error) || !matchJob(reply, error)) return Progress::Failed;
        if ((reply.status == MellowNativeGpuStatusPending || reply.status == MellowNativeGpuStatusBusy) &&
            reply.state == MellowNativeGpuStatePending && !reply.fenceSequence) {
            out.fenceSequence = 0; return Progress::Pending;
        }
        if (reply.status == MellowNativeGpuStatusOk && reply.state == MellowNativeGpuStateCompleted &&
            reply.fenceSequence == MELLOW_NATIVE_GPU_FENCE_SEQUENCE) {
            if (!localDeadline(error)) { out.fenceSequence = 0; return Progress::Failed; }
            completed = true; job.fenceSequence = reply.fenceSequence; out.fenceSequence = job.fenceSequence;
            return Progress::Completed;
        }
        readForbidden = true; completed = false; out.fenceSequence = 0;
        driverFailure(reply, error); return Progress::Failed;
    }
};

Transport::Transport() : impl_(new Impl) {}
Transport::~Transport() { disconnect(); }

bool Transport::open(const Identity &identity, Error &error) {
    error = {};
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->connected) return failure(error, ErrorCode::InvalidState, "Disconnect the current native GPU owner before opening another");
#if defined(__APPLE__)
    if (!target(identity)) return failure(error, ErrorCode::InvalidArgument, "Explicit distinct service/physical registry IDs and physical 8086:7D41 / GMD12.70 are required");
    auto matching = IORegistryEntryIDMatching(identity.serviceRegistryId);
    if (!matching) return failure(error, ErrorCode::Unavailable, "Unable to construct exact native GPU registry match");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (service == IO_OBJECT_NULL) return failure(error, ErrorCode::Unavailable, "Selected native GPU service is absent");
    uint64_t registryId = 0;
    const bool exact = IOObjectConformsTo(service, MELLOW_NATIVE_GPU_SERVICE) &&
        IORegistryEntryGetRegistryEntryID(service, &registryId) == KERN_SUCCESS && registryId == identity.serviceRegistryId;
    if (!exact) { IOObjectRelease(service); return failure(error, ErrorCode::WrongIdentity, "Selected registry object is not the exact native GPU service"); }
    io_connect_t connection = IO_OBJECT_NULL;
    const kern_return_t opened = IOServiceOpen(service, mach_task_self(), MELLOW_NATIVE_GPU_CONNECT_TYPE, &connection);
    IOObjectRelease(service);
    if (opened != KERN_SUCCESS || connection == IO_OBJECT_NULL) {
        if (connection != IO_OBJECT_NULL) IOServiceClose(connection);
        return failure(error, ErrorCode::Transport, "Native GPU IOServiceOpen failed", opened);
    }
    impl_->connection = connection; impl_->connected = true; impl_->expected = identity;
    impl_->generation = 0; impl_->nextCorrelation = 1; impl_->job = {};
    impl_->attempted = impl_->completed = impl_->readForbidden = impl_->poisoned = impl_->closed = false;
    MellowNativeGpuRequest request {}; MellowNativeGpuReply reply {};
    if (!impl_->call(MellowNativeGpuQuery, request, reply, error)) { impl_->disconnectLocked(); return false; }
    if (reply.jobId || reply.nonce || reply.count || reply.fenceSequence) {
        impl_->disconnectLocked(); return failure(error, ErrorCode::Protocol, "Initial native GPU query included unowned job data");
    }
    if (reply.status != MellowNativeGpuStatusOk) {
        impl_->disconnectLocked(); return driverFailure(reply, error);
    }
    if (reply.state != MellowNativeGpuStateReady || !reply.generation) {
        impl_->disconnectLocked(); return failure(error, ErrorCode::Unavailable, "Native GPU owner is not ready in a live reset generation");
    }
    impl_->generation = reply.generation; return true;
#else
    (void)identity;
    return failure(error, ErrorCode::Unavailable, "Native GPU IOKit transport requires Darwin");
#endif
}

bool Transport::query(DeviceInfo &info, Error &error) {
    info = {}; error = {};
    std::lock_guard<std::mutex> guard(impl_->mutex);
    MellowNativeGpuRequest request {}; MellowNativeGpuReply reply {};
    if (!impl_->call(MellowNativeGpuQuery, request, reply, error)) return false;
    if (reply.jobId || reply.nonce || reply.count || reply.fenceSequence || !reply.generation) {
        impl_->poisoned = true; impl_->readForbidden = true;
        return failure(error, ErrorCode::Protocol, "Native GPU query included invalid job/generation data");
    }
    if (reply.status != MellowNativeGpuStatusOk) {
        impl_->readForbidden = true; impl_->completed = false;
        return driverFailure(reply, error);
    }
    info = {impl_->expected, reply.generation, reply.state, reply.maxWords}; return true;
}

bool Transport::submit(uint32_t nonce, uint32_t count, uint64_t timeoutMicros, Job &out, Error &error) {
    out = {}; error = {};
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->active(error)) return false;
    if (!nonce || !count || count > MELLOW_NATIVE_GPU_MAX_WORDS || !budget(timeoutMicros))
        return failure(error, ErrorCode::InvalidArgument, "Evidence nonce/count/timeout is outside the fixed ABI bounds");
    if (impl_->attempted) return failure(error, ErrorCode::InvalidState, "This native GPU connection has already attempted its one job");
    impl_->attempted = true; impl_->jobDeadline = Clock::now() + std::chrono::microseconds(timeoutMicros);
    MellowNativeGpuRequest request {}; request.generation = impl_->generation;
    request.nonce = nonce; request.count = count; request.timeoutMicros = timeoutMicros;
    MellowNativeGpuReply reply {};
    if (!impl_->call(MellowNativeGpuSubmitEvidence, request, reply, error)) return false;
    const bool acceptedStatus = reply.status == MellowNativeGpuStatusOk || reply.status == MellowNativeGpuStatusPending;
    if ((reply.jobId || acceptedStatus) && (reply.nonce != nonce || reply.count != count)) {
        impl_->poisoned = true; impl_->readForbidden = true;
        return failure(error, ErrorCode::Protocol, "Native GPU submission nonce/count was not echoed");
    }
    if (reply.jobId) {
        impl_->job = {reply.generation, reply.jobId, 0, nonce, count}; out = impl_->job;
    }
    if (!acceptedStatus) { impl_->readForbidden = true; return driverFailure(reply, error); }
    const bool pending = (reply.status == MellowNativeGpuStatusOk || reply.status == MellowNativeGpuStatusPending) &&
        reply.state == MellowNativeGpuStatePending && !reply.fenceSequence;
    const bool complete = reply.status == MellowNativeGpuStatusOk && reply.state == MellowNativeGpuStateCompleted &&
        reply.fenceSequence == MELLOW_NATIVE_GPU_FENCE_SEQUENCE;
    if (!reply.jobId || (!pending && !complete)) {
        impl_->readForbidden = true; return driverFailure(reply, error);
    }
    if (!impl_->localDeadline(error)) return false;
    if (complete) { impl_->completed = true; impl_->job.fenceSequence = reply.fenceSequence; out = impl_->job; }
    return true;
}

Progress Transport::poll(Job &job, Error &error) {
    error = {};
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->pollLocked(job, error);
}

bool Transport::wait(Job &job, uint64_t timeoutMicros, Error &error) {
    error = {};
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        if (!impl_->active(error)) return false;
    }
    if (!budget(timeoutMicros)) return failure(error, ErrorCode::InvalidArgument, "Wait timeout is outside the fixed ABI bounds");
    const auto deadline = Clock::now() + std::chrono::microseconds(timeoutMicros);
    for (;;) {
        if (Clock::now() >= deadline) {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            if (sameJob(job, impl_->job)) { impl_->readForbidden = true; impl_->completed = false; job.fenceSequence = 0; }
            return failure(error, ErrorCode::Timeout, "Native GPU wait expired; resources still require real teardown");
        }
        const Progress result = poll(job, error);
        if (result == Progress::Completed) {
            if (Clock::now() < deadline) return true;
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->readForbidden = true; impl_->completed = false; job.fenceSequence = 0;
            return failure(error, ErrorCode::Timeout, "Native GPU wait completed after its local deadline");
        }
        if (result == Progress::Failed) return false;
        const auto remaining = deadline - Clock::now();
        if (remaining > Clock::duration::zero())
            std::this_thread::sleep_for(std::min(remaining, std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(2))));
    }
}

bool Transport::read(const Job &job, std::vector<uint32_t> &output, Error &error) {
    output.clear(); error = {};
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->active(error)) return false;
    if (!sameJob(job, impl_->job) || impl_->closed || impl_->readForbidden || !impl_->completed ||
        job.fenceSequence != MELLOW_NATIVE_GPU_FENCE_SEQUENCE || impl_->job.fenceSequence != job.fenceSequence)
        return failure(error, ErrorCode::InvalidState, "Read requires this connection's correlated successfully completed evidence job");
    MellowNativeGpuReply reply {};
    if (!impl_->call(MellowNativeGpuReadEvidence, impl_->jobRequest(), reply, error) || !impl_->matchJob(reply, error)) return false;
    if (reply.status != MellowNativeGpuStatusOk) {
        impl_->readForbidden = true; impl_->completed = false; return driverFailure(reply, error);
    }
    if (reply.state != MellowNativeGpuStateCompleted || reply.fenceSequence != job.fenceSequence) {
        impl_->poisoned = true; impl_->readForbidden = true; impl_->completed = false;
        return failure(error, ErrorCode::Protocol, "Native GPU read has no matching completed fence");
    }
    try { output.assign(reply.output, reply.output + reply.count); return true; }
    catch (const std::exception &) { output.clear(); return failure(error, ErrorCode::Execution, "Unable to allocate copied native GPU readback storage"); }
}

bool Transport::closeEvidence(const Job &job, Error &error) {
    error = {};
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->active(error)) return false;
    if (!sameJob(job, impl_->job) || impl_->closed)
        return failure(error, ErrorCode::InvalidState, "Close requires this connection's known unclosed evidence job");
    MellowNativeGpuReply reply {};
    if (!impl_->call(MellowNativeGpuCloseEvidence, impl_->jobRequest(), reply, error) || !impl_->matchJob(reply, error)) return false;
    if (reply.status != MellowNativeGpuStatusOk) return driverFailure(reply, error);
    if (reply.state != MellowNativeGpuStateClosed) {
        impl_->poisoned = true; impl_->readForbidden = true;
        return failure(error, ErrorCode::Protocol, "Kernel did not confirm native GPU job closure");
    }
    impl_->closed = true; impl_->completed = false; impl_->readForbidden = true; return true;
}
void Transport::disconnect() noexcept {
    std::lock_guard<std::mutex> guard(impl_->mutex); impl_->disconnectLocked();
}
bool Transport::isOpen() const {
    std::lock_guard<std::mutex> guard(impl_->mutex); return impl_->connected && !impl_->poisoned;
}
}
