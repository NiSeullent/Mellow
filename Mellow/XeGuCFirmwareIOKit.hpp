#pragma once
#include "XeGuCFirmware.hpp"
#include "XeMmioIOKit.hpp"
#include "XeMemoryIOKit.hpp"

namespace XeGuCFirmware {
struct IOKitProofs {
    void *opaque {};
    bool (*ownsEpoch)(void *,uint64_t,uint64_t) {};
    bool (*quiesced)(void *,uint64_t,uint64_t) {};
    bool (*retainGgtt)(void *,const Region &,bool) {};
    bool (*releaseGgtt)(void *,const Region &) {};
    bool (*mappingPublished)(void *,const Region &,uint64_t) {};
    bool (*readPat3)(void *,uint32_t &) {};
    bool (*preloadAdsValid)(void *,const Plan &,const MellowXe::FirmwareInfo &) {};
    bool (*goldenAdsValid)(void *,const Plan &,const MellowXe::FirmwareInfo &) {};
};
// Real IOKit binding. Caller retains device/MMIO/proofs, holds GT forcewake and
// one shared sleepable serialization domain. Nothing attaches/runs by default.
// The owner must prevent reset/power changes while a load/reset call is active.
// The exact pin context and its device mapper must outlive every region hold.
// epoch is the retained physical reset epoch, never an allocation generation.
class IOKitBinding {
public:
    IOKitBinding(IOPCIDevice &device,MellowXe::IOKitMmio &mmio,
                 XeMemory::IOKitContext &pins,IOKitProofs proofs,uint64_t epoch)
        : device_(device),mmio_(mmio),pins_(pins),proofs_(proofs),epoch_(epoch) {}
    IOKitBinding(const IOKitBinding &) = delete;
    IOKitBinding &operator=(const IOKitBinding &) = delete;
    Backend backend();
private:
    IOPCIDevice &device_;
    MellowXe::IOKitMmio &mmio_;
    XeMemory::IOKitContext &pins_;
    IOKitProofs proofs_ {};
    uint64_t epoch_ {};
    static bool admitted(void *,uint64_t,uint64_t);
    static bool quiesced(void *,uint64_t,uint64_t);
    static bool retain(void *,const Region &,bool);
    static bool release(void *,const Region &);
    static bool synchronize(void *,const Region &);
    static bool readPat(void *,uint32_t &);
    static bool published(void *,const Region &,uint64_t);
    static bool ads(void *,const Plan &,const MellowXe::FirmwareInfo &);
    static bool golden(void *,const Plan &,const MellowXe::FirmwareInfo &);
};
}
