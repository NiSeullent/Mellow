// Host-only OS lifetime boundary for unchanged production DMA adapters.
// No physical DMA, cache, IOMMU, provider locking, or GPU operation is modeled.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using vm_size_t = size_t;
using IOReturn = int;
using IODirection = unsigned;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotReady = -1, kIOReturnError = -2;
constexpr IODirection kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3;
constexpr IOOptionBits kIOMemoryMapperNone = 0x800;
constexpr unsigned kIOPCIConfigVendorID = 0;
inline void *kernel_task = reinterpret_cast<void *>(uintptr_t(1));

class OSObject;
class IOBufferMemoryDescriptor;
class IODMACommand;
namespace NativeMemoryShim {
enum class Kind { Service, Mapper, Buffer, Command, Other };
struct Faults {
    unsigned mallocFailureCall {}, mallocCalls {};
    bool bufferFactory {}, noCpu {}, descriptorPrepare {}, descriptorPrepareAcquires {true};
    bool descriptorComplete {}, descriptorCompleteConsumes {true};
    bool commandFactory {}, setDescriptor {}, setDescriptorAcquires {true};
    bool commandPrepare {}, commandPrepareAcquires {true};
    bool commandComplete {}, commandCompleteConsumes {true}, clearDescriptor {}, synchronize {};
    void *syncContext {};
    void (*afterSync)(void *, IOOptionBits) {};
    int segmentError {-1}, segmentCount {-1}, segmentLength {-1}, segmentOffset {-1},
        segmentAlignment {-1}, segmentWidth {-1}, physicalShort {-1}, translatedBounce {-1};
    bool copyDescriptor {};
};
inline Faults faults;
inline std::set<OSObject *> objects;
inline std::map<void *, size_t> allocations;
inline std::vector<std::string> events;
inline bool forcingCleanup {};
inline unsigned unsafeClearCalls {}, forbiddenMapperLookups {}, commandCompleteCalls {},
    descriptorCompleteCalls {}, syncCalls {}, specCalls {}, prematureDestroy {};
inline IOBufferMemoryDescriptor *lastBuffer {};
inline IODMACommand *lastCommand {};
inline void reset() {
    faults = {}; events.clear(); unsafeClearCalls = forbiddenMapperLookups = 0;
    commandCompleteCalls = descriptorCompleteCalls = syncCalls = specCalls = prematureDestroy = 0;
    lastBuffer = nullptr; lastCommand = nullptr;
}
inline void event(const char *name) { events.emplace_back(name); }
inline bool ordered(const char *first, const char *second) {
    size_t a = events.size(), b = events.size();
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i] == first && a == events.size()) a = i;
        if (events[i] == second && b == events.size()) b = i;
    }
    return a < b && b < events.size();
}
void disposeQuarantinedHostWorld();
}
class OSObject {
public:
    unsigned refs {1};
    NativeMemoryShim::Kind kind;
    explicit OSObject(NativeMemoryShim::Kind k = NativeMemoryShim::Kind::Other) : kind(k) {
        NativeMemoryShim::objects.insert(this);
    }
    virtual ~OSObject() { NativeMemoryShim::objects.erase(this); }
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) delete this; }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
class IOService : public OSObject {
public:
    IOService *provider {};
    bool inactive {};
    std::map<std::string, OSObject *> properties;
    IOService() : OSObject(NativeMemoryShim::Kind::Service) {}
    ~IOService() override { for (auto &entry : properties) entry.second->release(); }
    bool isInactive() const { return inactive; }
    IOService *getProvider() const { return provider; }
    OSObject *copyProperty(const char *key) {
        auto found = properties.find(key);
        if (found == properties.end()) return nullptr;
        found->second->retain(); return found->second;
    }
    void fixtureProperty(const char *key, OSObject *value) {
        auto found = properties.find(key);
        if (found != properties.end()) { found->second->release(); properties.erase(found); }
        if (value) { value->retain(); properties.emplace(key, value); }
    }
};
class IOPCIDevice : public IOService {
public:
    uint32_t physicalId {0x7d418086};
    uint64_t registryId {123};
    IOService *openOwner {};
    unsigned configReads {};
    uint32_t configRead32(IOByteCount offset) { ++configReads; return offset == 0 ? physicalId : 0xffffffff; }
    uint64_t getRegistryEntryID() const { return registryId; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
};
class IOMapper : public IOService {
public:
    std::map<uint64_t, uint64_t> translations;
    IOMapper() { kind = NativeMemoryShim::Kind::Mapper; }
    uint64_t mapToPhysicalAddress(uint64_t address) {
        NativeMemoryShim::event("mapper.translate");
        auto found = translations.find(address);
        if (found == translations.end()) return 0;
        const int index = static_cast<int>((address - 0x800000) / 4096);
        return found->second + (index == NativeMemoryShim::faults.translatedBounce ? 4096 : 0);
    }
    static IOMapper *copyMapperForDevice(IOService *) {
        ++NativeMemoryShim::forbiddenMapperLookups; return nullptr;
    }
};
class IOMemoryDescriptor : public OSObject {
public:
    IOMemoryDescriptor() : OSObject(NativeMemoryShim::Kind::Buffer) {}
    virtual uint64_t getPhysicalSegment(IOByteCount, IOByteCount *, IOOptionBits) = 0;
};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    uint8_t *cpu {};
    uint64_t bytes {};
    bool prepared {};
    explicit IOBufferMemoryDescriptor(uint64_t length) : bytes(length) {
        cpu = static_cast<uint8_t *>(std::aligned_alloc(4096, static_cast<size_t>(bytes)));
        if (!cpu) std::abort();
        std::memset(cpu, 0xa5, static_cast<size_t>(bytes));
    }
    ~IOBufferMemoryDescriptor() override {
        if (prepared && !NativeMemoryShim::forcingCleanup) ++NativeMemoryShim::prematureDestroy;
        if (NativeMemoryShim::lastBuffer == this) NativeMemoryShim::lastBuffer = nullptr;
        NativeMemoryShim::event("memory.destroy"); std::free(cpu);
    }
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *task, IOOptionBits options,
            vm_size_t length, uint64_t alignment) {
        if (task != kernel_task || options != kIODirectionInOut || alignment != 4096 || !length || (length & 4095)) std::abort();
        NativeMemoryShim::event("memory.factory");
        if (NativeMemoryShim::faults.bufferFactory) return nullptr;
        auto *memory = new IOBufferMemoryDescriptor(length);
        NativeMemoryShim::lastBuffer = memory; return memory;
    }
    void *getBytesNoCopy() const { return NativeMemoryShim::faults.noCpu ? nullptr : cpu; }
    uint64_t getLength() const { return bytes; }
    IOReturn prepare(IODirection direction) {
        if (direction != kIODirectionInOut) std::abort();
        NativeMemoryShim::event("memory.prepare");
        prepared = !NativeMemoryShim::faults.descriptorPrepare || NativeMemoryShim::faults.descriptorPrepareAcquires;
        return NativeMemoryShim::faults.descriptorPrepare ? kIOReturnError : kIOReturnSuccess;
    }
    IOReturn complete(IODirection direction) {
        if (direction != kIODirectionInOut) std::abort();
        ++NativeMemoryShim::descriptorCompleteCalls; NativeMemoryShim::event("memory.complete");
        if (!prepared) return kIOReturnNotReady;
        if (NativeMemoryShim::faults.descriptorComplete) {
            if (NativeMemoryShim::faults.descriptorCompleteConsumes) prepared = false;
            return kIOReturnError;
        }
        prepared = false; return kIOReturnSuccess;
    }
    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, IOOptionBits options) override {
        if (options != kIOMemoryMapperNone || !prepared || offset >= bytes) return 0;
        const auto index = static_cast<int>(offset / 4096);
        if (length) *length = index == NativeMemoryShim::faults.physicalShort ? 2048 : 4096;
        return 0x100000 + uint64_t(index) * 8192;
    }
};
class IODMACommand : public OSObject {
public:
    struct Segment64 { uint64_t fIOVMAddr {}, fLength {}; };
    enum MappingOptions { kMapped = 0 };
    static constexpr int OutputHost64 = 64;
    IOMapper *mapper {};
    IOBufferMemoryDescriptor *memory {};
    bool active {};
    uint8_t bits {};
    uint64_t capacity {};
    explicit IODMACommand(IOMapper *m, uint8_t b, uint64_t n)
        : OSObject(NativeMemoryShim::Kind::Command), mapper(m), bits(b), capacity(n) { mapper->retain(); }
    ~IODMACommand() override {
        if ((active || memory) && !NativeMemoryShim::forcingCleanup) ++NativeMemoryShim::prematureDestroy;
        if (memory) memory->release();
        mapper->release(); NativeMemoryShim::event("dma.destroy");
        if (NativeMemoryShim::lastCommand == this) NativeMemoryShim::lastCommand = nullptr;
    }
    static IODMACommand *withSpecification(int output, UInt8 bits, UInt64 segmentSize,
            MappingOptions options, UInt64 bytes, UInt32 alignment, IOMapper *mapper) {
        ++NativeMemoryShim::specCalls; NativeMemoryShim::event("dma.factory");
        if (output != OutputHost64 || bits < 39 || bits > 48 || segmentSize != 4096 ||
            options != kMapped || alignment != 4096 || !mapper) std::abort();
        if (NativeMemoryShim::faults.commandFactory) return nullptr;
        auto *command = new IODMACommand(mapper, bits, bytes);
        NativeMemoryShim::lastCommand = command; return command;
    }
    IOReturn setMemoryDescriptor(const IOMemoryDescriptor *descriptor, bool autoPrepare) {
        if (autoPrepare || memory || active) std::abort();
        NativeMemoryShim::event("dma.set");
        if (!NativeMemoryShim::faults.setDescriptor || NativeMemoryShim::faults.setDescriptorAcquires) {
            memory = dynamic_cast<IOBufferMemoryDescriptor *>(const_cast<IOMemoryDescriptor *>(descriptor));
            if (!memory) std::abort();
            memory->retain();
        }
        return NativeMemoryShim::faults.setDescriptor ? kIOReturnError : kIOReturnSuccess;
    }
    IOReturn prepare(UInt64 offset, UInt64 length) {
        NativeMemoryShim::event("dma.prepare");
        if (!memory || !memory->prepared || offset || length != capacity) std::abort();
        active = !NativeMemoryShim::faults.commandPrepare || NativeMemoryShim::faults.commandPrepareAcquires;
        return NativeMemoryShim::faults.commandPrepare ? kIOReturnError : kIOReturnSuccess;
    }
    IOReturn complete(bool invalidate, bool synchronize) {
        if (!invalidate || !synchronize) std::abort();
        ++NativeMemoryShim::commandCompleteCalls; NativeMemoryShim::event("dma.complete");
        if (!active) return kIOReturnNotReady;
        if (NativeMemoryShim::faults.commandComplete) {
            if (NativeMemoryShim::faults.commandCompleteConsumes) active = false;
            return kIOReturnError;
        }
        active = false; return kIOReturnSuccess;
    }
    IOReturn clearMemoryDescriptor(bool autoComplete) {
        NativeMemoryShim::event("dma.clear");
        if (autoComplete) {
            ++NativeMemoryShim::unsafeClearCalls;
            // Deliberately reproduce XNU: completion errors are discarded.
            if (active) (void)complete(true, true);
        } else if (active) return kIOReturnNotReady;
        if (NativeMemoryShim::faults.clearDescriptor) return kIOReturnError;
        if (memory) { memory->release(); memory = nullptr; }
        return kIOReturnSuccess;
    }
    const IOMemoryDescriptor *getMemoryDescriptor() const { return memory; }
    IOMemoryDescriptor *getIOMemoryDescriptor() const {
        return NativeMemoryShim::faults.copyDescriptor ? reinterpret_cast<IOMemoryDescriptor *>(uintptr_t(1)) : memory;
    }
    IOReturn synchronize(IOOptionBits direction) {
        ++NativeMemoryShim::syncCalls;
        NativeMemoryShim::event(direction == kIODirectionOut ? "sync.out" : "sync.in");
        if (direction != kIODirectionIn && direction != kIODirectionOut) std::abort();
        if (!active || NativeMemoryShim::faults.synchronize) return kIOReturnError;
        if (NativeMemoryShim::faults.afterSync)
            NativeMemoryShim::faults.afterSync(NativeMemoryShim::faults.syncContext, direction);
        return kIOReturnSuccess;
    }
    IOReturn gen64IOVMSegments(UInt64 *offset, Segment64 *segment, UInt32 *count) {
        if (!active || !offset || !segment || !count || *count != 1) std::abort();
        NativeMemoryShim::event("dma.segment");
        const int index = static_cast<int>(*offset / 4096);
        if (index == NativeMemoryShim::faults.segmentError) return kIOReturnError;
        segment->fIOVMAddr = 0x800000 + uint64_t(index) * 4096;
        segment->fLength = index == NativeMemoryShim::faults.segmentLength ? 2048 : 4096;
        *count = index == NativeMemoryShim::faults.segmentCount ? 0 : 1;
        *offset += index == NativeMemoryShim::faults.segmentOffset ? 2048 : 4096;
        if (index == NativeMemoryShim::faults.segmentAlignment) ++segment->fIOVMAddr;
        if (index == NativeMemoryShim::faults.segmentWidth) segment->fIOVMAddr = 1ULL << bits;
        mapper->translations[segment->fIOVMAddr] = 0x100000 + uint64_t(index) * 8192;
        return kIOReturnSuccess;
    }
};
inline void *IOMalloc(size_t size) {
    ++NativeMemoryShim::faults.mallocCalls;
    if (NativeMemoryShim::faults.mallocFailureCall == NativeMemoryShim::faults.mallocCalls) return nullptr;
    void *result = std::malloc(size); if (result) NativeMemoryShim::allocations.emplace(result, size);
    return result;
}
inline void IOFree(void *address, size_t size) {
    auto found = NativeMemoryShim::allocations.find(address);
    if (found == NativeMemoryShim::allocations.end() || found->second != size) std::abort();
    NativeMemoryShim::allocations.erase(found); std::free(address);
}
namespace NativeMemoryShim {
inline void disposeQuarantinedHostWorld() {
    // Test teardown only, after all assertions and adapter/context lifetimes.
    // This does not invoke production recovery or imply actual DMA retirement.
    forcingCleanup = true;
    for (Kind kind : {Kind::Command, Kind::Buffer, Kind::Service, Kind::Mapper, Kind::Other}) {
        for (;;) {
            OSObject *next = nullptr;
            for (auto *object : objects) if (object->kind == kind) { next = object; break; }
            if (!next) break;
            delete next;
        }
    }
    for (auto &allocation : allocations) std::free(allocation.first);
    allocations.clear(); forcingCleanup = false;
}
}
