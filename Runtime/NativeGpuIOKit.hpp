// SPDX-License-Identifier: MIT
#pragma once
#include "../Mellow/NativeGpuABI.h"
#include <memory>
#include <string>
#include <vector>

// Explicit Mellow-owned Darwin transport for ONE fixed Xe evidence job.
// It is neither a general shader provider nor an Apple Metal plugin and makes
// no system Metal, WindowServer, display or unsupported-family support claim.
// Non-Apple hosts return Unavailable; no OpenCL/CGL/CPU fallback is attempted.
namespace MellowNativeGpuIOKit {
enum class ErrorCode {
    None, InvalidArgument, InvalidState, Unavailable, Transport, Protocol,
    WrongIdentity, Stale, Timeout, Busy, Quarantined, Execution
};
struct Error {
    ErrorCode code {ErrorCode::None};
    int32_t nativeCode {};
    uint32_t driverStatus {};
    std::string message;
};
struct Identity {
    uint64_t serviceRegistryId {}, physicalPciRegistryId {};
    uint32_t vendorId {0x8086}, deviceId {0x7d41};
    uint32_t gmdArchitecture {12}, gmdRelease {70};
};
struct DeviceInfo {
    Identity identity {};
    uint64_t generation {};
    uint32_t state {}, maxWords {};
};
struct Job {
    uint64_t generation {}, id {}, fenceSequence {};
    uint32_t nonce {}, count {};
};
enum class Progress { Pending, Completed, Failed };

class Transport {
public:
    Transport();
    ~Transport();
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    // IDs must be supplied explicitly. The exact registry service must conform
    // to MellowNativeGpu, and a verified ABI query must identify its physical
    // 8086:7D41 / GMD12.70 provider and a live nonzero reset generation.
    bool open(const Identity &, Error &);
    bool query(DeviceInfo &, Error &);
    // Kernel owns the fixed program and input-data construction. Clients supply
    // only nonce/count/relative time budget, never executable bytes or addresses.
    // Even an uncertain submission consumes this connection's one attempt.
    // A known failed job is returned so closeEvidence can request real teardown.
    bool submit(uint32_t nonce, uint32_t count, uint64_t timeoutMicros, Job &, Error &);
    Progress poll(Job &, Error &);
    bool wait(Job &, uint64_t timeoutMicros, Error &);
    // Clears output on every failure. Read requires previously correlated
    // completion and validates a fresh completed read reply before copying.
    bool read(const Job &, std::vector<uint32_t> &output, Error &);
    // A timeout does not prove quiescence. Failure retains the connection/job;
    // no cancellation, synthetic fence, implicit retry or resource reuse occurs.
    bool closeEvidence(const Job &, Error &);
    // Ends IPC ownership; kernel client-death handling owns any live/quarantined
    // resources. Disconnect itself is never interpreted as GPU completion.
    void disconnect() noexcept;
    bool isOpen() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Calls are serialized per instance. IOKit methods are synchronous and cannot be
// preempted by this client. wait bounds time BETWEEN calls; use an externally
// timed process for a hard wall-clock deadline covering a hung kernel/driver.
}
