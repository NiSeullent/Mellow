// Explicit host-only IOKit allocation/provider boundary. Never a Darwin target.
// Production NvidiaMmioIOKit.cpp is compiled unchanged against these types.
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
constexpr uint32_t kIOPCIConfigVendorID = 0, kIOPCIConfigCommand = 4,
    kIOPCIConfigStatus = 6, kIOPCIConfigRevisionID = 8,
    kIOPCIConfigBaseAddress0 = 0x10, kIOPCIConfigBaseAddress1 = 0x14;
constexpr unsigned kIOPCIPowerManagementCapability = 1,
    kIOPCIDeviceOnState = 2, kIOPCI32BitMemorySpace = 2,
    kIOPCI64BitMemorySpace = 3;
constexpr IOOptionBits kIOMapInhibitCache = 0x200, kIOMemoryMapperNone = 0x200000;
inline constexpr const char *gIODeviceMemoryKey = "IODeviceMemory";
inline constexpr const char *kIOPCITunnelledKey = "IOPCITunnelled";

namespace NativeNvidiaShim {
inline unsigned aliveObjects = 0, aliveMaps = 0;
}
class OSObject {
public:
    unsigned refs {1};
    OSObject() { ++NativeNvidiaShim::aliveObjects; }
    virtual ~OSObject() { --NativeNvidiaShim::aliveObjects; }
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) delete this; }
};
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)

class OSArray : public OSObject {
public:
    std::vector<OSObject *> objects;
    ~OSArray() override { for (auto *object : objects) object->release(); }
    void add(OSObject *object) { object->retain(); objects.push_back(object); }
    unsigned getCount() const { return static_cast<unsigned>(objects.size()); }
    OSObject *getObject(unsigned index) const {
        return index < objects.size() ? objects[index] : nullptr;
    }
};
class IOService : public OSObject {
public:
    IOService *provider {};
    bool inactive {};
    unsigned powerState {kIOPCIDeviceOnState};
    std::map<std::string, OSObject *> properties;
    ~IOService() override { for (auto &item : properties) item.second->release(); }
    IOService *getProvider() const { return provider; }
    bool isInactive() const { return inactive; }
    unsigned getPowerState() const { return powerState; }
    OSObject *copyProperty(const char *name) {
        auto found = properties.find(name);
        if (found == properties.end()) return nullptr;
        found->second->retain(); return found->second;
    }
    void fixtureProperty(const char *name, OSObject *value) {
        auto found = properties.find(name);
        if (found != properties.end()) { found->second->release(); properties.erase(found); }
        if (value) { value->retain(); properties.emplace(name, value); }
    }
};

class IOMemoryDescriptor;
class IOMemoryMap : public OSObject {
public:
    IOMemoryDescriptor *descriptor {};
    uintptr_t virtualAddress {};
    uint64_t physical {}, length {};
    IOMemoryMap(IOMemoryDescriptor *, uintptr_t, uint64_t, uint64_t);
    ~IOMemoryMap() override;
    uintptr_t getVirtualAddress() const { return virtualAddress; }
    uint64_t getPhysicalAddress() const { return physical; }
    uint64_t getLength() const { return length; }
};
class IOMemoryDescriptor : public OSObject {
public:
    // Deliberately not an IODeviceMemory subclass: Apple PCI memory descriptors
    // can be IOGeneralMemoryDescriptor instances created by withAddressRange.
    alignas(8) uint32_t registers[4] {0x172000a1, 0, 0, 0};
    uint64_t physical {0xa0000000}, length {16}, contiguous {16};
    uint32_t tag {(kIOPCI32BitMemorySpace << 24) | (1U << 16) | 0x10U};
    bool failMap {}, overrideVirtual {}, overridePhysical {}, overrideLength {};
    uintptr_t mapVirtual {};
    uint64_t mapPhysical {}, mapLength {};
    unsigned mapCalls {};
    IOOptionBits lastMapOptions {};
    IOMemoryMap *lastMap {}; // Borrowed; valid only while the adapter holds it.
    uint32_t getTag() const { return tag; }
    uint64_t getLength() const { return length; }
    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *bytes, IOOptionBits options) {
        if (options != kIOMemoryMapperNone) std::abort();
        if (bytes) *bytes = offset < contiguous ? contiguous - offset : 0;
        return offset < length ? physical + offset : 0;
    }
    IOMemoryMap *map(IOOptionBits options) {
        ++mapCalls; lastMapOptions = options;
        if (failMap) return nullptr;
        lastMap = new IOMemoryMap(this,
            overrideVirtual ? mapVirtual : reinterpret_cast<uintptr_t>(registers),
            overridePhysical ? mapPhysical : physical,
            overrideLength ? mapLength : length);
        return lastMap;
    }
};
inline IOMemoryMap::IOMemoryMap(IOMemoryDescriptor *d, uintptr_t v, uint64_t p, uint64_t n)
    : descriptor(d), virtualAddress(v), physical(p), length(n) {
    descriptor->retain(); ++NativeNvidiaShim::aliveMaps;
}
inline IOMemoryMap::~IOMemoryMap() {
    descriptor->lastMap = nullptr;
    descriptor->release(); --NativeNvidiaShim::aliveMaps;
}

class IOPCIDevice : public IOService {
public:
    std::array<uint8_t, 256> config {};
    uint64_t registryId {123};
    uint8_t bus {1}, slot {}, function {};
    IOService *openOwner {};
    unsigned idReads {}, configReads {}, forbiddenCalls {};
    void (*beforeIdRead)(IOPCIDevice &, unsigned) {};
    IOMemoryDescriptor *fixtureDescriptor {};
    uint64_t getRegistryEntryID() const { return registryId; }
    uint8_t getBusNumber() const { return bus; }
    uint8_t getDeviceNumber() const { return slot; }
    uint8_t getFunctionNumber() const { return function; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
    void fixture8(unsigned offset, uint8_t value) { config.at(offset) = value; }
    void fixture16(unsigned offset, uint16_t value) {
        fixture8(offset, static_cast<uint8_t>(value)); fixture8(offset + 1, static_cast<uint8_t>(value >> 8));
    }
    void fixture32(unsigned offset, uint32_t value) {
        fixture16(offset, static_cast<uint16_t>(value)); fixture16(offset + 2, static_cast<uint16_t>(value >> 16));
    }
    uint8_t configRead8(IOByteCount offset) {
        ++configReads; return offset < config.size() ? config[static_cast<size_t>(offset)] : 0xff;
    }
    uint16_t configRead16(IOByteCount offset) {
        ++configReads;
        if (offset > config.size() - 2) return 0xffff;
        return uint16_t(config[static_cast<size_t>(offset)]) |
            uint16_t(config[static_cast<size_t>(offset + 1)]) << 8;
    }
    uint32_t configRead32(IOByteCount offset) {
        ++configReads;
        if (offset == kIOPCIConfigVendorID && beforeIdRead) beforeIdRead(*this, ++idReads);
        else if (offset == kIOPCIConfigVendorID) ++idReads;
        if (offset > config.size() - 4) return 0xffffffff;
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(config[static_cast<size_t>(offset + i)]) << (i * 8);
        return value;
    }
    // These accessors may alter tunnel power state in real IOKit. Their use
    // fails the harness even if a future edit happens to compile against them.
    IOMemoryMap *mapDeviceMemoryWithRegister(uint8_t, IOOptionBits) { ++forbiddenCalls; return nullptr; }
    IOMemoryDescriptor *getDeviceMemoryWithRegister(uint8_t) { ++forbiddenCalls; return nullptr; }
    uint32_t findPCICapability(uint8_t, uint8_t *) { ++forbiddenCalls; return 0; }
};
