// Host tests only. VM/engine authority below is a simulation, never GPU evidence.
#include "../Drivers/NativeNvidia/CopyPushbuffer.hpp"
#include "../Drivers/PortedNvidiaCopy/NvidiaCopyEncoder.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace MellowNative;
using namespace MellowNativeNvidia;
namespace {
constexpr uint64_t Owner = 31;
const DeviceIdentity Device {0x10de, 0x2206, 0x12345678, 9};
struct Mock {
    alignas(4096) uint8_t backing[8][4096] {};
    uint64_t dma[8] {};
    uint64_t addresses[8] {0x100000, 0x101000, 0x102000, 0x103000, 0x104000, 0x105000, 0x106000, 0x107000};
    uint32_t cookies[8] {};
    QueueStatus authorityResult {QueueStatus::Ok};
    uint32_t ceClass {0xC6B5}, subchannel {4}, calls {};
    ChannelMode mode {ChannelMode::Ordinary};
    RingDomain domain {RingDomain::CoherentSystem};
    bool wrongVm {}, wrongDevice {}, wrongEpoch {}, wrongChannel {};
};
bool admitted(void *, const DeviceIdentity &device)
{
    return device.vendor == Device.vendor && device.device == Device.device &&
        device.registryId == Device.registryId && device.epoch == Device.epoch;
}
MemoryStatus pin(void *opaque, const DeviceIdentity &device, uint64_t, const MemoryHandle &handle,
    uint64_t bytes, DmaPin &out)
{
    auto &mock = *static_cast<Mock *>(opaque);
    assert(handle.slot < 8 && bytes == 4096);
    mock.dma[handle.slot] = 0x10000000 + uint64_t(handle.slot) * 4096;
    out = {mock.backing[handle.slot], mock.backing[handle.slot], &mock.dma[handle.slot], 1, bytes, device.registryId};
    return MemoryStatus::Ok;
}
MemoryStatus unpin(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, DmaPin &) { return MemoryStatus::Ok; }
MemoryStatus sync(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, CacheDirection)
{
    return MemoryStatus::Ok;
}
MemoryStatus map(void *opaque, const DeviceIdentity &, uint64_t, const MemoryHandle &handle,
    const DmaPin &pin, bool writable, GpuMapping &out)
{
    auto &mock = *static_cast<Mock *>(opaque);
    out = {&mock.cookies[handle.slot], mock.addresses[handle.slot], pin.bytes, writable};
    return MemoryStatus::Ok;
}
MemoryStatus unmap(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, GpuMapping &)
{
    return MemoryStatus::Ok;
}
bool retired(void *, const MemoryView &) { return true; }
bool stopped(void *, const DeviceIdentity &) { return true; }
QueueStatus authority(void *opaque, const ChannelBinding &binding, const MemoryView &commands,
    const MemoryView *resources, uint32_t count, const MemoryView &fence, CopyEngineObservation &out)
{
    auto &mock = *static_cast<Mock *>(opaque);
    ++mock.calls;
    assert(commands.jobHolds == 1 && count == 2 && resources[0].jobHolds && resources[1].jobHolds && fence.jobHolds);
    out = {binding.device, binding.owner, binding.vm, binding.channel, binding.channelGeneration,
        binding.channelClass, mock.ceClass, mock.subchannel, mock.mode, mock.domain};
    if (mock.wrongVm) ++out.vm;
    if (mock.wrongDevice) ++out.device.registryId;
    if (mock.wrongEpoch) ++out.device.epoch;
    if (mock.wrongChannel) ++out.channelGeneration;
    return mock.authorityResult;
}
uint32_t le(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
void writeLe(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (i * 8));
}
struct Fixture {
    Mock mock;
    MemoryOwner memory;
    ChannelBinding binding;
    MemoryHandle handles[5] {};
    MemoryView commands {}, resources[2] {}, fence {};
    CopySealerContext context {&memory, &mock, authority};
    PushbufferRange range {16, CopyInputBytes, 128};
    Fixture(bool writableDestination = true, bool highFence = false, bool writableFence = true,
        uint64_t commandAddress = 0x102000)
    {
        if (highFence) mock.addresses[1] = (1ULL << 44) + 4096;
        mock.addresses[2] = commandAddress;
        const MemoryBackend backend {&mock, admitted, pin, unpin, sync, map, unmap, retired, stopped};
        assert(memory.initialize(Device, {}, backend) == MemoryStatus::Ok);
        for (uint32_t i = 0; i < 5; ++i) {
            assert(memory.allocate(Owner, 4096, handles[i]) == MemoryStatus::Ok);
            assert(memory.map(Owner, handles[i], (i != 4 || writableDestination) && (i != 1 || writableFence)) == MemoryStatus::Ok);
        }
        binding = {Device, Owner, 47, 73, 11, 0xC56F, 4, handles[0], handles[1], 16, 8};
        memset(mock.backing[2], 0xCD, 4096);
        memset(mock.backing[1], 0xA5, 4096); // Initialized before GPU ownership.
        for (auto handle : handles) assert(memory.retain(Owner, handle) == MemoryStatus::Ok);
        refresh();
        original();
    }
    void refresh()
    {
        assert(memory.inspect(Owner, handles[2], commands) == MemoryStatus::Ok);
        assert(memory.inspect(Owner, handles[3], resources[0]) == MemoryStatus::Ok);
        assert(memory.inspect(Owner, handles[4], resources[1]) == MemoryStatus::Ok);
        assert(memory.inspect(Owner, handles[1], fence) == MemoryStatus::Ok);
    }
    void original()
    {
        const MellowNvidia::CopyChannelLease channel {Owner, binding.vm, Device.epoch, 0xC6B5, 4, true, true, true};
        const MellowNvidia::CopyBufferLease source {Owner, binding.vm, Device.epoch, resources[0].handle.generation,
            resources[0].mapping.address, 4096, true, true, true};
        // Build the expected packet even for a later read-only destination test.
        const MellowNvidia::CopyBufferLease destination {Owner, binding.vm, Device.epoch, resources[1].handle.generation,
            resources[1].mapping.address, 4096, true, true, true};
        const MellowNvidia::VirtualLinearCopy copy {channel, source, destination, 32, 64, 256};
        uint32_t words[CopyInputWords];
        size_t count = 0;
        assert(MellowNvidia::encodeVirtualLinearCopy(copy, words, CopyInputWords, count) == MellowNvidia::CopyStatus::Ok);
        for (uint32_t i = 0; i < CopyInputWords; ++i) writeLe(mock.backing[2] + range.offset + uint64_t(i) * 4, words[i]);
    }
    uint64_t fenceAddress() const { return fence.mapping.address + binding.fenceOffset; }
    QueueStatus seal(uint64_t &bytes, uint32_t payload = 19)
    {
        return sealCopyPushbuffer(&context, binding, commands, range, resources, 2, fenceAddress(), payload, bytes);
    }
    void finish()
    {
        for (auto handle : handles) {
            MemoryView view;
            assert(memory.inspect(Owner, handle, view) == MemoryStatus::Ok);
            for (uint32_t i = 0; i < view.jobHolds; ++i) assert(memory.release(Owner, handle) == MemoryStatus::Ok);
        }
        assert(memory.quiesce() == MemoryStatus::Ok);
        assert(memory.close() == MemoryStatus::Ok);
    }
};
void exactOrderedGpuTail()
{
    Fixture f(true, true); // Fence above 40 bits uses the reviewed 49-bit CE path.
    uint8_t beforeCommands[4096], beforeFence[4096];
    memcpy(beforeCommands, f.mock.backing[2], 4096);
    memcpy(beforeFence, f.mock.backing[1], 4096);
    uint64_t bytes = 77;
    assert(f.seal(bytes) == QueueStatus::Ok && bytes == 96 && f.mock.calls == 1);
    const uint32_t tail[13] {0x2001001e, 0, 0x2004000a, 0, 0, 0, 0x28000000,
        0x20038090, uint32_t(f.fenceAddress() >> 32), uint32_t(f.fenceAddress()), 19, 0x200180c0, 0xC};
    for (uint32_t i = 0; i < 13; ++i)
        assert(le(f.mock.backing[2] + f.range.offset + CopyInputBytes + uint64_t(i) * 4) == tail[i]);
    assert(!memcmp(beforeCommands, f.mock.backing[2], size_t(f.range.offset + CopyInputBytes)));
    assert(!memcmp(beforeCommands + f.range.offset + CopySealedBytes,
        f.mock.backing[2] + f.range.offset + CopySealedBytes, size_t(4096 - f.range.offset - CopySealedBytes)));
    assert(!memcmp(beforeFence, f.mock.backing[1], 4096)); // CPU never fabricates completion.
    assert(f.seal(bytes) == QueueStatus::Ok && bytes == CopySealedBytes); // Same tail, not a second tail.
    f.range.commandBytes = CopySealedBytes;
    uint8_t sealed[4096];
    memcpy(sealed, f.mock.backing[2], 4096);
    assert(f.seal(bytes) == QueueStatus::Invalid && !bytes && !memcmp(sealed, f.mock.backing[2], 4096));
    f.finish();
}
void rejectedPacketIsAtomic()
{
    const uint32_t headerIndices[6] {0, 1, 2, 7, 9, 10};
    for (auto index : headerIndices) {
        Fixture f;
        f.mock.backing[2][f.range.offset + uint64_t(index) * 4] ^= 1;
        uint8_t before[4096];
        memcpy(before, f.mock.backing[2], 4096);
        uint64_t bytes = 777;
        assert(f.seal(bytes) == QueueStatus::Invalid && !bytes);
        assert(!memcmp(before, f.mock.backing[2], 4096));
        f.finish();
    }
    for (uint32_t scenario = 0; scenario < 6; ++scenario) {
        Fixture f;
        auto *input = f.mock.backing[2] + f.range.offset;
        if (scenario == 0) writeLe(input + 4 * 4, uint32_t(f.resources[0].mapping.address - 1));
        if (scenario == 1) writeLe(input + 6 * 4, uint32_t(f.resources[1].mapping.address - 1));
        if (scenario == 2) writeLe(input + 8 * 4, 0);
        if (scenario == 3) writeLe(input + 8 * 4, 5000);
        if (scenario == 4) writeLe(input + 3 * 4, 1U << 17);
        if (scenario == 5) writeLe(input + 5 * 4, 1U << 17);
        uint8_t before[4096];
        memcpy(before, f.mock.backing[2], 4096);
        uint64_t bytes = 8;
        assert(f.seal(bytes) != QueueStatus::Ok && !bytes && !memcmp(before, f.mock.backing[2], 4096));
        f.finish();
    }
}
void rejectedAuthorityAndSnapshots()
{
    for (uint32_t scenario = 0; scenario < 13; ++scenario) {
        Fixture f;
        if (scenario == 0) f.context.resolveEngineAndVm = nullptr;
        if (scenario == 1) f.mock.authorityResult = QueueStatus::Unavailable;
        if (scenario == 2) f.mock.wrongVm = true;
        if (scenario == 3) f.mock.wrongDevice = true;
        if (scenario == 4) f.mock.wrongEpoch = true;
        if (scenario == 5) f.mock.wrongChannel = true;
        if (scenario == 6) f.mock.ceClass = 0xC7B5;
        if (scenario == 7) f.mock.subchannel = 0;
        if (scenario == 8) f.mock.mode = ChannelMode::Confidential;
        if (scenario == 9) f.mock.domain = RingDomain::NoncoherentSystem;
        if (scenario == 10) ++f.commands.mapping.address;
        if (scenario == 11) ++f.resources[0].handle.epoch;
        if (scenario == 12) assert(f.memory.release(Owner, f.handles[4]) == MemoryStatus::Ok);
        uint8_t before[4096], beforeFence[4096];
        memcpy(before, f.mock.backing[2], 4096);
        memcpy(beforeFence, f.mock.backing[1], 4096);
        uint64_t bytes = 8;
        assert(f.seal(bytes) != QueueStatus::Ok && !bytes && !memcmp(before, f.mock.backing[2], 4096));
        assert(!memcmp(beforeFence, f.mock.backing[1], 4096));
        f.finish();
    }
}
void ownershipAndRangeLimits()
{
    for (uint32_t scenario = 0; scenario < 8; ++scenario) {
        Fixture f(scenario != 0, false, scenario != 1);
        if (scenario == 2) f.range.capacityBytes = 92;
        if (scenario == 3) f.range.offset = UINT64_MAX - 3;
        if (scenario == 4) f.range.commandBytes = CopySealedBytes;
        if (scenario == 5) f.binding.fenceOffset = 4096;
        if (scenario == 6) ++f.binding.device.epoch;
        if (scenario == 7) f.binding.channelClass = 0xC46F;
        uint8_t before[4096];
        memcpy(before, f.mock.backing[2], 4096);
        uint64_t bytes = 8;
        assert(f.seal(bytes) != QueueStatus::Ok && !bytes && !memcmp(before, f.mock.backing[2], 4096));
        f.finish();
    }
    {
        Fixture f;
        uint64_t bytes = 8;
        uint8_t before[4096];
        memcpy(before, f.mock.backing[2], 4096);
        assert(sealCopyPushbuffer(&f.context, f.binding, f.commands, f.range, f.resources, 2,
            f.fenceAddress() + 4, 19, bytes) == QueueStatus::Ownership && !bytes);
        assert(f.seal(bytes, 0) == QueueStatus::Invalid && !bytes);
        assert(!memcmp(before, f.mock.backing[2], 4096));
        f.finish();
    }
    {
        Fixture f(true, false, true, 1ULL << 40);
        uint64_t bytes = 8;
        assert(f.seal(bytes) == QueueStatus::Invalid && !bytes);
        f.finish();
    }
    {
        Fixture f(true, false, true, (1ULL << 40) - 4096);
        f.range = {4000, CopyInputBytes, CopySealedBytes};
        f.original();
        uint64_t bytes;
        assert(f.seal(bytes) == QueueStatus::Ok && bytes == CopySealedBytes);
        // The complete 96-byte fetch ends exactly at the last 40-bit byte.
        f.range.offset = 4004;
        assert(f.seal(bytes) == QueueStatus::Invalid && !bytes);
        f.finish();
    }
}
struct PhysicalSimulation {
    Fixture *fixture {};
    uint32_t put {}, token {0x11223344}, rung {}, writes {};
    char trace[32] {};
    uint32_t count {};
    void record(char event) { assert(count < 31); trace[count++] = event; }
};
Operation claimChannel(void *opaque, const ChannelBinding &binding, const MemoryView &ring,
    const MemoryView &fence, ChannelObservation &out)
{
    auto &sim = *static_cast<PhysicalSimulation *>(opaque);
    sim.record('C');
    assert(ring.jobHolds == 1 && fence.jobHolds == 1);
    out = {binding.device, binding.owner, binding.vm, binding.channel, binding.channelGeneration,
        binding.channelClass, binding.ringEntries, 0, 0, 0, 0, ChannelMode::Ordinary, RingDomain::CoherentSystem};
    return Operation::Done;
}
QueueStatus realSealer(void *opaque, const ChannelBinding &binding, const MemoryView &commands,
    PushbufferRange range, const MemoryView *resources, uint32_t count,
    uint64_t fenceAddress, uint32_t payload, uint64_t &bytes)
{
    auto &sim = *static_cast<PhysicalSimulation *>(opaque);
    sim.record('S');
    return sealCopyPushbuffer(&sim.fixture->context, binding, commands, range, resources, count, fenceAddress, payload, bytes);
}
QueueStatus orderBeforePut(void *opaque, const ChannelBinding &)
{
    static_cast<PhysicalSimulation *>(opaque)->record('B');
    return QueueStatus::Ok;
}
Operation put(void *opaque, const ChannelBinding &, uint32_t value)
{
    auto &sim = *static_cast<PhysicalSimulation *>(opaque);
    sim.record('P'); sim.put = value; ++sim.token; ++sim.writes;
    return Operation::Done;
}
QueueStatus orderAfterPut(void *opaque, const ChannelBinding &)
{
    static_cast<PhysicalSimulation *>(opaque)->record('A');
    return QueueStatus::Ok;
}
QueueStatus currentToken(void *opaque, const ChannelBinding &, uint32_t &out)
{
    auto &sim = *static_cast<PhysicalSimulation *>(opaque);
    sim.record('T'); out = sim.token;
    return QueueStatus::Ok;
}
Operation doorbell(void *opaque, const ChannelBinding &, uint32_t token)
{
    auto &sim = *static_cast<PhysicalSimulation *>(opaque);
    sim.record('D'); sim.rung = token;
    return Operation::Done;
}
QueueStatus readGpuFence(void *opaque, const ChannelBinding &binding, const MemoryView &fence, uint32_t &out)
{
    auto &sim = *static_cast<PhysicalSimulation *>(opaque);
    sim.record('F');
    out = le(fence.pin.cpu + binding.fenceOffset); // Simulated coherent/acquire device adapter.
    return QueueStatus::Ok;
}
bool stopChannel(void *opaque, const ChannelBinding &)
{
    static_cast<PhysicalSimulation *>(opaque)->record('Q');
    return true;
}
Operation releaseChannel(void *opaque, const ChannelBinding &)
{
    static_cast<PhysicalSimulation *>(opaque)->record('R');
    return Operation::Done;
}
void queueWithProductionSealer()
{
    Fixture f;
    // Hand lifetime management to the REAL queue; no job was GPU-published.
    for (auto handle : f.handles) assert(f.memory.release(Owner, handle) == MemoryStatus::Ok);
    f.refresh();
    writeLe(f.fence.pin.cpu + f.binding.fenceOffset, 0); // Initial payload before channel ownership.
    uint8_t initialFence[4096];
    memcpy(initialFence, f.mock.backing[1], 4096);
    PhysicalSimulation sim {&f};
    const QueueTransport transport {&sim, claimChannel, realSealer, orderBeforePut, put, orderAfterPut,
        currentToken, doorbell, readGpuFence, stopChannel, releaseChannel};
    GpFifoQueue queue(f.memory, transport);
    assert(queue.bind(f.binding) == QueueStatus::Ok);
    const DataResource data[2] {{f.handles[3], false}, {f.handles[4], true}};
    const SubmitRequest request {f.handles[2], f.range, data, 2, 100};
    FenceToken token;
    assert(queue.submit(request, 1, token) == QueueStatus::Ok);
    assert(!strcmp(sim.trace, "CSBPATD") && sim.put == 1 && sim.rung == 0x11223345 && sim.writes == 1);
    f.refresh();
    assert(f.commands.jobHolds == 1 && f.resources[0].jobHolds == 1 && f.resources[1].jobHolds == 1);
    assert(le(f.commands.pin.cpu + f.range.offset + 23 * 4) == 0xC);
    assert(le(f.commands.pin.cpu + f.range.offset + 21 * 4) == token.sequence);
    assert(!memcmp(initialFence, f.mock.backing[1], 4096)); // Actual sealer produced no CPU completion.
    Mellow::PortedNvidia::GpfifoEntry expected;
    assert(Mellow::PortedNvidia::encodeGpfifoPushbuffer(Mellow::PortedNvidia::ChannelClass::AmpereA,
        f.commands.mapping.address + f.range.offset, CopySealedBytes, Mellow::PortedNvidia::Sync::Proceed,
        expected) == Mellow::PortedNvidia::Status::Ok);
    uint8_t bytes[8];
    Mellow::PortedNvidia::writeGpfifoLittleEndian(expected, bytes);
    assert(!memcmp(bytes, f.mock.backing[0] + f.binding.ringOffset, 8));
    assert(queue.poll(2) == QueueStatus::Ok);
    f.refresh();
    assert(f.commands.jobHolds == 1 && f.resources[1].jobHolds == 1);
    // TEST DEVICE model performs the GPU release. Production code has no CPU
    // completion writer; this fixture simulates the hardware we cannot execute.
    writeLe(f.fence.pin.cpu + f.binding.fenceOffset, token.sequence);
    assert(queue.poll(3) == QueueStatus::Ok);
    f.refresh();
    assert(!f.commands.jobHolds && !f.resources[0].jobHolds && !f.resources[1].jobHolds && f.fence.jobHolds == 1);
    JobResult result;
    assert(queue.query(token, result) == QueueStatus::Ok && result.state == JobState::Completed && !result.resourcesHeld);
    assert(queue.retire(token) == QueueStatus::Ok && queue.reset() == QueueStatus::Ok);
    f.finish();
}
} // namespace

int main()
{
    exactOrderedGpuTail();
    rejectedPacketIsAtomic();
    rejectedAuthorityAndSnapshots();
    ownershipAndRangeLimits();
    queueWithProductionSealer();
    puts("Native NVIDIA copy pushbuffer + production queue host regressions passed; simulated hardware only.");
}
