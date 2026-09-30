// SPDX-License-Identifier: MIT
#pragma once
#include "NativeGpuSession.hpp"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>

class MellowNativeGpuClient;
// Native hardware owners derive from this real IOKit boundary. This abstract
// base never matches/registers a device and never supplies a fake driver.
// A concrete owner must initialize firmware/VM/context/fences and implement
// nativeDriverOperations before publishing a service. IOObjectConformsTo sees
// the inherited MellowNativeGpu class on that concrete service.
class MellowNativeGpu : public IOService {
    OSDeclareAbstractStructors(MellowNativeGpu)
public:
    bool init(OSDictionary * = nullptr) override;
    void free() override;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
    bool openNative(MellowNativeGpuClient *, MellowNativeGpuKernel::Session &);
    IOReturn callNative(MellowNativeGpuClient *, MellowNativeGpuKernel::Session &, uint32_t,
                        const MellowNativeGpuRequest &, MellowNativeGpuReply &);
    IOReturn closeNative(MellowNativeGpuClient *, MellowNativeGpuKernel::Session &);
    // Called by the concrete owner after actual context/GT reset/stop. It only
    // releases a quarantined client if the original driver's close proves that
    // all its outstanding DMA is retired. Do not call from IRQ context.
    bool retryQuarantinedClient();
protected:
    // Returns bindings to already retained driver objects; no per-client DMA
    // acquisition/publication happens here. Submit performs those operations.
    virtual bool nativeDriverOperations(uint64_t clientOwner, MellowNativeGpuKernel::DriverOps &) = 0;
    // The shared sleepable domain must also serialize actual owner's reset,
    // mapping and command operations. DMA preparation can sleep; no workloop
    // gate/interrupt-context calls or callback reentry is allowed.
    IOLock *nativeLock() const { return lock_; }
private:
    IOLock *lock_ {};
    MellowNativeGpuClient *client_ {};
    MellowNativeGpuKernel::DriverOps driver_ {};
    uint64_t nextOwner_ {1};
    bool quarantine_ {};
    MellowNativeGpuKernel::DriverOps forwardingOperations();
    static MellowNativeGpuStatus identity(void *, uint64_t, MellowNativeGpuKernel::Identity &);
    static uint64_t now(void *);
    static MellowNativeGpuStatus submit(void *, uint64_t, uint32_t, uint32_t, uint64_t, MellowNativeGpuKernel::Job &);
    static MellowNativeGpuStatus poll(void *, uint64_t, const MellowNativeGpuKernel::Job &, MellowNativeGpuKernel::Observation &);
    static MellowNativeGpuStatus readback(void *, uint64_t, const MellowNativeGpuKernel::Job &, uint32_t *, uint32_t);
    static MellowNativeGpuStatus close(void *, uint64_t, const MellowNativeGpuKernel::Job &);
};
class MellowNativeGpuClient : public IOUserClient {
    OSDeclareDefaultStructors(MellowNativeGpuClient)
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch * = nullptr,
                           OSObject * = nullptr, void * = nullptr) override;
    MellowNativeGpuKernel::Session &nativeSession() { return session_; }
private:
    MellowNativeGpu *owner_ {};
    MellowNativeGpuKernel::Session session_ {};
};
