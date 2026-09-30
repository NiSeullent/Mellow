// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#pragma once
#include "GpFifoQueue.hpp"

namespace MellowNativeNvidia {

struct CopyEngineObservation {
    MellowNative::DeviceIdentity device {};
    uint64_t owner {}, vm {}, channel {}, channelGeneration {};
    uint32_t channelClass {}, ceClass {}, ceSubchannel {};
    ChannelMode mode {ChannelMode::Unsupported};
    RingDomain memoryDomain {RingDomain::Unsupported};
};
struct CopySealerContext {
    MellowNative::MemoryOwner *memory {};
    void *authority {};
    // Real synchronized channel/GPU-VM owner, not caller-supplied ready bits.
    // Resolve negotiated CE class/subchannel, exclusive ordinary channel and
    // every exact allocation's current mapping in binding.vm. Prove ordinary
    // non-peer/non-proxy/non-confidential coherent, non-bounced system backing
    // for all views, dedicated GPU-only fence, and sealed private commands.
    // Success holds this condition through the queue's publication/completion.
    // Inspect the already-held lease only. Acquire no ownership and modify no
    // GPU/buffer state: this resolver has no side-effect rollback receipt.
    QueueStatus (*resolveEngineAndVm)(void *, const ChannelBinding &,
        const MellowNative::MemoryView &commands, const MellowNative::MemoryView *resources,
        uint32_t resourceCount, const MellowNative::MemoryView &fence, CopyEngineObservation &) {};
};

constexpr uint32_t CopyInputWords = 11, CopySealedWords = 24;
constexpr uint64_t CopyInputBytes = CopyInputWords * 4, CopySealedBytes = CopySealedWords * 4;

// Direct QueueTransport::sealPushbuffer adapter. Accepts ONLY the imported
// encoder's exact 44-byte virtual linear copy, resources[0]=source/[1]=dest,
// negotiated C56F+C6B5/subchannel4. Re-inspects held MemoryOwner snapshots,
// validates/re-encodes the copy, appends HOST WFI + SYS_MEMBAR + CE SYS-flushed
// one-word GPU semaphore release, then writes all 96 bytes as little endian.
// CPU never writes the fence. No success without the actual authority callback.
// Transactional before publication: errors leave every command byte unchanged
// and submittedBytes=0. The exclusive owner lock prevents concurrent CPU writes;
// this function neither creates a channel nor publishes or proves GPU execution.
// A retry still requests commandBytes=44; an appended 96-byte input is rejected.
QueueStatus sealCopyPushbuffer(void *context, const ChannelBinding &,
    const MellowNative::MemoryView &commands, PushbufferRange,
    const MellowNative::MemoryView *resources, uint32_t resourceCount,
    uint64_t fenceGpuAddress, uint32_t fencePayload, uint64_t &submittedBytes);

} // namespace MellowNativeNvidia
