// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See Drivers/PortedXe/LICENSE.MIT.
// Register/layout and operation source: Intel Xe MIT files at Linux revision
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a, drivers/gpu/drm/xe/{xe_ggtt.c,
// xe_gt_mcr.c,xe_pat.c,xe_force_wake.c,xe_guc_tlb_inval.c,xe_ttm_stolen_mgr.c,xe_wa_oob.rules,
// regs/xe_gt_regs.h,regs/xe_guc_regs.h,regs/xe_regs.h,regs/xe_gtt_defs.h}:
// https://github.com/torvalds/linux/tree/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/xe
// The pinned MMIO invalidation path does not wait for hardware clearance.
// Pre/post bit-clear waits follow Fei Yang <fei.yang@intel.com>, Intel Xe public
// patch, 2026-04-18, Message-ID 20260418000349.1398567-1-fei.yang@intel.com:
// https://lore.kernel.org/intel-xe/20260418000349.1398567-1-fei.yang@intel.com/
// This is a source-derived stopped-GuC implementation, not a claim that the
// patch is merged or that this Darwin path has been validated on hardware.
#include "XeGgttIOKit.hpp"
#include "XeMemoryIOKit.hpp"

namespace XeGgtt {
namespace {
constexpr uint32_t media=0x380000, gmd=0xd8c, wakeControl=0xa188, wakeAck=0xdfc;
constexpr uint32_t semaphore=0xfd0, selector=0xfd4, multicast=1U<<31;
constexpr uint32_t pat3=0x480c, tlbInvalidate=0xcee8, gucStatus=0xc000;
constexpr uint32_t postedRead=0x1901f8, gsm=0x800000, aperture=0x1000000;
constexpr uint32_t ggc=0x108040, ggmsMask=3U<<6;
constexpr uint64_t first=8ULL*1024*1024, top=0xfee00000ULL;
constexpr uint64_t dmaMask=((1ULL<<46)-1)&~(PageSize-1);
constexpr uint64_t patMask=3ULL<<52, present=1;
bool same(const Backing &a,const Backing &b) {
    return a.cookie==b.cookie && a.cpu==b.cpu && a.dmaPages==b.dmaPages &&
        a.pageCount==b.pageCount && a.identity==b.identity;
}
bool validRange(Range r) {
    return r.bytes && !(r.address%PageSize) && !(r.bytes%PageSize) &&
        r.address>=first && r.address<top && r.bytes<=top-r.address;
}
}

IOKitBinding::IOKitBinding(IOPCIDevice &device,MellowXe::IOKitMmio &mmio,
                          Authority authority,uint64_t epoch)
    :device_(device),mmio_(mmio),authority_(authority),epoch_(epoch) {
    device_.retain();
    if(authority_.owner)authority_.owner->retain();
}
bool IOKitBinding::read32(uint32_t reg,uint32_t &value) {
    const auto io=mmio_.access();
    return io.read32 && io.read32(io.opaque,reg,value) && value!=0xffffffffU;
}
bool IOKitBinding::write32(uint32_t reg,uint32_t value) {
    const auto io=mmio_.access();
    if(!io.write32 || !io.write32(io.opaque,reg,value)) {
        faulted_=true; return false; // a false write can already have taken effect
    }
    return true;
}
bool IOKitBinding::physical(uint64_t epoch,bool allowFault) {
    if(closed_ || (!allowFault && faulted_) || !epoch || epoch!=epoch_ ||
       !authority_.owner || !authority_.ownsEpoch ||
       !authority_.ownsEpoch(authority_.owner,epoch) ||
       !device_.isOpen(authority_.owner) || device_.getBusNumber()!=0 ||
       device_.getDeviceNumber()!=2 || device_.getFunctionNumber()!=0 ||
       device_.configRead16(0)!=0x8086 || device_.configRead16(2)!=0x7d41 ||
       mmio_.mappedLength()<aperture ||
       !mmio_.forceWake().held(MellowXe::WakeDomain::Gt))return false;
    const uint16_t command=device_.configRead16(4);
    if(command==0xffff || (command&6)!=6)return false;
    // MTL's real MMIO GGC.GGMS must report 3 (8MiB GSM / 4GiB GGTT).
    // PCI GGC and total DSM/GMS are not substitutes for this table-size check.
    // Intel source: detect_lmembar_integrated(), xe_ttm_stolen_mgr.c and GGC in
    // regs/xe_regs.h at the pinned Linux revision cited above; xe_ggtt.c uses
    // the 8MiB GSM window for graphics>=12.50.
    uint32_t actualGgc=0;
    if(!read32(ggc,actualGgc) || (actualGgc&ggmsMask)!=ggmsMask)return false;
    uint32_t mainIp=0,mediaIp=0,ack=0,control=0,vf=0;
    if(!read32(gmd,mainIp) || (mainIp>>22)!=12 || ((mainIp>>14)&255)!=70 ||
       !read32(media+gmd,mediaIp) || (mediaIp>>22)!=13 || ((mediaIp>>14)&255)!=0 ||
       !read32(wakeAck,ack) || !(ack&1) || !read32(wakeControl,control) || !(control&1) ||
       !read32(postedRead,vf) || (vf&1))return false;
    const uint16_t status=device_.configRead16(6);
    if(status==0xffff || !(status&0x10))return false;
    uint8_t cap=device_.configRead8(0x34);uint64_t seen=0;
    for(unsigned i=0;i<48 && cap;++i) {
        if((cap&3) || cap<0x40 || cap>0xfc)return false;
        const uint64_t bit=1ULL<<(cap/4);if(seen&bit)return false;seen|=bit;
        if(device_.configRead8(cap)==1) {
            if(cap>0xf8)return false;
            const uint16_t pm=device_.configRead16(cap+4);
            return pm!=0xffff && !(pm&3);
        }
        cap=device_.configRead8(cap+1);
    }
    return false;
}
bool IOKitBinding::bootstrap() {
    uint32_t a=0,b=0;
    return physical(epoch_) && authority_.ownsBootstrap &&
        authority_.ownsBootstrap(authority_.owner,epoch_) &&
        wakes() &&
        read32(gucStatus,a) && (a&1) && read32(media+gucStatus,b) && (b&1);
}
bool IOKitBinding::posted() {
    uint32_t value=0;
    if(!physical(epoch_,true) || !read32(postedRead,value) || (value&1)) {
        faulted_=true;return false;
    }
    __sync_synchronize();return true;
}
bool IOKitBinding::poll(uint32_t reg,uint32_t mask,uint32_t expected,
                       uint32_t timeout,uint32_t step,bool resetRequired) {
    const auto io=mmio_.access();
    if(!io.nowMicros || !io.delayMicros || !step)return false;
    const uint64_t start=io.nowMicros(io.opaque);uint64_t previous=start;
    const uint32_t attempts=timeout/step+1;
    for(uint32_t i=0;i<=attempts;++i) {
        uint32_t value=0;
        if(!(resetRequired?bootstrap():physical(epoch_)) || !read32(reg,value))return false;
        // A read of STEER_SEMAPHORE returning1 acquires ownership immediately;
        // never do a second semaphore read to verify or release that acquisition.
        if((value&mask)==expected)return true;
        const uint64_t now=io.nowMicros(io.opaque);
        if(now<previous || now-start>=timeout || i==attempts)return false;
        previous=now;io.delayMicros(io.opaque,step);
    }
    return false;
}
bool IOKitBinding::wakes() {
    if(!physical(epoch_)) {
        if(mainWakeHeld_ || mediaWakeHeld_)faulted_=true;
        return false;
    }
    if(!mainWakeHeld_) {
        // The owner's established hold makes this a refcount increment, never
        // adoption of a kernel-bit request from another MMIO/forcewake owner.
        if(mmio_.forceWake().acquire(MellowXe::WakeDomain::Gt)!=MellowXe::MmioStatus::Ok)return false;
        mainWakeHeld_=true;
    }
    uint32_t ack=0,control=0;
    if(mediaWakeHeld_) {
        if(!read32(media+wakeAck,ack) || !(ack&1) ||
           !read32(media+wakeControl,control) || !(control&1)) {
            faulted_=true;return false;
        }
        return true;
    }
    if(!read32(media+wakeAck,ack) || !read32(media+wakeControl,control) ||
       (ack&1) || (control&1))return false; // never adopt somebody else's hold
    mediaWakeHeld_=true; // retain the possibly issued hold on every failure
    if(!write32(media+wakeControl,0x10001) ||
       !poll(media+wakeAck,1,1,50000,50,false)) {
        faulted_=true;return false;
    }
    return true;
}
bool IOKitBinding::beginPat() {
    if(semaphoreHeld_ || !wakes() || !poll(semaphore,1,1,10,1,false))return false;
    semaphoreHeld_=true;
    uint32_t actual=0;
    // PAT3 is inside xe_gt_mcr.c's Xe-LPG INSTANCE0 nonterminated range,
    // 0x4000..0x48ff. This is an explicit platform target, not a guessed fuse.
    if(!write32(selector,multicast) || !read32(selector,actual) ||
       (actual&(multicast|0x1f00U|0xfU))!=multicast) {
        faulted_=true;endPat();return false;
    }
    return true;
}
bool IOKitBinding::endPat() {
    if(!semaphoreHeld_)return false;
    // Release a known hardware semaphore even after an earlier selector/read
    // failure, provided the SAME retained physical epoch is still accessible.
    // Reading the semaphore after releasing it would acquire it again.
    if(!physical(epoch_,true)) {faulted_=true;return false;}
    uint32_t state=0;
    if(!read32(selector,state) || !(state&multicast)) {
        // A failed selector operation may have changed hardware. Restore and
        // verify multicast before releasing access to another MCR agent.
        if(!write32(selector,multicast) || !read32(selector,state) ||
           (state&(multicast|0x1f00U|0xfU))!=multicast) {
            faulted_=true;return false;
        }
    }
    if(!posted() || !write32(semaphore,1) || !posted()) {
        faulted_=true;return false;
    }
    semaphoreHeld_=false;return true;
}
Status IOKitBinding::readPat3(uint32_t &value) {
    if(faulted_)return Status::Quarantined;
    if(!beginPat())return faulted_?Status::Quarantined:Status::Unavailable;
    uint32_t mainValue=0,mediaValue=0;
    // SAMedia PAT is non-MCR: read its actual translated register directly.
    const bool read=read32(pat3,mainValue) && read32(media+pat3,mediaValue);
    if(!read)faulted_=true;
    const bool released=endPat();
    if(!read || !released || faulted_)return Status::Quarantined;
    if(mainValue!=mediaValue || (mainValue&~0xfU))return Status::Unavailable;
    value=mainValue;return Status::Ok;
}
Status IOKitBinding::programPat3() {
    if(faulted_)return Status::Quarantined;
    if(!bootstrap() || !beginPat())return faulted_?Status::Quarantined:Status::Unavailable;
    bool ok=bootstrap() && write32(pat3,2) && bootstrap() && write32(media+pat3,2);
    uint32_t a=0,b=0;
    ok=ok && posted() && read32(pat3,a) && a==2 && read32(media+pat3,b) && b==2;
    if(!ok)faulted_=true;
    const bool released=endPat();
    return ok && released && !faulted_?Status::Ok:Status::Quarantined;
}
bool IOKitBinding::within(uint64_t address) const {
    if(!spaceHeld_ || address%PageSize)return false;
    for(size_t i=0;i<rangeCount_;++i)
        if(address>=ranges_[i].address && address-ranges_[i].address<ranges_[i].bytes)return true;
    return false;
}
IOKitBinding::PinLease *IOKitBinding::find(uint64_t owner,const Backing &b) {
    for(auto &pin:pins_)if(pin.held && pin.owner==owner && same(pin.backing,b))return &pin;
    return nullptr;
}
bool IOKitBinding::validPinned(uint64_t owner,const Backing &b) {
    if(!authority_.validateBacking ||
       !authority_.validateBacking(authority_.owner,owner,epoch_,b) ||
       !b.cookie || !b.cpu || !b.dmaPages || !b.identity || !b.pageCount ||
       b.pageCount>Manager::MaxPages)return false;
    // Cookie is dereferenced only AFTER owner records validate the complete pin.
    const XeMemory::Pin pin{b.cookie,b.dmaPages,b.pageCount};
    if(XeMemory::kernelBuffer(pin)!=b.cpu)return false;
    for(size_t i=0;i<b.pageCount;++i)
        if(b.dmaPages[i]&~dmaMask)return false;
    return true;
}
bool IOKitBinding::ownsEntry(uint64_t entry) {
    if(!entry || !(entry&present) || (entry&patMask)!=patMask ||
       (entry&~(dmaMask|patMask|present)))return false;
    const uint64_t dma=entry&dmaMask;
    for(const auto &pin:pins_)if(pin.held) {
        if(!validPinned(pin.owner,pin.backing)) {
            faulted_=true;return false;
        }
        for(size_t i=0;i<pin.backing.pageCount;++i)
            if(pin.backing.dmaPages[i]==dma)return true;
    }
    return false;
}
bool IOKitBinding::admitted(void *opaque,uint64_t epoch) {
    return static_cast<IOKitBinding *>(opaque)->physical(epoch);
}
bool IOKitBinding::acquireSpace(void *opaque,uint64_t epoch,const Range *ranges,size_t count) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    if(!s.physical(epoch) || s.spaceHeld_ || !ranges || !count || count>Manager::MaxRanges ||
       !s.authority_.acquireSpace || !s.authority_.releaseSpace)return false;
    for(size_t i=0;i<count;++i) {
        if(!validRange(ranges[i]))return false;
        for(size_t j=0;j<i;++j)
            if(ranges[i].address<ranges[j].address+ranges[j].bytes &&
               ranges[j].address<ranges[i].address+ranges[i].bytes)return false;
    }
    // Range zero-PTE inspection is intentionally NOT an ownership substitute.
    if(!s.authority_.acquireSpace(s.authority_.owner,epoch,ranges,count))return false;
    for(size_t i=0;i<count;++i)s.ranges_[i]=ranges[i];
    s.rangeCount_=count;s.spaceHeld_=true;return true;
}
bool IOKitBinding::releaseSpace(void *opaque,uint64_t epoch) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    if(!s.physical(epoch) || !s.spaceHeld_ || !s.authority_.releaseSpace)return false;
    for(const auto &pin:s.pins_)if(pin.held)return false;
    if(!s.authority_.releaseSpace(s.authority_.owner,epoch))return false;
    s.spaceHeld_=false;s.rangeCount_=0;return true;
}
bool IOKitBinding::retainBacking(void *opaque,uint64_t owner,uint64_t epoch,const Backing &b) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    if(!s.physical(epoch) || !s.spaceHeld_ || !owner || !s.authority_.retainBacking ||
       !s.authority_.releaseBacking || !s.validPinned(owner,b))return false;
    // Avoid implicit aliasing even if a caller tries two generations/owners.
    for(const auto &pin:s.pins_)if(pin.held && pin.backing.cookie==b.cookie)return false;
    for(auto &pin:s.pins_)if(!pin.held) {
        if(!s.authority_.retainBacking(s.authority_.owner,owner,epoch,b))return false;
        pin.backing=b;pin.owner=owner;pin.held=true;return true;
    }
    return false;
}
bool IOKitBinding::ownsBacking(void *opaque,uint64_t owner,uint64_t epoch,const Backing &b) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    return s.physical(epoch) && s.find(owner,b) && s.validPinned(owner,b);
}
bool IOKitBinding::releaseBacking(void *opaque,uint64_t owner,uint64_t epoch,const Backing &b) {
    auto &s=*static_cast<IOKitBinding *>(opaque);auto *pin=s.find(owner,b);
    if(!s.physical(epoch) || !pin || !s.validPinned(owner,b) || !s.authority_.releaseBacking ||
       !s.authority_.releaseBacking(s.authority_.owner,owner,epoch,b))return false;
    *pin=PinLease{};return true;
}
bool IOKitBinding::patReady(void *opaque,uint64_t epoch,uint8_t pat) {
    auto &s=*static_cast<IOKitBinding *>(opaque);uint32_t value=0;
    return pat==3 && s.physical(epoch) && s.readPat3(value)==Status::Ok && value==2;
}
bool IOKitBinding::readPte(void *opaque,uint64_t epoch,uint64_t address,uint64_t &entry) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    if(!s.physical(epoch) || !s.within(address))return false;
    const uint32_t reg=gsm+static_cast<uint32_t>((address/PageSize)*8);
    for(unsigned i=0;i<3;++i) {
        uint32_t hi=0,lo=0,again=0;
        if(!s.read32(reg+4,hi) || !s.read32(reg,lo) || !s.read32(reg+4,again)) {
            s.faulted_=true;return false;
        }
        if(hi==again) {entry=(uint64_t(hi)<<32)|lo;return true;}
    }
    s.faulted_=true;return false;
}
bool IOKitBinding::writePte(void *opaque,uint64_t epoch,uint64_t address,uint64_t entry) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    if(epoch!=s.epoch_ || !s.bootstrap() || !s.within(address) ||
       (entry && (!(entry&present) || (entry&patMask)!=patMask ||
                  (entry&~(dmaMask|patMask|present)))))return false;
    // The backend itself rejects foreign physical/DMA pages, even if a trusted
    // kernel caller accidentally bypasses Manager's publication validation.
    if(entry && !s.ownsEntry(entry))return false;
    uint64_t before=0;
    if(!readPte(opaque,epoch,address,before))return false;
    if((entry && before && before!=entry) || (!entry && before && !s.ownsEntry(before))) {
        s.faulted_=true;return false; // never overwrite or clear an alien PTE
    }
    const uint32_t reg=gsm+static_cast<uint32_t>((address/PageSize)*8);
    // Upstream uses io-64-nonatomic-lo-hi.h. Publish only with all GuC/engine
    // consumers stopped; present-first qword halves cannot be observed by jobs.
    if(!s.write32(reg,static_cast<uint32_t>(entry)) || !s.bootstrap() ||
       !s.write32(reg+4,static_cast<uint32_t>(entry>>32))) {
        s.faulted_=true;return false;
    }
    uint64_t actual=0;
    if(!readPte(opaque,epoch,address,actual) || actual!=entry) {
        s.faulted_=true;return false;
    }
    // Wa_22019338487 applies to other graphics/media versions, not12.70/13.00.
    return true;
}
bool IOKitBinding::flushOne(uint32_t offset) {
    const uint32_t reg=offset+tlbInvalidate;
    if(!bootstrap() || !poll(reg,1,0,1000000,50,true) ||
       !write32(reg,1) || !posted() || !poll(reg,1,0,1000000,50,true)) {
        faulted_=true;return false;
    }
    return true;
}
bool IOKitBinding::invalidate(void *opaque,uint64_t epoch) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    if(epoch!=s.epoch_ || !s.spaceHeld_ || !s.bootstrap() || !s.wakes())return false;
    // Posted MMIO read is required; a CPU fence alone is not a GGTT barrier.
    // Owner's single domain spans BOTH GTs, each request and its completion.
    return s.posted() && s.flushOne(0) && s.flushOne(media);
}
bool IOKitBinding::retired(void *opaque,uint64_t owner,uint64_t epoch,Handle h) {
    auto &s=*static_cast<IOKitBinding *>(opaque);
    return owner && h.generation && s.physical(epoch) && s.authority_.retired &&
        s.authority_.retired(s.authority_.owner,owner,epoch,h);
}
Backend IOKitBinding::backend() {
    Backend b{};b.context=this;b.admitted=admitted;b.acquireSpace=acquireSpace;
    b.releaseSpace=releaseSpace;b.retainBacking=retainBacking;b.ownsBacking=ownsBacking;
    b.releaseBacking=releaseBacking;b.patReady=patReady;b.readPte=readPte;
    b.writePte=writePte;b.invalidate=invalidate;b.retired=retired;return b;
}
Status IOKitBinding::close() {
    if(closed_)return Status::Ok;
    if(faulted_ || semaphoreHeld_)return Status::Quarantined;
    if(spaceHeld_)return Status::Busy;
    for(const auto &pin:pins_)if(pin.held)return Status::Busy;
    if(mediaWakeHeld_) {
        if(!physical(epoch_) || !write32(media+wakeControl,0x10000) ||
           !poll(media+wakeAck,1,0,50000,50,false)) {
            faulted_=true;return Status::Quarantined;
        }
        mediaWakeHeld_=false;
    }
    if(mainWakeHeld_) {
        if(mmio_.forceWake().release(MellowXe::WakeDomain::Gt)!=MellowXe::MmioStatus::Ok) {
            faulted_=true;return Status::Quarantined;
        }
        mainWakeHeld_=false;
    }
    // Explicit close only: never drop a live PCI/provider/authority reference
    // in a destructor, a timeout path or a guessed reset epoch.
    closed_=true;auto *owner=authority_.owner;authority_.owner=nullptr;
    device_.release();if(owner)owner->release();return Status::Ok;
}
}
