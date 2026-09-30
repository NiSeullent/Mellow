// SPDX-License-Identifier: MIT
#include "../Mellow/XeGgtt.hpp"
#include <cassert>
#include <cstdio>
using namespace XeGgtt;
// Removing range admission, exact epochs, or completion checks must fail these
// tests. Only the external device boundary is simulated; Manager is production.
struct Device {
    uint64_t ptes[32] {}, pages[2] {0x100000,0x300000};
    bool live=true, grant=true, owned=true, idle=true, inv=true, release=true;
    int held=0, writes=0, reads=0, invalidations=0, failWrite=0, failRead=0;
    bool corrupt=false;
    static Device &d(void *p) { return *static_cast<Device *>(p); }
    Backend backend() {
        Backend b{}; b.context=this;
        b.admitted=[](void*p,uint64_t e){return d(p).live && e==7;};
        b.acquireSpace=[](void*p,uint64_t,const Range*,size_t){return d(p).grant;};
        b.releaseSpace=[](void*p,uint64_t){return d(p).release;};
        b.retainBacking=[](void*p,uint64_t,uint64_t,const Backing&){if(!d(p).owned)return false; ++d(p).held;return true;};
        b.ownsBacking=[](void*p,uint64_t,uint64_t,const Backing&){return d(p).owned;};
        b.releaseBacking=[](void*p,uint64_t,uint64_t,const Backing&){if(!d(p).release)return false;--d(p).held;return true;};
        b.patReady=[](void*,uint64_t,uint8_t pat){return pat==3;};
        b.readPte=[](void*p,uint64_t,uint64_t a,uint64_t &v){auto &s=d(p);assert(a/4096<32);if(++s.reads==s.failRead)return false;v=s.ptes[a/4096];return true;};
        b.writePte=[](void*p,uint64_t,uint64_t a,uint64_t v){auto&s=d(p);assert(a/4096<32);s.ptes[a/4096]=s.corrupt?0xdead:v;return ++s.writes!=s.failWrite;};
        b.invalidate=[](void*p,uint64_t){++d(p).invalidations;return d(p).inv;};
        b.retired=[](void*p,uint64_t,uint64_t,Handle){return d(p).idle;};
        return b;
    }
    Backing backing() {return {this, reinterpret_cast<uint8_t*>(pages),pages,2,55};}
};
static void happy_path() {
    Device d; Manager m; Range range{4096,8*4096}; Handle h{};
    assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
    assert(m.reserve(1,7,8192,4096,h)==Status::Ok);
    assert(m.publish(1,8,h,d.backing(),3)==Status::WrongEpoch);
    assert(d.writes==0);
    assert(m.publish(1,7,h,d.backing(),3)==Status::Ok);
    assert(d.ptes[1]==0x0030000000100001ULL);
    assert(d.ptes[2]==0x0030000000300001ULL);
    assert(m.retain(1,7,h,d.backing())==Status::Ok);
    assert(m.retire(1,7,h)==Status::Busy);
    d.idle=false; assert(m.release(1,7,h)==Status::Busy);
    d.idle=true; assert(m.release(1,7,h)==Status::Ok);
    assert(m.retire(1,7,h)==Status::Ok);
    assert(d.held==0 && d.ptes[1]==0 && d.ptes[2]==0);
    assert(m.close()==Status::Ok);
}
static void stale_identity_and_overlap() {
    Device d; Manager m; Range range{4096,8*4096}; Handle h{}, other{};
    assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
    assert(m.reserveAt(1,7,{4096,8192},h)==Status::Ok);
    assert(m.publish(2,7,h,d.backing(),3)==Status::WrongOwner);
    assert(m.publish(1,8,h,d.backing(),3)==Status::WrongEpoch);
    assert(m.publish(1,7,{h.slot,h.generation+1},d.backing(),3)==Status::NotFound);
    assert(m.reserveAt(2,7,{8192,4096},other)==Status::NoSpace);
    assert(d.held==0 && d.writes==0);
    assert(m.retire(1,7,h)==Status::Ok);
    assert(m.close()==Status::Ok);
}
static void occupied_or_unreadable_initial_pte() {
    for (int scenario=0;scenario<2;++scenario) {
        Device d; Manager m; Range range{4096,8192}; Handle h{};
        if (scenario) d.ptes[1]=0x1234;
        else d.failRead=1;
        assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
        assert(m.reserve(1,7,8192,4096,h)==Status::Ok);
        assert(m.publish(1,7,h,d.backing(),3)==Status::Quarantined);
        Mapping state{};
        assert(m.inspect(1,7,h,state)==Status::Ok && state.state==State::Quarantined);
        assert(d.writes==0 && d.held==1);
        assert(m.retire(1,7,h)==Status::Quarantined);
        assert(m.close()==Status::Busy);
    }
}
static void write_or_readback_failure_rolls_back() {
    for (int scenario=0;scenario<2;++scenario) {
        Device d; Manager m; Range range{4096,8192}; Handle h{};
        if (scenario) d.failWrite=1; // Backend writes PTE before reporting failure.
        else d.failRead=3;             // Two initial reads, then first write readback.
        assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
        assert(m.reserve(1,7,8192,4096,h)==Status::Ok);
        assert(m.publish(1,7,h,d.backing(),3)==Status::Io);
        assert(d.writes>=3 && d.invalidations==1);
        assert(d.ptes[1]==0 && d.ptes[2]==0 && d.held==0);
        Mapping state{};
        assert(m.inspect(1,7,h,state)==Status::NotFound);
        assert(m.close()==Status::Ok);
    }
}
static void unexpected_readback_quarantines() {
    Device d; Manager m; Range range{4096,8192}; Handle h{};
    d.corrupt=true;
    assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
    assert(m.reserve(1,7,8192,4096,h)==Status::Ok);
    assert(m.publish(1,7,h,d.backing(),3)==Status::Quarantined);
    assert(d.ptes[1]==0xdead && d.held==1);
    assert(m.retire(1,7,h)==Status::Quarantined);
    assert(m.close()==Status::Busy);
}
static void invalidation_and_retirement_require_retry() {
    {
        Device d; Manager m; Range range{4096,8192}; Handle h{};
        assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
        assert(m.reserve(1,7,8192,4096,h)==Status::Ok);
        d.inv=false;
        assert(m.publish(1,7,h,d.backing(),3)==Status::Quarantined);
        assert(d.ptes[1]==0 && d.ptes[2]==0 && d.held==1);
        assert(m.close()==Status::Busy);
        d.inv=true;
        assert(m.retire(1,7,h)==Status::Ok);
        assert(d.held==0 && m.close()==Status::Ok);
    }
    {
        Device d; Manager m; Range range{4096,8192}; Handle h{};
        assert(m.initialize(7,&range,1,d.backend())==Status::Ok);
        assert(m.reserve(1,7,8192,4096,h)==Status::Ok);
        assert(m.publish(1,7,h,d.backing(),3)==Status::Ok);
        d.idle=false;
        const int writes=d.writes;
        assert(m.retire(1,7,h)==Status::Quarantined);
        assert(d.writes==writes && d.held==1 && m.close()==Status::Busy);
        d.idle=true;
        assert(m.retire(1,7,h)==Status::Ok);
        assert(d.ptes[1]==0 && d.ptes[2]==0 && d.held==0);
        assert(m.close()==Status::Ok);
    }
}
int main() {
    happy_path();
    stale_identity_and_overlap();
    occupied_or_unreadable_initial_pte();
    write_or_readback_failure_rolls_back();
    unexpected_readback_quarantines();
    invalidation_and_retirement_require_retry();
    puts("PASS xe_ggtt_tests (simulated hardware boundary)");
}
