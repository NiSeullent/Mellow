// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See Drivers/PortedXe/LICENSE.MIT.
#include "XeGgtt.hpp"
#include "../Drivers/PortedXe/XePageTable.hpp"

namespace XeGgtt {
namespace {
bool valid(Range r) {
    return r.bytes && !(r.address%PageSize) && !(r.bytes%PageSize) &&
           r.bytes<=(1ULL<<32) && r.address<=(1ULL<<32)-r.bytes;
}
bool overlap(Range a,Range b) {
    return a.address<b.address+b.bytes && b.address<a.address+a.bytes;
}
bool same(const Backing &a,const Backing &b) {
    return a.cookie==b.cookie && a.cpu==b.cpu && a.dmaPages==b.dmaPages &&
           a.pageCount==b.pageCount && a.identity==b.identity;
}
}
bool Manager::live() const {
    return started_ && !closed_ && io_.admitted(io_.context,epoch_);
}
Status Manager::initialize(uint64_t epoch,const Range *ranges,size_t count,Backend io) {
    if(started_)return Status::Busy;
    if(!epoch || !ranges || !count || count>MaxRanges || !io.admitted ||
       !io.acquireSpace || !io.releaseSpace || !io.retainBacking || !io.ownsBacking ||
       !io.releaseBacking || !io.patReady || !io.readPte || !io.writePte ||
       !io.invalidate || !io.retired)return Status::Invalid;
    for(size_t i=0;i<count;++i) {
        if(!valid(ranges[i]))return Status::Invalid;
        for(size_t j=0;j<i;++j)if(overlap(ranges[i],ranges[j]))return Status::Invalid;
    }
    if(!io.admitted(io.context,epoch))return Status::Unavailable;
    if(!io.acquireSpace(io.context,epoch,ranges,count))return Status::Unavailable;
    io_=io; epoch_=epoch; count_=count; started_=true;
    for(size_t i=0;i<count;++i)ranges_[i]=ranges[i];
    return Status::Ok;
}
Status Manager::get(uint64_t owner,uint64_t epoch,Handle h,size_t &index) const {
    if(!started_ || closed_)return Status::Unavailable;
    if(epoch!=epoch_)return Status::WrongEpoch;
    if(h.slot>=MaxMappings || !h.generation)return Status::NotFound;
    const auto &m=slots_[h.slot].m;
    if(m.state==State::Free || m.generation!=h.generation)return Status::NotFound;
    if(!owner || m.owner!=owner)return Status::WrongOwner;
    index=h.slot; return Status::Ok;
}
bool Manager::fits(Range r) const {
    if(!valid(r) || r.bytes/PageSize>MaxPages)return false;
    bool contained=false;
    for(size_t i=0;i<count_;++i)
        if(r.address>=ranges_[i].address && r.address+r.bytes<=ranges_[i].address+ranges_[i].bytes)contained=true;
    if(!contained)return false;
    for(const auto &s:slots_)if(s.m.state!=State::Free && overlap(r,s.m.range))return false;
    return true;
}
Status Manager::insert(uint64_t owner,Range r,Handle &out) {
    if(!serial_)return Status::NoSpace;
    for(size_t i=0;i<MaxMappings;++i)if(slots_[i].m.state==State::Free) {
        auto &m=slots_[i].m;
        m.state=State::Reserved; m.owner=owner; m.epoch=epoch_;
        m.generation=serial_++; m.range=r; out={i,m.generation}; return Status::Ok;
    }
    return Status::NoSpace;
}
Status Manager::reserveAt(uint64_t owner,uint64_t epoch,Range r,Handle &out) {
    if(epoch!=epoch_)return Status::WrongEpoch;
    if(!live())return Status::Unavailable;
    if(!owner || !valid(r) || r.bytes/PageSize>MaxPages)return Status::Invalid;
    if(!fits(r))return Status::NoSpace;
    return insert(owner,r,out);
}
Status Manager::reserve(uint64_t owner,uint64_t epoch,uint64_t bytes,uint64_t alignment,Handle &out) {
    if(epoch!=epoch_)return Status::WrongEpoch;
    if(!live())return Status::Unavailable;
    if(!owner || !bytes || bytes%PageSize || bytes/PageSize>MaxPages ||
       alignment<PageSize || alignment>(1ULL<<32) || (alignment&(alignment-1)))return Status::Invalid;
    for(size_t i=0;i<count_;++i) {
        uint64_t at=(ranges_[i].address+alignment-1)&~(alignment-1);
        const uint64_t end=ranges_[i].address+ranges_[i].bytes;
        // Each failed iteration skips at least one occupied interval.
        for(size_t attempt=0;attempt<=MaxMappings && at<end && bytes<=end-at;++attempt) {
            uint64_t next=at;
            for(const auto &s:slots_)if(s.m.state!=State::Free && overlap({at,bytes},s.m.range)) {
                const uint64_t after=(s.m.range.address+s.m.range.bytes+alignment-1)&~(alignment-1);
                if(after>next)next=after;
            }
            if(next==at)return insert(owner,{at,bytes},out);
            at=next;
        }
    }
    return Status::NoSpace;
}
bool Manager::read(uint64_t address,uint64_t &pte) const {
    return live() && io_.readPte(io_.context,epoch_,address,pte);
}
bool Manager::expected(const Slot &s,size_t page,uint64_t &pte) const {
    return Mellow::PortedXe::encodeGgtt(s.m.backing.dmaPages[page],s.m.pat,false,46,pte)==Mellow::PortedXe::Status::Ok;
}
bool Manager::verify(const Slot &s,bool clearing) const {
    for(size_t i=0;i<s.m.backing.pageCount;++i) {
        uint64_t actual=0,want=0;
        if(!expected(s,i,want) || !read(s.m.range.address+i*PageSize,actual))return false;
        if(clearing ? (actual!=0 && actual!=want) : actual!=want)return false;
    }
    return true;
}
Status Manager::publish(uint64_t owner,uint64_t epoch,Handle h,const Backing &b,uint8_t pat) {
    size_t i=0; auto result=get(owner,epoch,h,i); if(result!=Status::Ok)return result;
    auto &s=slots_[i];
    if(s.m.state!=State::Reserved)return s.m.state==State::Quarantined?Status::Quarantined:Status::Busy;
    if(!live())return Status::Unavailable;
    if(!b.cookie || !b.cpu || !b.dmaPages || !b.identity || b.pageCount!=s.m.range.bytes/PageSize || pat>3)return Status::Invalid;
    if(!io_.patReady(io_.context,epoch_,pat))return Status::Unavailable;
    if(!io_.retainBacking(io_.context,owner,epoch_,b))return Status::Unavailable;
    s.m.backing=b; s.m.pat=pat; s.held=true;
    // All DMA metadata is read only AFTER the trusted retain validates it.
    bool validBacking=io_.ownsBacking(io_.context,owner,epoch_,b);
    for(size_t page=0;validBacking && page<b.pageCount;++page) {
        uint64_t pte=0; validBacking=expected(s,page,pte);
    }
    if(!validBacking) {
        s.m.state=State::Quarantined;
        return teardown(i)==Status::Ok?Status::Invalid:Status::Quarantined;
    }
    // Never overwrite even an apparently valid firmware mapping in a grant.
    for(size_t page=0;page<b.pageCount;++page) {
        uint64_t pte=0;
        if(!read(s.m.range.address+page*PageSize,pte) || pte!=0) {
            s.m.state=State::Quarantined; return Status::Quarantined;
        }
    }
    s.m.state=State::Quarantined;
    for(size_t page=0;page<b.pageCount;++page) {
        uint64_t pte=0;
        if(!live() || !expected(s,page,pte))return Status::Quarantined;
        s.touched=true;
        if(!io_.writePte(io_.context,epoch_,s.m.range.address+page*PageSize,pte))
            return teardown(i)==Status::Ok?Status::Io:Status::Quarantined;
        uint64_t actual=0;
        if(!read(s.m.range.address+page*PageSize,actual) || actual!=pte)
            return teardown(i)==Status::Ok?Status::Io:Status::Quarantined;
    }
    if(!live() || !io_.invalidate(io_.context,epoch_) || !verify(s,false))
        return teardown(i)==Status::Ok?Status::Io:Status::Quarantined;
    s.m.state=State::Published; return Status::Ok;
}
Status Manager::published(uint64_t owner,uint64_t epoch,Handle h,const Backing &b) {
    size_t i=0; auto result=get(owner,epoch,h,i); if(result!=Status::Ok)return result;
    auto &s=slots_[i];
    if(s.m.state!=State::Published)return Status::Quarantined;
    if(!same(s.m.backing,b))return Status::Invalid;
    if(!live() || !io_.ownsBacking(io_.context,owner,epoch_,b) ||
       !io_.patReady(io_.context,epoch_,s.m.pat) || !verify(s,false)) {
        s.m.state=State::Quarantined; return Status::Quarantined;
    }
    return Status::Ok;
}
Status Manager::retain(uint64_t owner,uint64_t epoch,Handle h,const Backing &b) {
    auto result=published(owner,epoch,h,b); if(result!=Status::Ok)return result;
    auto &s=slots_[h.slot];
    if(s.m.users==UINT32_MAX)return Status::Busy;
    ++s.m.users; return Status::Ok;
}
Status Manager::release(uint64_t owner,uint64_t epoch,Handle h) {
    size_t i=0; auto result=get(owner,epoch,h,i); if(result!=Status::Ok)return result;
    auto &s=slots_[i];
    if(!s.m.users)return Status::Invalid;
    if(!live() || !io_.retired(io_.context,owner,epoch_,h))return Status::Busy;
    --s.m.users; return Status::Ok;
}
Status Manager::teardown(size_t i) {
    auto &s=slots_[i]; const Handle h{i,s.m.generation};
    if(!live() || s.m.users)return Status::Quarantined;
    if(s.touched && !s.cleared) {
        if(!io_.ownsBacking(io_.context,s.m.owner,epoch_,s.m.backing) ||
           !io_.retired(io_.context,s.m.owner,epoch_,h) || !verify(s,true))return Status::Quarantined;
        for(size_t page=0;page<s.m.backing.pageCount;++page) {
            const uint64_t address=s.m.range.address+page*PageSize;
            uint64_t actual=0;
            if(!live() || !io_.writePte(io_.context,epoch_,address,0) ||
               !read(address,actual) || actual!=0)return Status::Quarantined;
        }
        if(!live() || !io_.invalidate(io_.context,epoch_))return Status::Quarantined;
        for(size_t page=0;page<s.m.backing.pageCount;++page) {
            uint64_t actual=0;
            if(!read(s.m.range.address+page*PageSize,actual) || actual!=0)return Status::Quarantined;
        }
        s.cleared=true;
    }
    if(s.held && !io_.releaseBacking(io_.context,s.m.owner,epoch_,s.m.backing))return Status::Quarantined;
    s={}; return Status::Ok;
}
Status Manager::retire(uint64_t owner,uint64_t epoch,Handle h) {
    size_t i=0; auto result=get(owner,epoch,h,i); if(result!=Status::Ok)return result;
    auto &s=slots_[i];
    if(s.m.users)return Status::Busy;
    if(!live()) {s.m.state=State::Quarantined;return Status::Quarantined;}
    // Unknown initial contents were never ours; no automatic freeing of that
    // reservation. Trusted space reconstruction is required outside this owner.
    if(s.m.state==State::Quarantined && s.held && !s.touched)return Status::Quarantined;
    s.m.state=State::Quarantined; return teardown(i);
}
Status Manager::inspect(uint64_t owner,uint64_t epoch,Handle h,Mapping &out) const {
    size_t i=0; auto result=get(owner,epoch,h,i); if(result!=Status::Ok)return result;
    out=slots_[i].m; return Status::Ok;
}
Status Manager::close() {
    if(!live())return Status::Unavailable;
    for(const auto &s:slots_)if(s.m.state!=State::Free)return Status::Busy;
    if(!io_.releaseSpace(io_.context,epoch_))return Status::Quarantined;
    closed_=true;return Status::Ok;
}
}
