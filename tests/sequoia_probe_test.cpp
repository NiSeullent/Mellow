// SPDX-License-Identifier: MIT
#include "../Mellow/XeProbe.hpp"
#include "../Mellow/XeProbeABI.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using MellowProbe::Status;
static unsigned checks = 0;
static void check(bool value, const char *label) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
struct Fake {
    uint32_t id=0x7d418086, subsystem=0xc906144d, cls=0x03000004, command=2, pm=0;
    uint32_t gmd=(12U<<22)|(70U<<14)|4U;
    unsigned reads=0, mmioReads=0, idReads=0, cmdReads=0, pmReads=0;
    int failPci=-1, failMmio=-1;
    bool changeId=false, changeCommand=false, changePower=false, changeGmd=false;
    static bool pci(void *p, uint16_t off, uint32_t &v) {
        auto &f=*static_cast<Fake *>(p); ++f.reads;
        if (f.failPci==static_cast<int>(f.reads)) return false;
        switch (off) {
            case 0: v=f.id ^ ((++f.idReads>1 && f.changeId)?0x10000U:0U); break;
            case 4: v=f.command ^ ((++f.cmdReads>1 && f.changeCommand)?4U:0U); break;
            case 8: v=f.cls; break;
            case 0x2c: v=f.subsystem; break;
            case 0x54: v=(++f.pmReads>1 && f.changePower)?3U:f.pm; break;
            default: std::abort();
        }
        return true;
    }
    static bool mmio(void *p, uint32_t reg, uint32_t &v) {
        auto &f=*static_cast<Fake *>(p); ++f.mmioReads;
        if (reg!=MellowProbe::GmdRegister) std::abort();
        if (f.failMmio==static_cast<int>(f.mmioReads)) return false;
        v=f.gmd ^ ((f.mmioReads>1 && f.changeGmd)?1U:0U); return true;
    }
    MellowProbe::Access access() { return {this,pci,mmio,16ULL*1024*1024,0,2,0,0x50}; }
};
static void expect(Fake &f, Status expected, const char *label) {
    MellowProbe::Snapshot s; std::memset(&s,0xa5,sizeof(s));
    check(MellowProbe::capture(f.access(),s)==expected,label);
    if (expected!=Status::Ok) {
        check(!s.pciId && !s.subsystemId && !s.classRevision && !s.command && !s.pmcsr && !s.gmd &&
              !s.barBytes && !s.bus && !s.slot && !s.function,"failure has no stale hardware output");
    }
}
int main() {
    using MellowTarget::BootPolicy;
    for (unsigned kernel=0; kernel<30; ++kernel) {
        check(MellowTarget::admitDiagnostic({kernel,true,false,false,false})==(kernel==24 || kernel==25),"Darwin admission range");
    }
    for (unsigned bits=0; bits<16; ++bits) {
        BootPolicy p {24,(bits&1)!=0,(bits&2)!=0,(bits&4)!=0,(bits&8)!=0};
        check(MellowTarget::admitDiagnostic(p)==(bits==1),"exclusive explicit diagnostic intent");
    }
    check(sizeof(MellowXeProbeRequest)==32 && sizeof(MellowXeProbeReply)==112,"fixed C ABI");
    Fake good; MellowProbe::Snapshot sample;
    check(MellowProbe::capture(good.access(),sample)==Status::Ok,"valid physical sample");
    check(sample.pciId==good.id && sample.subsystemId==good.subsystem && sample.classRevision==good.cls &&
          sample.barBytes==16ULL*1024*1024 && sample.slot==2 && sample.gmd==good.gmd,"raw identity preserved");
    check(good.mmioReads==2 && good.reads==8,"bounded two GMD/eight config reads");
    for (int fail=1; fail<=8; ++fail) {
        Fake f; f.failPci=fail;
        const auto status=fail<=2 || fail==5?Status::PciUnavailable:
                          fail==3?Status::BarUnavailable: fail==4?Status::PowerUnavailable:Status::Changed;
        expect(f,status,"each failing PCI boundary");
    }
    for (int fail=1; fail<=2; ++fail) { Fake f; f.failMmio=fail; expect(f,fail==1?Status::MmioUnavailable:Status::Changed,"each failing MMIO boundary"); }
    { Fake f; f.id=UINT32_MAX; expect(f,Status::PciUnavailable,"absent config"); check(!f.mmioReads,"absent device no MMIO"); }
    { Fake f; f.id=0x9a498086; expect(f,Status::WrongDevice,"ICL/TGL spoof rejected"); check(!f.mmioReads,"wrong device no MMIO"); }
    { Fake f; f.cls=0x02000001; expect(f,Status::WrongClass,"not display"); }
    { Fake f; f.command=0; expect(f,Status::BarUnavailable,"memory decoder disabled"); }
    { Fake f; f.pm=3; expect(f,Status::PowerUnavailable,"D3 device"); check(!f.mmioReads,"D3 no MMIO"); }
    { Fake f; f.pm=UINT32_MAX; expect(f,Status::PowerUnavailable,"invalid PM read"); }
    { Fake f; f.gmd=UINT32_MAX; expect(f,Status::MmioUnavailable,"inaccessible BAR"); }
    { Fake f; f.gmd=(12U<<22)|(71U<<14); expect(f,Status::UnsupportedIp,"unsupported release"); }
    { Fake f; f.gmd=(20U<<22)|(70U<<14); expect(f,Status::UnsupportedIp,"unsupported architecture"); }
    { Fake f; f.changeId=true; expect(f,Status::Changed,"identity transition"); }
    { Fake f; f.changeCommand=true; expect(f,Status::Changed,"bus-master transition"); }
    { Fake f; f.changePower=true; expect(f,Status::Changed,"power transition"); }
    { Fake f; f.changeGmd=true; expect(f,Status::Changed,"GMD transition"); }
    for (uint8_t offset : {uint8_t(0),uint8_t(0x3c),uint8_t(0x51),uint8_t(0xfc)}) {
        Fake f; auto a=f.access(); a.pmCapability=offset;
        check(MellowProbe::capture(a,sample)==Status::PowerUnavailable,"invalid capability location");
        check(!f.mmioReads,"invalid PM capability no MMIO");
    }
    { Fake f; auto a=f.access(); a.barBytes=MellowProbe::GmdRegister+3ULL; check(MellowProbe::capture(a,sample)==Status::BarUnavailable,"BAR end boundary"); }
    { Fake f; auto a=f.access(); a.slot=3; check(MellowProbe::capture(a,sample)==Status::WrongDevice,"wrong BDF"); }
    { Fake f; auto a=f.access(); a.mmio32=nullptr; check(MellowProbe::capture(a,sample)==Status::InvalidAccess,"missing MMIO callback"); }
    { Fake f; auto a=f.access(); a.pci32=nullptr; check(MellowProbe::capture(a,sample)==Status::InvalidAccess,"missing config callback"); }
    std::printf("{\"status\":\"PASS\",\"checks\":%u,\"scope\":\"read-only production sampler with fake PCI/MMIO\",\"iokit_runtime_tested\":false,\"physical_gpu_tested\":false,\"metal_tested\":false}\n",checks);
}
