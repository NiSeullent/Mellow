// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
#pragma once
#include "XeContextExecution.hpp"
#include "XeMemoryIOKit.hpp"

namespace XeContext {
enum class IOKitStagingStatus { NotAttempted, Staged, Invalid, Unavailable, CopyMismatch, DmaFailure };
struct IOKitStagingReport {
    IOKitStagingStatus status {IOKitStagingStatus::NotAttempted};
    uint32_t copiedHeaps {}, synchronizedAllocations {};
    uint64_t copiedBytes {};
};

// Replaces only stageHeaps in an existing, authoritative ExecutionBackend.
// The physical owner's admission, fresh/primed context, retained GGTT mappings,
// direct-coherent ring/LRC barrier and quiescence callbacks remain mandatory.
// No admission is inferred from an IOKit pin or from a user-provided predicate.
// VM, IOKitContext, mapper and physical backend outlive this object and every
// execution using backend(). Serialize all operations under the SAME sleepable
// ownership/reset lock as VM, GuC, GGTT and the physical backend; no reentry.
// Allocate off-stack: bounded DMA-alias checking uses about 256 KiB of scratch.
// This object owns no VM uses, pins or mapping leases; EvidenceExecution owns
// the six uses and the delegated physical context hold until its normal close.
// Prepared/handle storage, VM/IOKitContext objects, allocation records and DMA
// arrays must be disjoint from this object's entire storage, including scratch.
// stageHeaps rejects aliased Prepared/handles before even changing report().
class IOKitExecutionStaging {
public:
    static constexpr size_t MaxDataPages = 16384; // 64 MiB per data allocation.
    static constexpr size_t MaxPages = 4 + 2 * MaxDataPages;
    IOKitExecutionStaging(XeMemory::VirtualMemory &vm, XeMemory::IOKitContext &pins,
                         ExecutionBackend physical) : vm_(vm), pins_(pins), physical_(physical) {}
    IOKitExecutionStaging(const IOKitExecutionStaging &) = delete;
    IOKitExecutionStaging &operator=(const IOKitExecutionStaging &) = delete;
    // Empty backend if any authoritative callback other than stageHeaps is
    // absent. Existing physical stageHeaps is deliberately not invoked.
    ExecutionBackend backend();
    // Early source/handle/object alias rejection leaves this report unchanged;
    // the callback's false result always prevents execution publication.
    const IOKitStagingReport &report() const { return report_; }
    bool contextHeld() const { return held_; }
private:
    XeMemory::VirtualMemory &vm_;
    XeMemory::IOKitContext &pins_;
    ExecutionBackend physical_ {};
    LiveContext context_ {};
    XeDispatch::Policy policy_ {};
    bool held_ {}, policyObserved_ {};
    IOKitStagingReport report_ {};
    uint64_t pages_[MaxPages] {};
    bool configured() const;
    bool exact(const LiveContext &) const;
    bool allowed(const LiveContext &) const;
    bool stage(const LiveContext &, const XeMemory::Handle (&)[6], const XeDispatch::Prepared &);
    static bool admitted(void *, const LiveContext &, const XeDispatch::Policy &);
    static bool freshStopped(void *, const LiveContext &);
    static bool retainContext(void *, const LiveContext &);
    static bool releaseContext(void *, const LiveContext &);
    static bool stageHeaps(void *, const LiveContext &, const XeMemory::Handle (&)[6], const XeDispatch::Prepared &);
    static bool synchronizeContext(void *, const LiveContext &);
    static bool quiesced(void *, const LiveContext &);
};
}
