// Real XeMemoryIOKit.cpp/XeMemory.cpp against a host-only OS boundary.
// Simulates XNU completion errors that consume state; no actual DMA tested.
#include "native_memory_iokit_shim.hpp"
#include "../Mellow/XeMemoryIOKit.hpp"
#include <cstdio>
#include <cstdlib>
using namespace XeMemory;
namespace Mock = NativeMemoryShim;
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (0)
struct Fixture {
    IOMapper *mapper {new IOMapper};
    IOKitContext context;
    Backend backend;
    bool quarantine {};
    Fixture() {
        Mock::reset(); context.mapper = mapper; context.maxAllocationBytes = context.maxPinnedBytes = 8192;
        backend = makeIOKitPinBackend(context);
    }
    ~Fixture() {
        CHECK(context.pinnedBytes == (quarantine ? 8192U : 0U));
        CHECK(Mock::unsafeClearCalls == 0 && Mock::prematureDestroy == 0 && Mock::forbiddenMapperLookups == 0);
        mapper->release();
    }
};
static void heldAfterUnknown(Fixture &f, Pin &pin) {
    CHECK(pin.cookie && f.context.pinnedBytes == 8192);
    const auto cookie = pin.cookie;
    const auto commandCalls = Mock::commandCompleteCalls, descriptorCalls = Mock::descriptorCompleteCalls,
        syncCalls = Mock::syncCalls;
    Mock::faults.commandComplete = false; Mock::faults.descriptorComplete = false;
    CHECK(f.backend.unpin(f.backend.context, pin) == Status::BackendFailure);
    CHECK(pin.cookie == cookie && f.context.pinnedBytes == 8192);
    CHECK(Mock::commandCompleteCalls == commandCalls && Mock::descriptorCompleteCalls == descriptorCalls);
    CHECK(synchronizeForDevice(pin) != Status::Ok && synchronizeForCpu(pin) != Status::Ok);
    CHECK(kernelBuffer(pin) == nullptr && Mock::syncCalls == syncCalls);
    CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    f.quarantine = true;
}
int main() {
    { Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
      CHECK(pin.pageCount == 2 && pin.dmaPages[0] == 0x800000 && pin.dmaPages[1] == 0x801000);
      auto *cpu = static_cast<uint8_t *>(kernelBuffer(pin)); CHECK(cpu != nullptr);
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == cpu);
      for (size_t i = 0; i < 8192; ++i) CHECK(cpu[i] == 0);
      // A real private pin is insufficient without the exact allocation owner,
      // context, descriptor extent and prepared DMA association. No lookup
      // admits client-supplied cookie addresses or fabricates CPU ownership.
      IOKitContext sameMapperForeign; sameMapperForeign.mapper = f.mapper;
      CHECK(resolvePinnedBuffer(sameMapperForeign, 42, 8192, pin) == nullptr);
      CHECK(resolvePinnedBuffer(f.context, 43, 8192, pin) == nullptr);
      CHECK(resolvePinnedBuffer(f.context, 0, 8192, pin) == nullptr);
      CHECK(resolvePinnedBuffer(f.context, 42, 4096, pin) == nullptr);
      CHECK(resolvePinnedBuffer(f.context, 42, 8193, pin) == nullptr);
      CHECK(resolvePinnedBuffer(f.context, 42, 0, pin) == nullptr);
      Pin changed = pin; changed.pageCount = 1;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, changed) == nullptr);
      changed = pin; changed.dmaPages = nullptr;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, changed) == nullptr);
      CHECK(Mock::lastBuffer != nullptr && Mock::lastCommand != nullptr);
      Mock::lastBuffer->bytes = 4096;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
      Mock::lastBuffer->bytes = 8192;
      auto *originalDescriptor = Mock::lastCommand->memory; Mock::lastCommand->memory = nullptr;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
      Mock::lastCommand->memory = originalDescriptor;
      auto *differentMapper = new IOMapper; f.context.mapper = differentMapper;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
      f.context.mapper = f.mapper; differentMapper->release();
      Mock::faults.noCpu = true;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
      Mock::faults.noCpu = false;
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == cpu);
      CHECK(f.context.pinnedBytes == 8192 && Mock::syncCalls == 0 &&
          Mock::commandCompleteCalls == 0 && Mock::descriptorCompleteCalls == 0);
      CHECK(synchronizeForDevice(pin) == Status::Ok && synchronizeForCpu(pin) == Status::Ok);
      IOKitContext foreign; CHECK(f.backend.unpin(&foreign, pin) == Status::Invalid && f.context.pinnedBytes == 8192);
      CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok && !pin.cookie);
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
      CHECK(Mock::commandCompleteCalls == 1 && Mock::descriptorCompleteCalls == 1);
      CHECK(Mock::ordered("dma.complete", "dma.clear") && Mock::ordered("dma.clear", "dma.destroy") &&
          Mock::ordered("dma.destroy", "memory.complete") && Mock::ordered("memory.complete", "memory.destroy")); }
    { Fixture f; Pin pin {}; Mock::faults.commandPrepare = true;
      CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::BackendFailure);
      CHECK(!pin.cookie && f.context.pinnedBytes == 0 && Mock::commandCompleteCalls == 1); }
    { Fixture f; Pin pin {}; Mock::faults.descriptorPrepare = true;
      CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::BackendFailure);
      CHECK(!pin.cookie && f.context.pinnedBytes == 0 && Mock::descriptorCompleteCalls == 1); }
    { Fixture f; Pin pin {}; Mock::faults.setDescriptor = true;
      CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::BackendFailure && !pin.cookie && f.context.pinnedBytes == 0); }
    { Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
      Mock::faults.clearDescriptor = true;
      CHECK(f.backend.unpin(f.backend.context, pin) == Status::BackendFailure && pin.cookie && f.context.pinnedBytes == 8192);
      CHECK(Mock::commandCompleteCalls == 1);
      CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == nullptr);
      Mock::faults.clearDescriptor = false;
      CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok && !pin.cookie && Mock::commandCompleteCalls == 1); }
    CHECK(Mock::objects.empty() && Mock::allocations.empty());
    { Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
      Mock::faults.commandComplete = true;
      CHECK(f.backend.unpin(f.backend.context, pin) == Status::BackendFailure);
      CHECK(Mock::lastCommand && !Mock::lastCommand->active && Mock::lastCommand->getMemoryDescriptor());
      CHECK(Mock::commandCompleteCalls == 1 && Mock::descriptorCompleteCalls == 0);
      heldAfterUnknown(f, pin); }
    { Fixture f; Pin pin {}; Mock::faults.commandPrepare = true; Mock::faults.commandComplete = true;
      CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::BackendFailure);
      CHECK(Mock::lastCommand && !Mock::lastCommand->active && Mock::commandCompleteCalls == 1);
      heldAfterUnknown(f, pin); }
    { Fixture f; Pin pin {}; Mock::faults.commandPrepare = true; Mock::faults.commandPrepareAcquires = false;
      CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::BackendFailure && Mock::commandCompleteCalls == 1);
      heldAfterUnknown(f, pin); }
    { Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
      Mock::faults.descriptorComplete = true;
      CHECK(f.backend.unpin(f.backend.context, pin) == Status::BackendFailure);
      CHECK(!Mock::lastCommand && Mock::lastBuffer && !Mock::lastBuffer->prepared && Mock::descriptorCompleteCalls == 1);
      heldAfterUnknown(f, pin); }
    { Fixture f; Pin pin {}; Mock::faults.descriptorPrepare = true; Mock::faults.descriptorComplete = true;
      CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::BackendFailure && Mock::descriptorCompleteCalls == 1);
      heldAfterUnknown(f, pin); }
    CHECK(!Mock::objects.empty() && !Mock::allocations.empty());
    // Explicit fake-world disposal only after retained production/context
    // objects are gone. This supplies no production cleanup authority.
    Mock::disposeQuarantinedHostWorld();
    CHECK(Mock::objects.empty() && Mock::allocations.empty());
    std::printf("PASS Xe DMA completion boundary: %u checks; simulated OS, no physical DMA\n", checks);
}
