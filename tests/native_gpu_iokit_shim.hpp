// Host-only OS boundary emulator. Never include this in the Darwin target.
// Adapter and Session method bodies stay production code; allocation/provider,
// registry, privilege, termination and lock/refcount behavior here is synthetic.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

using UInt32 = uint32_t;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = -1, kIOReturnNotReady = -2,
    kIOReturnBusy = -3, kIOReturnBadArgument = -4, kIOReturnNoMemory = -5,
    kIOReturnNotPrivileged = -6;
constexpr unsigned kIOPCIConfigVendorID = 0, kIOPCIConfigDeviceID = 2;
inline constexpr const char *kIOClientPrivilegeAdministrator = "administrator";
struct HostTask { bool administrator {true}; };
using task_t = HostTask *;
inline HostTask hostCurrentTask {};
inline task_t current_task() { return &hostCurrentTask; }

namespace NativeGpuHost {
inline unsigned heldLocks = 0, liveLocks = 0, releasesUnderLock = 0;
inline void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "host shim: %s\n", message); std::abort(); }
}
}
struct IOLock { bool held {}; };
inline IOLock *IOLockAlloc() { ++NativeGpuHost::liveLocks; return new IOLock; }
inline void IOLockFree(IOLock *lock) {
    NativeGpuHost::require(lock && !lock->held, "freeing a held lock");
    --NativeGpuHost::liveLocks; delete lock;
}
inline void IOLockLock(IOLock *lock) {
    NativeGpuHost::require(lock && !lock->held, "recursive or invalid lock acquisition");
    lock->held = true; ++NativeGpuHost::heldLocks;
}
inline void IOLockUnlock(IOLock *lock) {
    NativeGpuHost::require(lock && lock->held, "unlock without ownership");
    lock->held = false; --NativeGpuHost::heldLocks;
}

class OSObject {
public:
    inline static std::set<const OSObject *> live;
    unsigned refs {1};
    OSObject() { live.insert(this); }
    OSObject(const OSObject &) = delete;
    OSObject &operator=(const OSObject &) = delete;
    virtual ~OSObject() { live.erase(this); }
    virtual void free() { delete this; }
    void retain() {
        NativeGpuHost::require(refs != 0, "retaining a destroyed object"); ++refs;
    }
    void release() {
        NativeGpuHost::require(refs != 0, "unbalanced release");
        if (NativeGpuHost::heldLocks) ++NativeGpuHost::releasesUnderLock;
        if (!--refs) free();
    }
    static bool alive(const OSObject *object) { return live.count(object) != 0; }
};
#define OSDeclareAbstractStructors(cls) public: cls() = default; ~cls() override = default;
#define OSDeclareDefaultStructors(cls) public: cls() = default; ~cls() override = default;
#define OSDefineMetaClassAndAbstractStructors(cls, parent)
#define OSDefineMetaClassAndStructors(cls, parent)
#define OSDynamicCast(cls, object) dynamic_cast<cls *>(object)
class OSDictionary : public OSObject {};
class IOUserClient;
struct IOExternalMethodDispatch {};
struct IOExternalMethodArguments {
    uint32_t scalarInputCount {}, scalarOutputCount {}, asyncReferenceCount {};
    uintptr_t asyncWakePort {};
    void *structureInputDescriptor {}, *structureOutputDescriptor {}, *structureVariableOutputData {};
    const void *structureInput {};
    void *structureOutput {};
    size_t structureInputSize {}, structureOutputSize {};
};
class IOService : public OSObject {
public:
    inline static uint64_t nextRegistryId {100};
    uint64_t registryId {nextRegistryId++};
    bool started {}, terminated {};
    unsigned stops {};
    IOService *provider {};
    virtual bool init(OSDictionary * = nullptr) { return true; }
    virtual bool start(IOService *parent) { started = parent != nullptr; return started; }
    virtual void stop(IOService *) { started = false; ++stops; }
    bool attach(IOService *parent) { if (provider || !parent) return false; provider = parent; return true; }
    void detach(IOService *parent) { if (provider == parent) provider = nullptr; }
    IOService *getProvider() const { return provider; }
    uint64_t getRegistryEntryID() const { return registryId; }
    bool terminate() { terminated = true; return true; }
    virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) {
        return kIOReturnNotReady;
    }
};
class IOPCIDevice : public IOService {
public:
    uint16_t vendor {0x8086}, device {0x7d41};
    uint16_t configRead16(unsigned reg) const {
        return reg == kIOPCIConfigVendorID ? vendor : reg == kIOPCIConfigDeviceID ? device : UINT16_MAX;
    }
};
class IOUserClient : public IOService {
public:
    inline static IOUserClient *lastCreated {};
    inline static unsigned liveClients {}, freedClients {};
    IOUserClient() { lastCreated = this; ++liveClients; }
    ~IOUserClient() override {
        if (lastCreated == this) lastCreated = nullptr;
        --liveClients; ++freedClients;
    }
    static IOReturn clientHasPrivilege(task_t task, const char *) {
        return task && task->administrator ? kIOReturnSuccess : kIOReturnNotPrivileged;
    }
    virtual bool initWithTask(task_t task, void *, UInt32, OSDictionary *properties) {
        return task && IOService::init(properties);
    }
    virtual IOReturn clientClose() { return kIOReturnSuccess; }
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch * = nullptr,
                                   OSObject * = nullptr, void * = nullptr) { return kIOReturnNotReady; }
};
