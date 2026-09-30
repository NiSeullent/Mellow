// Production ADS serialization with explicit synthetic topology/ownership/DMA.
// This test neither authenticates firmware nor executes a GPU/context switch.
#include "XeGuCAds.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>
namespace A = XeGuCAds;
using A::Error;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
static uint32_t word(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
struct Fixture {
    A::Input input {};
    A::Layout layout {};
    std::array<A::Reg, 2> regs {{{0x2080,0,0,0},{0x20a8,0,0,0}}};
    std::array<A::Reg, 2> copyRegs {{{0x22080,0,0,0},{0x220a8,0,0,0}}};
    std::array<A::Engine, 2> engines {};
    std::array<A::CaptureList, 5> lists {};
    std::array<A::WaKlv, 2> wa {{{A::WaBlockInterrupts,0,nullptr},{A::WaResetBbStack,0,nullptr}}};
    std::vector<uint8_t> bytes, sourceBytes;
    std::vector<uint64_t> pages, sourcePages;
    XeGuCFirmware::Region region {}, source {};
    bool snapshot {true}, owned {true}, captureAuthority {true}, syncOk {true};
    bool failAdsCpuAcquire {}, revokeOnSourceSync {};
    unsigned synchronizations {}, capturesValidated {};
    Fixture() {
        input.owner=7; input.epoch=11; input.deviceId=0x7d41;
        input.graphicsVersion=1270; input.mediaVersion=1300; input.gtId=0;
        input.snapshotCookie=this; input.pciRevision=8;
        // Known pinned CSS metadata, synthetic here. The real Loader separately
        // requires exact original firmware bytes/hash and physical authentication.
        input.firmware.release={70,53,0}; input.firmware.submission={1,26,0};
        input.firmware.ucodeOffset=128; input.firmware.ucodeBytes=319808;
        input.firmware.rsaOffset=319936; input.firmware.rsaBytes=384;
        input.firmware.minimumBytes=320320; input.firmware.privateDataBytes=8392704;
        input.firmware.moduleVendor=0x8086; input.firmware.declaredDeviceId=0x7d41;
        input.firmware.deviceInfoValid=true;
        engines[0]={0,0,0,0x2000,regs.data(),regs.size()};
        engines[1]={3,0,0,0x22000,copyRegs.data(),copyRegs.size()};
        input.engines=engines.data(); input.engineCount=engines.size();
        input.enabledMasks[0]=1; input.enabledMasks[3]=1;
        input.doorbellCountPerSqidi=16; input.regsetReserveBytes=4096;
        lists[0]={0,0,A::CaptureKind::Global,regs.data(),regs.size()};
        lists[1]={0,0,A::CaptureKind::Class,regs.data(),regs.size()};
        lists[2]={0,0,A::CaptureKind::Instance,regs.data(),regs.size()};
        // The pinned Xe HPG capture table has an explicitly empty BCS class
        // list. Keep its list/header; never invent a register to make it nonempty.
        lists[3]={0,3,A::CaptureKind::Class,nullptr,0};
        lists[4]={0,3,A::CaptureKind::Instance,regs.data(),regs.size()};
        input.captureLists=lists.data(); input.captureListCount=lists.size();
        input.waKlvs=wa.data(); input.waKlvCount=wa.size();
        CHECK(A::calculateLayout(input,layout)==Error::None);
        bytes.assign(layout.totalBytes,0xa5); pages.resize(bytes.size()/4096);
        for(size_t i=0;i<pages.size();++i)pages[i]=0x200000000ULL+i*4096;
        region={7,21,0x2000000,bytes.size(),bytes.data(),pages.data(),pages.size(),&bytes};
        sourceBytes.resize(57344); sourcePages.resize(sourceBytes.size()/4096);
        for(size_t i=0;i<sourcePages.size();++i)sourcePages[i]=0x300000000ULL+i*4096;
        for(size_t i=0;i<sourceBytes.size();++i)sourceBytes[i]=uint8_t((i*29+3)&255);
        source={7,22,0x4000000,sourceBytes.size(),sourceBytes.data(),sourcePages.data(),sourcePages.size(),&sourceBytes};
    }
    static Fixture &self(void *p) { return *static_cast<Fixture *>(p); }
    static bool validSnapshot(void *p,const A::Input &i) {
        auto &f=self(p); return f.snapshot && i.owner==7 && i.epoch==11 && i.snapshotCookie==&f;
    }
    static bool regionOwned(void *p,const A::Input &,const XeGuCFirmware::Region &r,A::Access access) {
        auto &f=self(p);
        const auto &expected=access==A::Access::CapturedSourceRead?f.source:f.region;
        return f.owned && r.owner==expected.owner && r.generation==expected.generation &&
            r.ggtt==expected.ggtt && r.bytes==expected.bytes && r.cpu==expected.cpu &&
            r.dmaPages==expected.dmaPages && r.pageCount==expected.pageCount && r.pinCookie==expected.pinCookie;
    }
    static bool synchronize(void *p,const XeGuCFirmware::Region &r,uint32_t offset,uint32_t size,A::Direction direction) {
        auto &f=self(p); ++f.synchronizations;
        CHECK(uint64_t(offset)+size<=r.bytes);
        if(direction==A::Direction::DeviceToCpu && r.pinCookie==f.region.pinCookie && f.failAdsCpuAcquire)return false;
        if(direction==A::Direction::DeviceToCpu && r.pinCookie==f.source.pinCookie && f.revokeOnSourceSync)f.owned=false;
        return f.syncOk;
    }
    static bool validCapture(void *p,const A::Input &,const A::Capture &c) {
        auto &f=self(p); ++f.capturesValidated;
        return f.captureAuthority && c.owner==7 && c.epoch==11 && c.provenanceCookie==&f &&
            c.prime.kernelFence==&f.regs[0] && c.switchedTo.kernelFence==&f.regs[1];
    }
    A::KernelAuthority authority() { return {this,validSnapshot,regionOwned,synchronize,validCapture}; }
    A::Capture capture(uint8_t cls) {
        A::Capture c {}; c.owner=7; c.epoch=11; c.gucClass=cls;
        c.source=source; c.imageBytes=layout.classGoldenBytes[cls]; c.provenanceCookie=this;
        c.prime={1,1,7,0x4000000,&regs[0]};
        c.switchedTo={2,2,8,0x5000000,&regs[1]};
        return c;
    }
};
static void layoutAndWire() {
    Fixture f;
    CHECK(f.layout.regsetOffset==21820 && f.layout.regsetUsedBytes==64);
    CHECK(f.layout.goldenOffset==28672 && f.layout.classGoldenBytes[0]==57344);
    CHECK(f.layout.classStateBytes[0]==52864 && f.layout.classGoldenBytes[3]==8192);
    CHECK(f.layout.classStateBytes[3]==3712 && f.layout.enabledClasses==9);
    CHECK(f.layout.goldenBytes==65536 && !(f.layout.totalBytes&4095));
    A::Builder b(f.authority()); CHECK(b.buildPreload(f.input,f.region)==Error::None);
    CHECK(b.state()==A::State::Preload && b.publishedClasses()==0);
    CHECK(b.validatePreload()==Error::None && b.validatePostLoad()!=Error::None);
    CHECK(word(f.bytes.data()+4100)==f.region.ggtt+4572);
    CHECK(word(f.bytes.data()+4104)==f.region.ggtt+4668);
    CHECK(word(f.bytes.data()+4116)==f.region.ggtt+f.layout.classGoldenOffset[0]);
    CHECK(word(f.bytes.data()+4180)==52864);
    CHECK(word(f.bytes.data())==f.region.ggtt+21820);
    CHECK(f.bytes[4]==2 && f.bytes[5]==0 && f.bytes[6]==0 && f.bytes[7]==0);
    CHECK(f.bytes[4668]==0 && f.bytes[4669]==32 && f.bytes[4668+96]==0);
    CHECK(word(f.bytes.data()+4668+512)==1 && word(f.bytes.data()+4668+524)==1);
    CHECK(word(f.bytes.data()+4668+576+8)==16);
    CHECK(word(f.bytes.data()+f.layout.waOffset)==uint32_t(A::WaBlockInterrupts)<<16);
    CHECK(word(f.bytes.data()+f.layout.waOffset+4)==uint32_t(A::WaResetBbStack)<<16);
    CHECK(word(f.bytes.data()+f.layout.captureOffset)==0);
    CHECK(std::all_of(f.bytes.begin()+f.layout.goldenOffset,
        f.bytes.begin()+f.layout.goldenOffset+f.layout.goldenBytes,[](uint8_t x){return x==0;}));
    CHECK(b.buildPreload(f.input,f.region)==Error::Busy);
    const auto backup=f.bytes;
    for(size_t offset:{size_t(0),size_t(6),size_t(4100),size_t(4180),size_t(4248),size_t(4528),
        size_t(4669),size_t(f.layout.regsetOffset),size_t(f.layout.goldenOffset),
        size_t(f.layout.waOffset),size_t(f.layout.captureOffset+4096),size_t(f.layout.privateOffset)}) {
        f.bytes[offset]^=1; CHECK(b.validatePreload()!=Error::None);
        std::copy(backup.begin(),backup.end(),f.bytes.begin());
    }
}
static void admissionAndBounds() {
    Fixture f; A::Layout l;
    for(unsigned mode=0;mode<12;++mode) {
        auto i=f.input;
        switch(mode) {
        case 0:i.owner=0;break; case 1:i.epoch=0;break; case 2:i.deviceId=0x7d40;break;
        case 3:i.graphicsVersion=1271;break; case 4:i.gtId=1;break;
        case 5:i.firmware.release.patch=1;break; case 6:i.enabledMasks[0]=3;break;
        case 7:i.doorbellCountPerSqidi=0;break; case 8:i.regsetReserveBytes=16;break;
        case 9:i.engineCount=0;break; case 10:i.captureListCount=1;break;
        case 11:i.waKlvCount=1;break;
        }
        CHECK(A::calculateLayout(i,l)!=Error::None);
    }
    auto engines=f.engines; engines[1]=engines[0]; auto i=f.input; i.engines=engines.data();
    CHECK(A::calculateLayout(i,l)!=Error::None);
    auto wa=f.wa; wa[1]=wa[0]; i=f.input; i.waKlvs=wa.data();
    CHECK(A::calculateLayout(i,l)!=Error::None);
    for(unsigned mode=0;mode<7;++mode) {
        Fixture g; auto r=g.region;
        switch(mode) {
        case 0:r.bytes-=4096; r.pageCount--;break;
        case 1:r.ggtt=0xfee00000;break; case 2:r.owner++;break;
        case 3:r.cpu=nullptr;break; case 4:r.pageCount=0;break;
        case 5:g.snapshot=false;break; case 6:g.owned=false;break;
        }
        A::Builder b(g.authority()); CHECK(b.buildPreload(g.input,r)!=Error::None);
        CHECK(std::all_of(g.bytes.begin(),g.bytes.end(),[](uint8_t x){return x==0xa5;}));
    }
    Fixture g; auto authority=g.authority(); authority.snapshotValid=nullptr;
    A::Builder absent(authority); CHECK(absent.buildPreload(g.input,g.region)!=Error::None);
    g.syncOk=false; A::Builder failed(g.authority());
    CHECK(failed.buildPreload(g.input,g.region)!=Error::None);
    CHECK(failed.state()==A::State::Failed && failed.validatePostLoad()!=Error::None);
    // Rejected authority must precede any unsafe pointer dereference.
    Fixture rejected; rejected.snapshot=false;
    auto invalidInput=rejected.input; invalidInput.engines=reinterpret_cast<const A::Engine *>(4096);
    A::Builder noSnapshot(rejected.authority());
    CHECK(noSnapshot.buildPreload(invalidInput,rejected.region)==Error::Unavailable);
    CHECK(A::validatePreload(invalidInput,rejected.region,rejected.authority())==Error::Unavailable);
    Fixture noPin; noPin.owned=false; auto invalidRegion=noPin.region;
    invalidRegion.dmaPages=reinterpret_cast<const uint64_t *>(4096);
    A::Builder noOwnership(noPin.authority());
    CHECK(noOwnership.buildPreload(noPin.input,invalidRegion)==Error::Unavailable);
}
static void goldenPublication() {
    Fixture f; A::Builder b(f.authority()); CHECK(b.buildPreload(f.input,f.region)==Error::None);
    auto c=f.capture(0);
    for(unsigned mode=0;mode<9;++mode) {
        auto bad=c;
        switch(mode) {
        case 0:bad.epoch++;break; case 1:bad.owner++;break; case 2:bad.gtId=1;break;
        case 3:bad.physicalInstance=1;break; case 4:bad.imageBytes--;break;
        case 5:bad.source=f.region;break; case 6:bad.prime=bad.switchedTo;break;
        case 7:bad.provenanceCookie=nullptr;break;
        case 8:bad.prime.contextId=65535;break;
        }
        CHECK(b.publishGolden(bad)!=Error::None);
        CHECK(b.publishedClasses()==0);
    }
    f.captureAuthority=false; CHECK(b.publishGolden(c)!=Error::None); f.captureAuthority=true;
    CHECK(b.publishGolden(c)==Error::None);
    CHECK(b.state()==A::State::PartiallyPublished && b.publishedClasses()==1);
    CHECK(b.validatePostLoad()!=Error::None && b.publishGolden(c)==Error::Busy);
    CHECK(std::equal(f.sourceBytes.begin(),f.sourceBytes.begin()+c.imageBytes,
        f.bytes.begin()+f.layout.classGoldenOffset[0]));
    auto copy=f.capture(3); CHECK(b.publishGolden(copy)==Error::None);
    CHECK(b.state()==A::State::PostLoaded && b.publishedClasses()==9);
    CHECK(b.validatePostLoad()==Error::None);
    f.failAdsCpuAcquire=true; CHECK(b.validatePostLoad()==Error::Synchronization);
    f.failAdsCpuAcquire=false;
    // GuC owns engine saved values/private data after boot, but capture input
    // descriptors remain immutable. These changes have different validity.
    const uint32_t savedValue=f.layout.regsetOffset+4;
    f.bytes[savedValue]=0x59; f.bytes[f.layout.privateOffset]=0x42;
    CHECK(b.validatePostLoad()==Error::None);
    const uint32_t captureValue=f.layout.captureOffset+4096+sizeof(A::CaptureHeader)+4;
    f.bytes[captureValue]^=1; CHECK(b.validatePostLoad()!=Error::None);
    f.bytes[captureValue]^=1;
    f.bytes[f.layout.classGoldenOffset[0]+123]^=1;
    CHECK(b.validatePostLoad()!=Error::None);
    f.bytes[f.layout.classGoldenOffset[0]+123]^=1;
    f.captureAuthority=false; CHECK(b.validatePostLoad()!=Error::None); f.captureAuthority=true;
    f.snapshot=false; CHECK(b.validatePostLoad()!=Error::None); f.snapshot=true;
    CHECK(b.validatePostLoad()==Error::None);
    Fixture g; A::Builder failure(g.authority()); CHECK(failure.buildPreload(g.input,g.region)==Error::None);
    g.syncOk=false; CHECK(failure.publishGolden(g.capture(0))!=Error::None);
    CHECK(failure.state()==A::State::Failed && failure.validatePostLoad()!=Error::None);
    Fixture revoked; A::Builder cannotPublish(revoked.authority());
    CHECK(cannotPublish.buildPreload(revoked.input,revoked.region)==Error::None);
    revoked.revokeOnSourceSync=true;
    CHECK(cannotPublish.publishGolden(revoked.capture(0))!=Error::None);
    CHECK(cannotPublish.publishedClasses()==0);
    CHECK(std::all_of(revoked.bytes.begin()+revoked.layout.goldenOffset,
        revoked.bytes.begin()+revoked.layout.goldenOffset+revoked.layout.goldenBytes,[](uint8_t x){return x==0;}));
}
int main() {
    layoutAndWire(); admissionAndBounds(); goldenPublication();
    std::cout << "{\"passed\":true,\"checks\":" << checks
        << ",\"gpu_execution\":false,\"firmware_authenticated\":false}\n";
}
