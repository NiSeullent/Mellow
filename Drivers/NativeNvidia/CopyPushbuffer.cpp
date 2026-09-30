/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2017-2025 NVIDIA Corporation
 * Copyright (c) 2018-2023 NVIDIA Corporation
 * Copyright (c) 2020-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Copyright (c) 2026 Mellow contributors.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "CopyPushbuffer.hpp"
#include "../PortedNvidiaCopy/NvidiaCopyEncoder.hpp"

// Primary source prefix (pinned revision, no architecture-derived class):
// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/
// kernel-open/nvidia-uvm/uvm_turing_host.c L30-39: WFI then SYS_MEMBAR.
// kernel-open/nvidia-uvm/uvm_turing_ce.c L52-69: one-word CE release + SYS flush;
// L135-149: transfer flush alone is insufficient; use host WFI + MEMBAR.
// kernel-open/nvidia-uvm/uvm_channel.c L957-966: SYS completion ordering.
// kernel-open/nvidia-uvm/uvm_hal.c: C56F/C6B5 inherit reviewed Turing methods.
// kernel-open/nvidia-uvm/uvm_push_macros.h L69-90: host0, CE fixed4.
// src/common/sdk/nvidia/inc/class/cla06fsubch.h: COPY_ENGINE=4.
// src/common/sdk/nvidia/inc/class/clc56f.h: WFI0x78; MEM_OP_A-D0x28..34,
// MEM_OP_C SYS_MEMBAR=0; MEM_OP_D MEMBAR=5 in bits31:27.
// src/common/sdk/nvidia/inc/class/clc6b5.h: SET_SEMAPHORE0x240..248,
// 49-bit address; LAUNCH_DMA0x300 NONE transfer, SYS flush bit2, release bit3.

namespace MellowNativeNvidia {
namespace {
bool sameDevice(MellowNative::DeviceIdentity a, MellowNative::DeviceIdentity b)
{
    return a.vendor == b.vendor && a.device == b.device && a.registryId == b.registryId && a.epoch == b.epoch;
}
bool sameHandle(MellowNative::MemoryHandle a, MellowNative::MemoryHandle b)
{
    return a.slot == b.slot && a.generation == b.generation && a.registryId == b.registryId && a.epoch == b.epoch;
}
bool span(uint64_t offset, uint64_t bytes, uint64_t limit)
{
    return bytes && offset <= limit && bytes <= limit - offset;
}
uint32_t readLittleEndian(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
void writeLittleEndian(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i)
        bytes[i] = uint8_t(value >> (i * 8));
}
bool actualView(MellowNative::MemoryOwner &memory, const ChannelBinding &binding,
    const MellowNative::MemoryView &provided, MellowNative::MemoryView &actual)
{
    if (memory.inspect(binding.owner, provided.handle, actual) != MellowNative::MemoryStatus::Ok)
        return false;
    return sameDevice(actual.device, binding.device) && sameDevice(actual.device, provided.device) &&
        sameHandle(actual.handle, provided.handle) && actual.owner == binding.owner && actual.owner == provided.owner &&
        actual.state == MellowNative::MemoryState::Mapped && actual.state == provided.state &&
        actual.jobHolds && actual.jobHolds == provided.jobHolds && actual.bytes == provided.bytes &&
        actual.pin.cookie && actual.pin.cookie == provided.pin.cookie && actual.pin.cpu && actual.pin.cpu == provided.pin.cpu &&
        actual.pin.pages == provided.pin.pages && actual.pin.pageCount == provided.pin.pageCount &&
        actual.pin.bytes == actual.bytes && actual.pin.bytes == provided.pin.bytes &&
        actual.pin.deviceRegistryId == binding.device.registryId &&
        actual.pin.deviceRegistryId == provided.pin.deviceRegistryId && actual.mapping.cookie &&
        actual.mapping.cookie == provided.mapping.cookie && actual.mapping.address == provided.mapping.address &&
        actual.mapping.bytes == actual.bytes && actual.mapping.bytes == provided.mapping.bytes &&
        actual.mapping.writable == provided.mapping.writable;
}
}

QueueStatus sealCopyPushbuffer(void *context, const ChannelBinding &binding,
    const MellowNative::MemoryView &providedCommands, PushbufferRange range,
    const MellowNative::MemoryView *providedResources, uint32_t resourceCount,
    uint64_t fenceGpuAddress, uint32_t fencePayload, uint64_t &submittedBytes)
{
    submittedBytes = 0;
    if (!context)
        return QueueStatus::Unavailable;
    auto &sealer = *static_cast<CopySealerContext *>(context);
    if (!sealer.memory || !sealer.authority || !sealer.resolveEngineAndVm)
        return QueueStatus::Unavailable;
    if (binding.channelClass != 0xC56F)
        return QueueStatus::UnsupportedClass;
    if (binding.device.vendor != 0x10de || !binding.device.device || binding.device.device == UINT16_MAX ||
        !binding.device.registryId || !binding.device.epoch || !binding.owner || !binding.vm ||
        !binding.channel || !binding.channelGeneration || !fencePayload || resourceCount != 2 || !providedResources ||
        range.commandBytes != CopyInputBytes || range.offset % 4 || range.capacityBytes % 4)
        return QueueStatus::Invalid;
    if (range.capacityBytes < CopySealedBytes)
        return QueueStatus::Capacity;
    if (sealer.memory->draining() || !sameDevice(sealer.memory->device(), binding.device))
        return QueueStatus::Ownership;
    MellowNative::MemoryView commands {}, resources[2] {}, fence {};
    if (!actualView(*sealer.memory, binding, providedCommands, commands) || commands.jobHolds != 1 ||
        !actualView(*sealer.memory, binding, providedResources[0], resources[0]) ||
        !actualView(*sealer.memory, binding, providedResources[1], resources[1]))
        return QueueStatus::Ownership;
    if (sameHandle(commands.handle, binding.ring) || sameHandle(commands.handle, binding.fence) ||
        sameHandle(resources[0].handle, resources[1].handle))
        return QueueStatus::Ownership;
    for (const auto &resource : resources)
        if (sameHandle(resource.handle, commands.handle) || sameHandle(resource.handle, binding.ring) ||
            sameHandle(resource.handle, binding.fence))
            return QueueStatus::Ownership;
    if (range.capacityBytes > Mellow::PortedNvidia::GpfifoBytesMax ||
        !span(range.offset, range.capacityBytes, commands.bytes) ||
        commands.mapping.address > UINT64_MAX - range.offset)
        return QueueStatus::Invalid;
    const uint64_t fetchAddress = commands.mapping.address + range.offset;
    if (fetchAddress > Mellow::PortedNvidia::GpfifoAddressMax ||
        CopySealedBytes - 1 > Mellow::PortedNvidia::GpfifoAddressMax - fetchAddress)
        return QueueStatus::Invalid;
    if (sealer.memory->inspect(binding.owner, binding.fence, fence) != MellowNative::MemoryStatus::Ok ||
        !sameDevice(fence.device, binding.device) || fence.state != MellowNative::MemoryState::Mapped ||
        !fence.jobHolds || !fence.pin.cookie || !fence.pin.cpu || !fence.mapping.cookie || !fence.mapping.writable ||
        !span(binding.fenceOffset, 4, fence.bytes) || !span(binding.fenceOffset, 4, fence.mapping.bytes) ||
        fence.mapping.address > UINT64_MAX - binding.fenceOffset ||
        fenceGpuAddress != fence.mapping.address + binding.fenceOffset || fenceGpuAddress % 4 ||
        fenceGpuAddress >= (1ULL << 49) || 4 > (1ULL << 49) - fenceGpuAddress)
        return QueueStatus::Ownership;
    CopyEngineObservation engine {};
    const auto authorityStatus = sealer.resolveEngineAndVm(sealer.authority, binding, commands,
        resources, 2, fence, engine);
    if (authorityStatus != QueueStatus::Ok)
        return authorityStatus;
    if (!sameDevice(engine.device, binding.device) || engine.owner != binding.owner || engine.vm != binding.vm ||
        engine.channel != binding.channel || engine.channelGeneration != binding.channelGeneration ||
        engine.channelClass != binding.channelClass)
        return QueueStatus::Ownership;
    if (engine.ceClass != 0xC6B5)
        return QueueStatus::UnsupportedClass;
    if (engine.ceSubchannel != 4 || engine.mode != ChannelMode::Ordinary || engine.memoryDomain != RingDomain::CoherentSystem)
        return QueueStatus::UnsupportedChannel;

    uint32_t input[CopyInputWords] {}, words[CopySealedWords] {};
    const auto *commandBytes = commands.pin.cpu + range.offset;
    for (uint32_t i = 0; i < CopyInputWords; ++i)
        input[i] = readLittleEndian(commandBytes + uint64_t(i) * 4);
    const uint64_t sourceAddress = uint64_t(input[3]) << 32 | input[4];
    const uint64_t destinationAddress = uint64_t(input[5]) << 32 | input[6];
    // Check before subtraction; unowned addresses must never wrap into offsets.
    if (sourceAddress < resources[0].mapping.address || destinationAddress < resources[1].mapping.address)
        return QueueStatus::Ownership;
    const MellowNvidia::CopyChannelLease copyChannel {
        binding.owner, binding.vm, binding.device.epoch, engine.ceClass, engine.ceSubchannel, true, true, true};
    const MellowNvidia::CopyBufferLease source {binding.owner, binding.vm, binding.device.epoch,
        resources[0].handle.generation, resources[0].mapping.address, resources[0].bytes, true, true, resources[0].mapping.writable};
    const MellowNvidia::CopyBufferLease destination {binding.owner, binding.vm, binding.device.epoch,
        resources[1].handle.generation, resources[1].mapping.address, resources[1].bytes, true, true, resources[1].mapping.writable};
    const MellowNvidia::VirtualLinearCopy copy {copyChannel, source, destination,
        sourceAddress - resources[0].mapping.address, destinationAddress - resources[1].mapping.address, input[8]};
    size_t writtenWords = 0;
    if (MellowNvidia::encodeVirtualLinearCopy(copy, words, CopySealedWords, writtenWords) != MellowNvidia::CopyStatus::Ok ||
        writtenWords != CopyInputWords)
        return QueueStatus::Invalid;
    for (uint32_t i = 0; i < CopyInputWords; ++i)
        if (input[i] != words[i])
            return QueueStatus::Invalid; // Exact whitelist: no alternate headers or launch bits.

    auto method = [&](uint32_t index, uint32_t address, uint32_t count, uint32_t subchannel) {
        return Mellow::PortedNvidia::encodeMethodHeader(Mellow::PortedNvidia::ChannelClass::AmpereA,
            Mellow::PortedNvidia::MethodMode::Incrementing, address, subchannel, count, words[index]) ==
            Mellow::PortedNvidia::Status::Ok;
    };
    if (!method(11, 0x78, 1, 0) || !method(13, 0x28, 4, 0) ||
        !method(18, 0x240, 3, 4) || !method(22, 0x300, 1, 4))
        return QueueStatus::Invalid;
    words[12] = 0;             // Host WFI, before the system memory barrier.
    words[14] = words[15] = words[16] = 0; // MEM_OP_A/B + SYS_MEMBAR TYPE=0.
    words[17] = 5U << 27;      // MEM_OP_D OPERATION=MEMBAR.
    words[19] = uint32_t(fenceGpuAddress >> 32); // CE semaphore 49-bit upper field.
    words[20] = uint32_t(fenceGpuAddress);
    words[21] = fencePayload;
    words[23] = (1U << 2) | (1U << 3); // SYS_FLUSH + ONE_WORD_RELEASE, transfer NONE.
    // Every validation precedes the first write. No CPU fence store occurs.
    auto *destinationBytes = commands.pin.cpu + range.offset;
    for (uint32_t i = 0; i < CopySealedWords; ++i)
        writeLittleEndian(destinationBytes + uint64_t(i) * 4, words[i]);
    submittedBytes = CopySealedBytes;
    return QueueStatus::Ok;
}

} // namespace MellowNativeNvidia
