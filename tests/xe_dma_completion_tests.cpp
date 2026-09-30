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
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    f.quarantine = true;
}
static void directInspectionSuccessAndIdentity() {
    Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
    void *cpu = kernelBuffer(pin); CHECK(cpu != nullptr);
    const auto allocations = Mock::allocations.size(), objects = Mock::objects.size();
    const auto segments = Mock::segmentCalls, physical = Mock::physicalCalls, translations = Mock::translationCalls;
    for (unsigned repeat = 0; repeat < 3; ++repeat)
        CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == cpu);
    CHECK(Mock::segmentCalls == segments + 6 && Mock::physicalCalls == physical + 6);
    CHECK(Mock::translationCalls == translations + 12);
    CHECK(Mock::allocations.size() == allocations && Mock::objects.size() == objects);
    CHECK(Mock::syncCalls == 0 && Mock::commandCompleteCalls == 0 && Mock::descriptorCompleteCalls == 0);
    CHECK(f.context.pinnedBytes == 8192 && pin.dmaPages[0] == 0x800000 && pin.dmaPages[1] == 0x801000);
    // AlwaysPrepared is a valid stable public preparation ID, not an error.
    Mock::faults.alwaysPrepared = true;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == cpu);
    Mock::faults.alwaysPrepared = false;
    IOKitContext foreign; foreign.mapper = f.mapper;
    const auto before = Mock::segmentCalls;
    CHECK(resolveDirectPinnedBuffer(foreign, 42, 8192, pin) == nullptr);
    for (uint64_t owner : {uint64_t(0), uint64_t(43)})
        CHECK(resolveDirectPinnedBuffer(f.context, owner, 8192, pin) == nullptr);
    for (uint64_t bytes : {uint64_t(0), uint64_t(4096), uint64_t(8193), UINT64_MAX})
        CHECK(resolveDirectPinnedBuffer(f.context, 42, bytes, pin) == nullptr);
    Pin changed = pin; changed.pageCount = 1;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, changed) == nullptr);
    changed = pin; changed.dmaPages = nullptr;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, changed) == nullptr);
    auto *originalDescriptor = Mock::lastCommand->memory; Mock::lastCommand->memory = nullptr;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    Mock::lastCommand->memory = originalDescriptor;
    Mock::lastBuffer->prepared = false;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    Mock::lastBuffer->prepared = true; Mock::lastCommand->active = false;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    Mock::lastCommand->active = true;
    auto *different = new IOMapper; f.context.mapper = different;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    f.context.mapper = f.mapper; different->release();
    f.context.pinnedBytes = 0;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    f.context.pinnedBytes = 8192; f.context.maxPinnedBytes = 4096;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    f.context.maxPinnedBytes = 8192; f.context.maxAllocationBytes = 4096;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    f.context.maxAllocationBytes = 8192;
    CHECK(Mock::segmentCalls == before);
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == cpu);
    CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok && !pin.cookie);
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
}
static void directPinQuotaAndExtentAdmission() {
    Fixture f; Pin pin {};
    for (uint64_t bytes : {uint64_t(0), uint64_t(4097), uint64_t(12288), UINT64_MAX})
        CHECK(f.backend.pin(f.backend.context, 42, bytes, pin) == Status::Invalid && !pin.cookie);
    CHECK(f.backend.pin(f.backend.context, 0, 4096, pin) == Status::Invalid && !pin.cookie);
    CHECK(f.context.pinnedBytes == 0 && Mock::specCalls == 0 && Mock::allocations.empty());
    CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
    Pin excess {};
    CHECK(f.backend.pin(f.backend.context, 43, 4096, excess) == Status::Invalid && !excess.cookie);
    CHECK(f.context.pinnedBytes == 8192 && resolveDirectPinnedBuffer(f.context, 42, 8192, pin) != nullptr);
    CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok && f.context.pinnedBytes == 0);
}
static void directInspectionRejectsInitiallyAliasedDmaPages() {
    Fixture f; Mock::faults.segmentAlias = 1; Pin pin {};
    // The generic DMA pin/resolver may retain these pages for a reviewed
    // bounce-capable consumer. GuC requires proof of the original CPU pages.
    CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
    CHECK(pin.dmaPages[0] == pin.dmaPages[1] && resolvePinnedBuffer(f.context, 42, 8192, pin) != nullptr);
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    CHECK(f.context.pinnedBytes == 8192 && Mock::commandCompleteCalls == 0);
    CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok);
}
static void directInspectionRejectsChangedPreparedMappings() {
    Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
    void *cpu = kernelBuffer(pin); CHECK(cpu != nullptr);
    void *replacement = std::aligned_alloc(4096, 8192); CHECK(replacement != nullptr);
    const auto allocations = Mock::allocations.size(), objects = Mock::objects.size();
    for (unsigned fault = 0; fault < 29; ++fault) {
        Mock::faults = {};
        switch (fault) {
        case 0: Mock::faults.copyDescriptor = true; break; // Full-buffer bounce.
        case 1: Mock::faults.translatedBounce = 1; break; // Per-page bounce.
        case 2: Mock::faults.iovaShift = 4096; break; // Fresh segments differ from retained IOVA.
        case 3: Mock::faults.preparationID = kIOPreparationIDUnprepared; break;
        case 4: Mock::faults.preparationID = 1; break; // Unsupported preparation ID.
        case 5: Mock::faults.preparationIDChangeCall = Mock::preparationCalls + 2; break;
        case 6: Mock::faults.preparedOffset = 4096; break;
        case 7: Mock::faults.preparedLength = 4096; break;
        case 8: Mock::faults.preparedRangeError = true; break;
        case 9: Mock::faults.preparedRangeChangeCall = Mock::preparedRangeCalls + 2; break;
        case 10: f.mapper->pageSize = 0; break;
        case 11: f.mapper->pageSize = 8192; break;
        case 12: Mock::faults.noCpu = true; break;
        case 13: Mock::faults.cpuOverride = static_cast<uint8_t *>(cpu) + 1; break;
        case 14: Mock::faults.cpuOverride = replacement; break; // Aligned but foreign backing.
        case 15: Mock::faults.physicalFirstMismatch = 0; break;
        case 16: Mock::faults.translatedFirstMismatch = 1; break;
        case 17: Mock::faults.translatedLastMismatch = 1; break;
        case 18: Mock::faults.physicalShort = 1; break;
        case 19: Mock::faults.physicalZero = 1; break;
        case 20: Mock::faults.physicalAlignment = 1; break;
        case 21: Mock::faults.segmentError = 1; break;
        case 22: Mock::faults.segmentCount = 1; break;
        case 23: Mock::faults.segmentLength = 1; break;
        case 24: Mock::faults.segmentOffset = 1; break;
        case 25: Mock::faults.segmentAlignment = 1; break;
        case 26: Mock::faults.segmentWidth = 1; break;
        case 27: Mock::faults.segmentAlias = 1; break;
        default: f.mapper->inactive = true; break;
        }
        CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
        // Inspection failure neither drops cleanup authority nor changes the
        // existing bounce-capable resolver's contract.
        CHECK(pin.cookie && pin.pageCount == 2 && f.context.pinnedBytes == 8192);
        if (fault != 12 && fault != 13 && fault != 14)
            CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == cpu);
        CHECK(Mock::allocations.size() == allocations && Mock::objects.size() == objects);
        CHECK(Mock::syncCalls == 0 && Mock::commandCompleteCalls == 0 && Mock::descriptorCompleteCalls == 0);
        f.mapper->pageSize = 4096; f.mapper->inactive = false;
    }
    Mock::faults = {}; std::free(replacement);
    f.mapper->translations[0x800000] = 0x900000;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
    CHECK(f.mapper->translations[0x800000] == 0x900000); // A fresh walk must not repair the fake map.
    CHECK(resolvePinnedBuffer(f.context, 42, 8192, pin) == cpu);
    f.mapper->translations[0x800000] = 0x100000;
    CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == cpu);
    CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok);
}
static void directInspectionRechecksAuthorityAfterLastTranslation() {
    for (unsigned revoke = 0; revoke < 5; ++revoke) {
        Fixture f; Pin pin {}; CHECK(f.backend.pin(f.backend.context, 42, 8192, pin) == Status::Ok);
        Mock::faults.inspectionContext = &f.context;
        Mock::faults.afterInspectionAddress = 0x801fff;
        switch (revoke) {
        case 0: Mock::faults.afterInspection = [](void *context) {
            static_cast<IOKitContext *>(context)->mapper = nullptr;
        }; break;
        case 1: Mock::faults.afterInspection = [](void *) { Mock::faults.noCpu = true; }; break;
        case 2: Mock::faults.afterInspection = [](void *) { Mock::faults.copyDescriptor = true; }; break;
        case 3: Mock::faults.afterInspection = [](void *context) {
            static_cast<IOKitContext *>(context)->pinnedBytes = 0;
        }; break;
        default: Mock::faults.afterInspection = [](void *context) {
            static_cast<IOKitContext *>(context)->mapper->inactive = true;
        }; break;
        }
        CHECK(resolveDirectPinnedBuffer(f.context, 42, 8192, pin) == nullptr);
        CHECK(Mock::inspectionCallbacks == 1 && Mock::segmentCalls == 4 && Mock::translationCalls == 4);
        CHECK(Mock::commandCompleteCalls == 0 && Mock::descriptorCompleteCalls == 0 && Mock::syncCalls == 0);
        // Fixture-only restoration permits its real production unpin path;
        // the failed inspection itself supplies no retirement proof.
        f.context.mapper = f.mapper; f.mapper->inactive = false;
        f.context.pinnedBytes = 8192; Mock::faults = {};
        CHECK(f.backend.unpin(f.backend.context, pin) == Status::Ok);
    }
}
int main() {
    directInspectionSuccessAndIdentity(); directPinQuotaAndExtentAdmission();
    directInspectionRejectsInitiallyAliasedDmaPages();
    directInspectionRejectsChangedPreparedMappings();
    directInspectionRechecksAuthorityAfterLastTranslation();
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
