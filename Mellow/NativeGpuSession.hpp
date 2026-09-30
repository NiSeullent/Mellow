// SPDX-License-Identifier: MIT
#pragma once
#include "NativeGpuABI.h"

namespace MellowNativeGpuKernel {
struct Identity {
    uint64_t serviceRegistryId {}, physicalPciRegistryId {}, generation {};
    uint32_t vendorId {}, deviceId {}, gmdArchitecture {}, gmdRelease {};
};
struct Job {
    uint64_t generation {}, id {};
    uint32_t nonce {}, count {};
};
struct Observation {
    Job job {};
    uint64_t sequence {}, sampledMicros {};
    // Supplied only by the trusted kernel implementation after observing the
    // actual GPU-written fence with the required device/CPU acquire ordering.
    bool acquireOrdered {};
};
// Native driver boundary, not user-client data or simulated GPU readiness.
// The physical owner implements all callbacks from retained hardware resources.
// Every operation is serialized with reset/VM/context/IRQ handling by its owner.
struct DriverOps {
    void *opaque {};
    MellowNativeGpuStatus (*identity)(void *, uint64_t clientOwner, Identity &) {};
    uint64_t (*nowMicros)(void *) {};
    // Reserves a unique job before any possible publication. Unknown acceptance
    // or partial failures keep its backing until close establishes quiescence.
    MellowNativeGpuStatus (*submit)(void *, uint64_t, uint32_t nonce, uint32_t count,
                                    uint64_t absoluteDeadline, Job &) {};
    MellowNativeGpuStatus (*poll)(void *, uint64_t, const Job &, Observation &) {};
    // Must revalidate exact output backing/fence and perform actual CPU DMA
    // synchronization. The words come from GPU output, never an expected oracle.
    MellowNativeGpuStatus (*readback)(void *, uint64_t, const Job &, uint32_t *, uint32_t) {};
    // Actual context/GT quiescence and retirement, including uncertain submits.
    // Non-Ok retains every possibly live resource, even after timeout/client death.
    MellowNativeGpuStatus (*close)(void *, uint64_t, const Job &) {};
};

// Own fixed evidence-job protocol engine. No allocation, hardware impersonation,
// or destructor cleanup. Keep this and its driver alive until close() returns Ok.
class Session {
public:
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    MellowNativeGpuStatus initialize(uint64_t clientOwner, DriverOps);
    MellowNativeGpuStatus call(uint32_t selector, const MellowNativeGpuRequest &, MellowNativeGpuReply &);
    MellowNativeGpuStatus close();
    bool held() const { return opened_ && !closed_; }
    MellowNativeGpuState state() const { return state_; }
    const Identity &identity() const { return identity_; }
private:
    DriverOps ops_ {};
    Identity identity_ {};
    Job job_ {};
    uint64_t owner_ {}, lastTime_ {}, deadline_ {}, lastCorrelation_ {}, fenceSequence_ {}, sampledMicros_ {};
    bool opened_ {}, attempted_ {}, closed_ {};
    MellowNativeGpuState state_ {MellowNativeGpuStateCold};
    MellowNativeGpuStatus terminalStatus_ {MellowNativeGpuStatusQuarantined};
    MellowNativeGpuStatus refresh();
    MellowNativeGpuStatus poll();
    bool matches(const MellowNativeGpuRequest &) const;
    void fill(uint32_t selector, uint64_t correlation, MellowNativeGpuReply &) const;
};
}
