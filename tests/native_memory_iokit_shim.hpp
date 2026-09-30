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
using UInt16 = uint16_t;
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
constexpr uint64_t kIOPreparationIDUnprepared = 0, kIOPreparationIDAlwaysPrepared = 2;
constexpr unsigned kIOPCIConfigVendorID = 0;
inline void *kernel_task = reinterpret_cast<void *>(uintptr_t(1));

class OSObject;
class IOBufferMemoryDescriptor;
class IODMACommand;
class IOMemoryMap;
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
    void *cpuOverride {};
    uint64_t preparationID {17}, preparedOffset {}, preparedLength {UINT64_MAX}, iovaShift {};
    unsigned preparationIDChangeCall {}, preparedRangeChangeCall {};
    bool alwaysPrepared {}, preparedRangeError {};
    void *inspectionContext {};
    void (*afterInspection)(void *) {};
    uint64_t afterInspectionAddress {};
    int segmentError {-1}, segmentCount {-1}, segmentLength {-1}, segmentOffset {-1},
        segmentAlignment {-1}, segmentWidth {-1}, segmentAlias {-1}, physicalShort {-1},
        physicalFirstMismatch {-1}, physicalLastMismatch {-1}, physicalZero {-1}, physicalAlignment {-1},
        translatedBounce {-1}, translatedFirstMismatch {-1}, translatedLastMismatch {-1};
    bool copyDescriptor {};
};
inline Faults faults;
inline std::set<OSObject *> objects;
inline std::map<void *, size_t> allocations;
inline std::vector<std::string> events;
inline bool forcingCleanup {};
inline unsigned unsafeClearCalls {}, forbiddenMapperLookups {}, commandCompleteCalls {},
    descriptorCompleteCalls {}, syncCalls {}, specCalls {}, prematureDestroy {},
    preparationCalls {}, preparedRangeCalls {}, physicalCalls {}, translationCalls {}, segmentCalls {}, inspectionCallbacks {};
inline IOBufferMemoryDescriptor *lastBuffer {};
inline IODMACommand *lastCommand {};
inline void reset() {
    faults = {}; events.clear(); unsafeClearCalls = forbiddenMapperLookups = 0;
    commandCompleteCalls = descriptorCompleteCalls = syncCalls = specCalls = prematureDestroy = 0;
    preparationCalls = preparedRangeCalls = physicalCalls = translationCalls = segmentCalls = inspectionCallbacks = 0;
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
    OSObject *getProperty(const char *key) const {
        auto found = properties.find(key);
        return found == properties.end() ? nullptr : found->second;
    }
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
    uint8_t pciConfig[256] {};
    uint8_t busNumber {}, deviceNumber {2}, functionNumber {};
    IOService *openOwner {};
    unsigned configReads {};
    IOPCIDevice() {
        pciConfig[4] = 6; pciConfig[6] = 0x10; pciConfig[0x34] = 0x40;
        pciConfig[0x40] = 1; // PM capability, no next capability; D0 PMCSR.
    }
    uint8_t fixtureConfigByte(IOByteCount offset) const {
        if (offset < 4) return static_cast<uint8_t>(physicalId >> (offset * 8));
        return offset < sizeof(pciConfig) ? pciConfig[offset] : uint8_t(0xff);
    }
    uint8_t configRead8(IOByteCount offset) { ++configReads; return fixtureConfigByte(offset); }
    uint16_t configRead16(IOByteCount offset) {
        ++configReads;
        return static_cast<uint16_t>(uint16_t(fixtureConfigByte(offset)) |
            (uint16_t(fixtureConfigByte(offset + 1)) << 8));
    }
    uint32_t configRead32(IOByteCount offset) {
        ++configReads;
        return uint32_t(fixtureConfigByte(offset)) | (uint32_t(fixtureConfigByte(offset + 1)) << 8) |
            (uint32_t(fixtureConfigByte(offset + 2)) << 16) | (uint32_t(fixtureConfigByte(offset + 3)) << 24);
    }
    uint8_t getBusNumber() const { return busNumber; }
    uint8_t getDeviceNumber() const { return deviceNumber; }
    uint8_t getFunctionNumber() const { return functionNumber; }
    uint64_t getRegistryEntryID() const { return registryId; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
};
class IOMapper : public IOService {
public:
    uint64_t pageSize {4096};
    std::map<uint64_t, uint64_t> translations;
    IOMapper() { kind = NativeMemoryShim::Kind::Mapper; }
    uint64_t getPageSize() const { return pageSize; }
    uint64_t mapToPhysicalAddress(uint64_t address) {
        ++NativeMemoryShim::translationCalls;
        NativeMemoryShim::event("mapper.translate");
        const uint64_t page = address & ~uint64_t(4095), offset = address & 4095;
        auto found = translations.find(page);
        if (found == translations.end()) return 0;
        const int index = static_cast<int>((page - 0x800000) / 4096);
        uint64_t result = found->second + offset;
        if (index == NativeMemoryShim::faults.translatedBounce ||
            (!offset && index == NativeMemoryShim::faults.translatedFirstMismatch) ||
            (offset == 4095 && index == NativeMemoryShim::faults.translatedLastMismatch)) result += 4096;
        if (NativeMemoryShim::faults.afterInspection &&
            address == NativeMemoryShim::faults.afterInspectionAddress) {
            ++NativeMemoryShim::inspectionCallbacks;
            NativeMemoryShim::faults.afterInspection(NativeMemoryShim::faults.inspectionContext);
        }
        return result;
    }
    static IOMapper *copyMapperForDevice(IOService *) {
        ++NativeMemoryShim::forbiddenMapperLookups; return nullptr;
    }
};
class IOMemoryDescriptor : public OSObject {
public:
    IOMemoryDescriptor() : OSObject(NativeMemoryShim::Kind::Buffer) {}
    virtual uint64_t getPreparationID() { return kIOPreparationIDUnprepared; }
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
    void *getBytesNoCopy() const {
        return NativeMemoryShim::faults.noCpu ? nullptr :
            NativeMemoryShim::faults.cpuOverride ? NativeMemoryShim::faults.cpuOverride : cpu;
    }
    uint64_t getLength() const { return bytes; }
    uint64_t getPreparationID() override {
        ++NativeMemoryShim::preparationCalls;
        NativeMemoryShim::event("memory.preparation-id");
        if (!prepared) return kIOPreparationIDUnprepared;
        if (NativeMemoryShim::faults.alwaysPrepared) return kIOPreparationIDAlwaysPrepared;
        return NativeMemoryShim::faults.preparationID +
            (NativeMemoryShim::faults.preparationIDChangeCall &&
             NativeMemoryShim::preparationCalls >= NativeMemoryShim::faults.preparationIDChangeCall ? 1 : 0);
    }
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
        ++NativeMemoryShim::physicalCalls; NativeMemoryShim::event("memory.physical");
        if (options != kIOMemoryMapperNone || !prepared || offset >= bytes) return 0;
        const auto index = static_cast<int>(offset / 4096);
        const uint64_t inPage = offset & 4095;
        if (length) *length = index == NativeMemoryShim::faults.physicalShort ? 2048 : 4096 - inPage;
        if (index == NativeMemoryShim::faults.physicalZero) return 0;
        uint64_t result = 0x100000 + uint64_t(index) * 8192 + inPage;
        if (index == NativeMemoryShim::faults.physicalAlignment) ++result;
        if ((!inPage && index == NativeMemoryShim::faults.physicalFirstMismatch) ||
            (inPage == 4095 && index == NativeMemoryShim::faults.physicalLastMismatch)) result += 4096;
        return result;
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
    std::set<int> mappedIndices;
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
    IOReturn getPreparedOffsetAndLength(UInt64 *offset, UInt64 *length) {
        ++NativeMemoryShim::preparedRangeCalls; NativeMemoryShim::event("dma.prepared-range");
        if (!active || !offset || !length || NativeMemoryShim::faults.preparedRangeError) return kIOReturnError;
        *offset = NativeMemoryShim::faults.preparedOffset;
        *length = NativeMemoryShim::faults.preparedLength == UINT64_MAX ? capacity : NativeMemoryShim::faults.preparedLength;
        if (NativeMemoryShim::faults.preparedRangeChangeCall &&
            NativeMemoryShim::preparedRangeCalls >= NativeMemoryShim::faults.preparedRangeChangeCall) *length = capacity - 4096;
        return kIOReturnSuccess;
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
        ++NativeMemoryShim::segmentCalls; NativeMemoryShim::event("dma.segment");
        const int index = static_cast<int>(*offset / 4096);
        if (index == NativeMemoryShim::faults.segmentError) return kIOReturnError;
        segment->fIOVMAddr = 0x800000 + uint64_t(index) * 4096 + NativeMemoryShim::faults.iovaShift;
        if (index == NativeMemoryShim::faults.segmentAlias) segment->fIOVMAddr -= 4096;
        segment->fLength = index == NativeMemoryShim::faults.segmentLength ? 2048 : 4096;
        *count = index == NativeMemoryShim::faults.segmentCount ? 0 : 1;
        *offset += index == NativeMemoryShim::faults.segmentOffset ? 2048 : 4096;
        if (index == NativeMemoryShim::faults.segmentAlignment) ++segment->fIOVMAddr;
        if (index == NativeMemoryShim::faults.segmentWidth) segment->fIOVMAddr = 1ULL << bits;
        // Segment inspection cannot repair or install a DMA translation. Only
        // the first walk of this prepared command establishes the fake map.
        if (mappedIndices.insert(index).second)
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
