// SPDX-License-Identifier: MIT
#include "NativeGpuIOKit.hpp"
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

OSDefineMetaClassAndAbstractStructors(MellowNativeGpu, IOService)
OSDefineMetaClassAndStructors(MellowNativeGpuClient, IOUserClient)
namespace NG = MellowNativeGpuKernel;

bool MellowNativeGpu::init(OSDictionary *properties) {
    if (!IOService::init(properties)) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}
void MellowNativeGpu::free() {
    // Each active/uncertain client retains this owner. Its reciprocal resource
    // lease is released only by successful native close, before this destructor.
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    IOService::free();
}
MellowNativeGpuStatus MellowNativeGpu::identity(void *opaque, uint64_t owner, NG::Identity &info) {
    auto &service = *static_cast<MellowNativeGpu *>(opaque);
    auto &driver = service.activeOperations();
    auto result = driver.identity(driver.opaque, owner, info);
    if (result != MellowNativeGpuStatusOk) return result;
    auto *pci = OSDynamicCast(IOPCIDevice, service.getProvider());
    // Registry identities refer to this service and its actual direct PCI
    // provider. A device name/property or caller-supplied number is insufficient.
    if (!pci || info.serviceRegistryId != service.getRegistryEntryID() ||
        info.physicalPciRegistryId != pci->getRegistryEntryID() ||
        info.vendorId != pci->configRead16(kIOPCIConfigVendorID) ||
        info.deviceId != pci->configRead16(kIOPCIConfigDeviceID))
        return MellowNativeGpuStatusWrongIdentity;
    return MellowNativeGpuStatusOk;
}
uint64_t MellowNativeGpu::now(void *opaque) {
    auto &service = *static_cast<MellowNativeGpu *>(opaque);
    auto &driver = service.activeOperations();
    return driver.nowMicros(driver.opaque);
}
MellowNativeGpuStatus MellowNativeGpu::submit(void *opaque, uint64_t owner, uint32_t nonce, uint32_t count,
                                           uint64_t deadline, NG::Job &job) {
    auto &service = *static_cast<MellowNativeGpu *>(opaque);
    auto &driver = service.activeOperations();
    return driver.submit(driver.opaque, owner, nonce, count, deadline, job);
}
MellowNativeGpuStatus MellowNativeGpu::poll(void *opaque, uint64_t owner, const NG::Job &job, NG::Observation &observation) {
    auto &service = *static_cast<MellowNativeGpu *>(opaque);
    auto &driver = service.activeOperations();
    return driver.poll(driver.opaque, owner, job, observation);
}
MellowNativeGpuStatus MellowNativeGpu::readback(void *opaque, uint64_t owner, const NG::Job &job,
                                             uint32_t *words, uint32_t count) {
    auto &service = *static_cast<MellowNativeGpu *>(opaque);
    auto &driver = service.activeOperations();
    return driver.readback(driver.opaque, owner, job, words, count);
}
MellowNativeGpuStatus MellowNativeGpu::close(void *opaque, uint64_t owner, const NG::Job &job) {
    auto &service = *static_cast<MellowNativeGpu *>(opaque);
    auto &driver = service.activeOperations();
    return driver.close(driver.opaque, owner, job);
}
NG::DriverOps MellowNativeGpu::forwardingOperations() {
    return {this, identity, now, submit, poll, readback, close};
}
bool MellowNativeGpu::openNative(MellowNativeGpuClient *client, NG::Session &session) {
    if (!lock_ || !client) return false;
    MellowNativeGpuClient *released = nullptr;
    IOLockLock(lock_);
    bool opened = false;
    if (!client_ && nextOwner_ && nextOwner_ != UINT64_MAX) {
        // Burn the identity even when prepare fails. Never reuse an owner whose
        // allocation or GPU acceptance could have been partially observed.
        preparingOwner_ = nextOwner_++;
        preparing_ = true;
        candidate_ = {};
        client_ = client;
        client_->retain();
        quarantine_ = true;
        const auto prepared = prepareNativeDriver(preparingOwner_, candidate_);
        if (prepared == MellowNativeGpuStatusOk && candidate_.identity && candidate_.nowMicros &&
            candidate_.submit && candidate_.poll && candidate_.readback && candidate_.close &&
            session.initialize(preparingOwner_, forwardingOperations()) == MellowNativeGpuStatusOk) {
            driver_ = candidate_; candidate_ = {};
            preparing_ = false; preparingOwner_ = 0; quarantine_ = false;
            opened = true;
        } else if (abortNativeDriver(preparingOwner_) == MellowNativeGpuStatusOk) {
            released = client_; client_ = nullptr;
            candidate_ = {}; preparing_ = false; preparingOwner_ = 0; quarantine_ = false;
        }
    }
    IOLockUnlock(lock_);
    if (released) released->release();
    return opened;
}
IOReturn MellowNativeGpu::callNative(MellowNativeGpuClient *client, NG::Session &session, uint32_t selector,
                                   const MellowNativeGpuRequest &request, MellowNativeGpuReply &reply) {
    if (!lock_) return kIOReturnNotReady;
    IOLockLock(lock_);
    if (client_ != client || preparing_) { IOLockUnlock(lock_); return kIOReturnNotReady; }
    const auto result = session.call(selector, request, reply);
    if (session.state() == MellowNativeGpuStateQuarantined ||
        (selector == MellowNativeGpuCloseEvidence && result != MellowNativeGpuStatusOk)) quarantine_ = true;
    // The in-band status is the Mellow operation result. IOReturn describes
    // transport validity, allowing failed GPU operations to remain inspectable.
    IOLockUnlock(lock_);
    return kIOReturnSuccess;
}
IOReturn MellowNativeGpu::closeNative(MellowNativeGpuClient *client, NG::Session &session) {
    if (!lock_) return kIOReturnNotReady;
    MellowNativeGpuClient *released = nullptr;
    IOLockLock(lock_);
    if (client_ != client) {
        const bool held = session.held();
        IOLockUnlock(lock_);
        return held ? kIOReturnNotReady : kIOReturnSuccess;
    }
    const auto result = preparing_ ? abortNativeDriver(preparingOwner_) : session.close();
    if (result == MellowNativeGpuStatusOk) {
        released = client_; client_ = nullptr; quarantine_ = false;
        driver_ = {}; candidate_ = {}; preparing_ = false; preparingOwner_ = 0;
    } else quarantine_ = true;
    IOLockUnlock(lock_);
    if (released) released->release(); // May invoke client free; never under lock.
    return result == MellowNativeGpuStatusOk ? kIOReturnSuccess : kIOReturnBusy;
}
bool MellowNativeGpu::retryQuarantinedClient() {
    if (!lock_) return false;
    retain();
    MellowNativeGpuClient *released = nullptr;
    IOLockLock(lock_);
    bool retired = !client_;
    if (client_ && quarantine_ &&
        (preparing_ ? abortNativeDriver(preparingOwner_) : client_->nativeSession().close()) == MellowNativeGpuStatusOk) {
        released = client_; client_ = nullptr; quarantine_ = false;
        driver_ = {}; candidate_ = {}; preparing_ = false; preparingOwner_ = 0; retired = true;
    }
    IOLockUnlock(lock_);
    if (released) released->release();
    release();
    return retired;
}
IOReturn MellowNativeGpu::newUserClient(task_t task, void *security, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    // Initial evidence interface is administrator-only. Production application
    // entitlements/resource isolation and Metal plugin integration remain work.
    if (!task || type != MELLOW_NATIVE_GPU_CONNECT_TYPE ||
        IOUserClient::clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    auto *client = new MellowNativeGpuClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) { client->release(); return kIOReturnError; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnNotReady; }
    *handler = client;
    return kIOReturnSuccess;
}
bool MellowNativeGpuClient::initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) {
    return task && type == MELLOW_NATIVE_GPU_CONNECT_TYPE &&
        clientHasPrivilege(task, kIOClientPrivilegeAdministrator) == kIOReturnSuccess &&
        IOUserClient::initWithTask(task, security, type, properties);
}
bool MellowNativeGpuClient::start(IOService *provider) {
    auto *owner = OSDynamicCast(MellowNativeGpu, provider);
    if (!owner || !IOUserClient::start(provider)) return false;
    owner_ = owner; owner_->retain();
    if (!owner_->openNative(this, session_)) {
        // A failed prepare may still own DMA. Keep our provider hold until
        // free(); the provider's transaction hold prevents free until abort Ok.
        IOUserClient::stop(provider);
        return false;
    }
    return true;
}
IOReturn MellowNativeGpuClient::clientClose() {
    const IOReturn result = owner_ ? owner_->closeNative(this, session_) : kIOReturnSuccess;
    terminate();
    return result;
}
void MellowNativeGpuClient::stop(IOService *provider) {
    if (owner_) owner_->closeNative(this, session_);
    IOUserClient::stop(provider);
}
void MellowNativeGpuClient::free() {
    // An active/uncertain resource lease prevents free. Successful close has
    // already retired hardware work and removed the provider's client hold.
    if (owner_) { owner_->release(); owner_ = nullptr; }
    IOUserClient::free();
}
IOReturn MellowNativeGpuClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                              IOExternalMethodDispatch *, OSObject *, void *) {
    if (clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (!owner_) return kIOReturnNotReady;
    if (!args || selector > MellowNativeGpuCloseEvidence || args->scalarInputCount || args->scalarOutputCount ||
        args->asyncWakePort || args->asyncReferenceCount || args->structureInputDescriptor ||
        args->structureOutputDescriptor || args->structureVariableOutputData ||
        !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(MellowNativeGpuRequest) ||
        args->structureOutputSize != sizeof(MellowNativeGpuReply)) return kIOReturnBadArgument;
    MellowNativeGpuRequest request {};
    MellowNativeGpuReply reply {};
    memcpy(&request, args->structureInput, sizeof(request));
    const IOReturn result = owner_->callNative(this, session_, selector, request, reply);
    if (result == kIOReturnSuccess) {
        memcpy(args->structureOutput, &reply, sizeof(reply));
        args->structureOutputSize = sizeof(reply);
    }
    return result;
}
