// SPDX-License-Identifier: MIT
#ifndef MELLOW_XE_PROBE_ABI_H
#define MELLOW_XE_PROBE_ABI_H
#include <stdint.h>
// The existing v1 diagnostic selectors 0..2 remain byte-for-byte compatible.
#define MELLOW_XE_PROBE_SELECTOR 3U
#define MELLOW_XE_PROBE_VERSION 1U
typedef struct MellowXeProbeRequest {
    uint32_t version, size;
    uint64_t nonce, reserved[2];
} MellowXeProbeRequest;
typedef struct MellowXeProbeReply {
    uint32_t version, size, status, darwinMajor;
    uint64_t nonce, sampledMicros, pciRegistryId, barBytes;
    uint32_t pciId, subsystemId, classRevision, command, pmcsr, gmd;
    uint32_t bus, slot, function, gpuSubmissionSupported, metalSupported, reserved0;
    uint64_t reserved[2];
} MellowXeProbeReply;
#if defined(__cplusplus)
static_assert(sizeof(MellowXeProbeRequest) == 32, "probe request ABI");
static_assert(sizeof(MellowXeProbeReply) == 112, "probe reply ABI");
#else
_Static_assert(sizeof(MellowXeProbeRequest) == 32, "probe request ABI");
_Static_assert(sizeof(MellowXeProbeReply) == 112, "probe reply ABI");
#endif
#endif
