// SPDX-License-Identifier: MIT
// Mellow-owned fixed evidence-job wire contract. This is not Apple's private
// IOAccelerator/Metal ABI and carries no system Metal or display capability.
#ifndef MELLOW_NATIVE_GPU_ABI_H
#define MELLOW_NATIVE_GPU_ABI_H
#include <stddef.h>
#include <stdint.h>

#define MELLOW_NATIVE_GPU_SERVICE "MellowNativeGpu"
#define MELLOW_NATIVE_GPU_ABI_VERSION 1U
#define MELLOW_NATIVE_GPU_CONNECT_TYPE 0x4d470001U
#define MELLOW_NATIVE_GPU_MAX_WORDS 256U
#define MELLOW_NATIVE_GPU_MIN_TIMEOUT_US 1000ULL
#define MELLOW_NATIVE_GPU_MAX_TIMEOUT_US 10000000ULL
#define MELLOW_NATIVE_GPU_FENCE_SEQUENCE 1ULL

enum MellowNativeGpuSelector {
    MellowNativeGpuQuery = 0,
    MellowNativeGpuSubmitEvidence = 1,
    MellowNativeGpuPollEvidence = 2,
    MellowNativeGpuReadEvidence = 3,
    MellowNativeGpuCloseEvidence = 4
};
enum MellowNativeGpuStatus {
    MellowNativeGpuStatusOk = 0,
    MellowNativeGpuStatusPending = 1,
    MellowNativeGpuStatusInvalid = 2,
    MellowNativeGpuStatusUnavailable = 3,
    MellowNativeGpuStatusBusy = 4,
    MellowNativeGpuStatusTimeout = 5,
    MellowNativeGpuStatusQuarantined = 6,
    MellowNativeGpuStatusStale = 7,
    MellowNativeGpuStatusWrongIdentity = 8
};
enum MellowNativeGpuState {
    MellowNativeGpuStateCold = 0,
    MellowNativeGpuStateReady = 1,
    MellowNativeGpuStatePending = 2,
    MellowNativeGpuStateCompleted = 3,
    MellowNativeGpuStateFailed = 4,
    MellowNativeGpuStateClosed = 5,
    MellowNativeGpuStateQuarantined = 6
};

// All fields use the native little-endian Darwin x86_64/arm64 wire layout.
// Zero initialize all reserved fields. correlation is nonzero and echoed for
// EVERY call and increases strictly within a connection; it is a replay/
// correlation token, not an authorization claim.
// Query: generation/jobId/nonce/count/timeoutMicros are zero.
// Submit: generation from Query, jobId zero, nonce nonzero, count 1..256,
// timeoutMicros 1000..10000000. The kernel derives an absolute deadline from
// its own monotonic clock. The program AND input-data construction belong to
// the trusted fixed evidence implementation, never to client executable bytes.
// Poll/Read/Close: exact generation/jobId/nonce/count from Submit; timeout zero.
// A connection admits at most one job. Close does not authorize another submit.
typedef struct MellowNativeGpuRequest {
    uint32_t version, size;
    uint64_t generation, jobId, correlation, timeoutMicros;
    uint32_t nonce, count;
    uint64_t reserved[2];
} MellowNativeGpuRequest;

// Registry IDs are actual IORegistry identities: the service and its physical
// PCI provider before spoofing. generation identifies the immutable reset epoch.
// Query echoes correlation; jobId/nonce/count/fenceSequence are zero.
// Job replies echo exact generation/jobId/nonce/count/correlation, including
// failure replies when the job is known. Fence is 0 until actual correlated
// completion, then exactly 1 for this EvidenceExecution model. Status/state
// alone cannot establish completion. Failed/timed-out results are never readable.
// output[] is populated ONLY by successful ReadEvidence in Completed state,
// after actual completion + CPU DMA synchronization; all unused words are zero.
// All other replies contain zero output[]. reserved0/reserved[] are always zero.
typedef struct MellowNativeGpuReply {
    uint32_t version, size, status, state;
    uint64_t serviceRegistryId, physicalPciRegistryId, generation, jobId;
    uint64_t correlation, fenceSequence, sampledMicros;
    uint32_t nonce, count, vendorId, deviceId;
    uint32_t gmdArchitecture, gmdRelease, maxWords, reserved0;
    uint32_t output[MELLOW_NATIVE_GPU_MAX_WORDS];
    uint64_t reserved[3];
} MellowNativeGpuReply;

#if defined(__cplusplus)
static_assert(sizeof(MellowNativeGpuRequest) == 64, "native GPU request layout");
static_assert(offsetof(MellowNativeGpuRequest, nonce) == 40, "native GPU nonce offset");
static_assert(sizeof(MellowNativeGpuReply) == 1152, "native GPU reply layout");
static_assert(offsetof(MellowNativeGpuReply, output) == 104, "native GPU output offset");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(MellowNativeGpuRequest) == 64, "native GPU request layout");
_Static_assert(offsetof(MellowNativeGpuRequest, nonce) == 40, "native GPU nonce offset");
_Static_assert(sizeof(MellowNativeGpuReply) == 1152, "native GPU reply layout");
_Static_assert(offsetof(MellowNativeGpuReply, output) == 104, "native GPU output offset");
#endif
#endif
