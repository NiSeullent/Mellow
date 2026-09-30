// SPDX-License-Identifier: MIT
// Synthetic driver observations test the native boundary's ownership/error
// contracts only. No result from this test is native GPU execution evidence.
#include "../Mellow/NativeGpuSession.hpp"
#include <cassert>
#include <cstdio>

namespace NG = MellowNativeGpuKernel;
struct Driver {
    NG::Identity device {0x111, 0x222, 7, 0x8086, 0x7d41, 12, 70};
    uint64_t micros {100};
    bool completed {}, acquire {true}, quiesced {}, held {}, partialReadFailure {};
    unsigned submits {}, polls {}, reads {}, closes {};
    unsigned corruptObservation {};
    uint64_t sampleAdvance {};
    NG::Job submitted {};
    MellowNativeGpuStatus submitResult {MellowNativeGpuStatusPending};
    static MellowNativeGpuStatus identity(void *p, uint64_t owner, NG::Identity &out) {
        assert(owner == 55); out = static_cast<Driver *>(p)->device; return MellowNativeGpuStatusOk;
    }
    static uint64_t now(void *p) { return static_cast<Driver *>(p)->micros; }
    static MellowNativeGpuStatus submit(void *p, uint64_t owner, uint32_t nonce, uint32_t count,
                                       uint64_t deadline, NG::Job &out) {
        auto &d = *static_cast<Driver *>(p); assert(owner == 55 && deadline > d.micros);
        ++d.submits; d.held = true;
        d.submitted = {d.device.generation, 0x333, nonce, count}; out = d.submitted;
        return d.submitResult;
    }
    static MellowNativeGpuStatus poll(void *p, uint64_t owner, const NG::Job &job, NG::Observation &out) {
        auto &d = *static_cast<Driver *>(p); assert(owner == 55 && job.id == d.submitted.id);
        ++d.polls;
        if (!d.completed) return MellowNativeGpuStatusPending;
        out = {d.submitted, 1, d.micros + d.sampleAdvance, d.acquire};
        switch (d.corruptObservation) {
        case 1: ++out.job.id; break;
        case 2: ++out.job.generation; break;
        case 3: ++out.job.nonce; break;
        case 4: ++out.job.count; break;
        case 5: out.sequence = 0; break;
        case 6: out.sequence = 2; break;
        case 7: out.sampledMicros = 0; break;
        case 8: --out.sampledMicros; break;
        }
        return MellowNativeGpuStatusOk;
    }
    static MellowNativeGpuStatus readback(void *p, uint64_t owner, const NG::Job &job,
                                         uint32_t *output, uint32_t count) {
        auto &d = *static_cast<Driver *>(p); assert(owner == 55 && job.id == d.submitted.id && count == job.count);
        ++d.reads;
        // Deliberately not the evidence shader's CPU arithmetic: only test that
        // actual callback bytes are relayed and failure bytes are never exposed.
        for (uint32_t i = 0; i < count; ++i) output[i] = 0x98760000 + i;
        return d.partialReadFailure ? MellowNativeGpuStatusUnavailable : MellowNativeGpuStatusOk;
    }
    static MellowNativeGpuStatus close(void *p, uint64_t owner, const NG::Job &job) {
        auto &d = *static_cast<Driver *>(p); assert(owner == 55);
        if (d.submits) assert(job.id == d.submitted.id && job.generation == d.submitted.generation);
        ++d.closes;
        if (!d.quiesced) return MellowNativeGpuStatusBusy;
        d.held = false; return MellowNativeGpuStatusOk;
    }
    NG::DriverOps operations() { return {this, identity, now, submit, poll, readback, close}; }
};
struct Fixture {
    Driver driver;
    NG::Session session;
    MellowNativeGpuReply reply {};
    MellowNativeGpuRequest request {};
    uint64_t correlation {};
    Fixture() {
        assert(session.initialize(55, driver.operations()) == MellowNativeGpuStatusOk);
    }
    MellowNativeGpuStatus call(uint32_t selector) {
        request.version = MELLOW_NATIVE_GPU_ABI_VERSION; request.size = sizeof(request);
        request.correlation = ++correlation;
        auto status = session.call(selector, request, reply);
        assert(reply.version == 1 && reply.size == sizeof(reply) && reply.correlation == correlation);
        assert(reply.status == unsigned(status));
        assert(reply.serviceRegistryId == 0x111 && reply.physicalPciRegistryId == 0x222 && reply.maxWords == 256);
        assert(!reply.reserved0 && !reply.reserved[0] && !reply.reserved[1] && !reply.reserved[2]);
        return status;
    }
    void zeroOutput() const { for (uint32_t word : reply.output) assert(word == 0); }
    void submit() {
        request = {}; request.generation = 7; request.nonce = 91; request.count = 3; request.timeoutMicros = 1000;
        assert(call(MellowNativeGpuSubmitEvidence) == MellowNativeGpuStatusPending);
        assert(reply.jobId == 0x333 && reply.nonce == 91 && reply.count == 3 && !reply.fenceSequence);
        request.timeoutMicros = 0; request.jobId = reply.jobId;
        zeroOutput();
    }
    void close() {
        driver.quiesced = true;
        assert(call(MellowNativeGpuCloseEvidence) == MellowNativeGpuStatusOk);
        assert(!session.held() && !driver.held && session.state() == MellowNativeGpuStateClosed);
        zeroOutput();
    }
};
static void completion() {
    Fixture f;
    assert(f.call(MellowNativeGpuQuery) == MellowNativeGpuStatusOk);
    assert(f.reply.state == MellowNativeGpuStateReady && !f.reply.jobId && !f.reply.nonce && !f.reply.fenceSequence);
    f.zeroOutput(); f.submit();
    assert(f.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusPending);
    assert(!f.driver.reads && f.driver.held && f.session.held()); f.zeroOutput();
    f.driver.completed = true; f.driver.micros = 200;
    assert(f.call(MellowNativeGpuPollEvidence) == MellowNativeGpuStatusOk);
    assert(f.reply.state == MellowNativeGpuStateCompleted && f.reply.fenceSequence == 1); f.zeroOutput();
    assert(f.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusOk);
    assert(f.driver.reads == 1 && f.driver.held);
    for (unsigned i = 0; i < 3; ++i) assert(f.reply.output[i] == 0x98760000 + i);
    for (unsigned i = 3; i < 256; ++i) assert(f.reply.output[i] == 0);
    // Even completed output retains backing until the owner proves stop/retire.
    assert(f.call(MellowNativeGpuCloseEvidence) == MellowNativeGpuStatusBusy);
    assert(f.session.held() && f.driver.held); f.zeroOutput(); f.close();
}
static void malformedAndReplay() {
    for (unsigned variant = 0; variant < 6; ++variant) {
        Fixture f;
        switch (variant) {
        case 0: f.request.reserved[0] = 1; break;
        case 1: f.request.count = 1; break;
        case 2: f.request.timeoutMicros = 1000; break;
        case 3: f.request.jobId = 5; break;
        case 4: f.request.generation = 7; break;
        case 5: f.request.nonce = 1; break;
        }
        assert(f.call(MellowNativeGpuQuery) == MellowNativeGpuStatusInvalid);
        assert(!f.driver.submits && !f.driver.polls && !f.driver.reads); f.zeroOutput();
        f.driver.quiesced = true; assert(f.session.close() == MellowNativeGpuStatusOk);
    }
    Fixture replay; replay.submit();
    assert(replay.session.call(MellowNativeGpuPollEvidence, replay.request, replay.reply) == MellowNativeGpuStatusInvalid);
    assert(replay.driver.submits == 1 && replay.driver.polls == 0); replay.zeroOutput();
    const auto submitted = replay.request;
    replay.request.jobId = 0; replay.request.timeoutMicros = 1000;
    assert(replay.call(MellowNativeGpuSubmitEvidence) == MellowNativeGpuStatusBusy);
    assert(replay.driver.submits == 1); replay.zeroOutput();
    replay.request = submitted; replay.close();
}
static void corruptObservations() {
    for (unsigned variant = 0; variant <= 8; ++variant) {
        Fixture f; f.submit(); f.driver.completed = true; f.driver.micros = 200;
        if (!variant) f.driver.acquire = false; else f.driver.corruptObservation = variant;
        assert(f.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusQuarantined);
        assert(!f.driver.reads && f.session.held() && f.driver.held); f.zeroOutput(); f.close();
    }
    Fixture failed; failed.submit(); failed.driver.completed = true; failed.driver.partialReadFailure = true;
    assert(failed.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusUnavailable);
    assert(failed.driver.reads == 1 && failed.session.held()); failed.zeroOutput(); failed.close();
}
static void deadlinesAndReset() {
    Fixture late; late.submit(); late.driver.completed = true; late.driver.micros = 1100;
    assert(late.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusTimeout);
    assert(!late.driver.polls && !late.driver.reads && late.session.held()); late.zeroOutput();
    assert(late.call(MellowNativeGpuPollEvidence) == MellowNativeGpuStatusTimeout);
    late.zeroOutput(); late.close();
    Fixture crossing; crossing.submit(); crossing.driver.completed = true;
    crossing.driver.micros = 1000; crossing.driver.sampleAdvance = 100;
    assert(crossing.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusTimeout);
    assert(crossing.driver.polls == 1 && !crossing.driver.reads); crossing.zeroOutput(); crossing.close();
    Fixture reset; reset.submit(); reset.driver.device.generation = 8;
    assert(reset.call(MellowNativeGpuPollEvidence) == MellowNativeGpuStatusStale);
    assert(reset.reply.generation == 7 && reset.driver.held); reset.zeroOutput();
    assert(reset.call(MellowNativeGpuCloseEvidence) == MellowNativeGpuStatusBusy);
    assert(reset.driver.held); reset.zeroOutput(); reset.close();
    Fixture identity; identity.submit(); ++identity.driver.device.physicalPciRegistryId;
    assert(identity.call(MellowNativeGpuReadEvidence) == MellowNativeGpuStatusWrongIdentity);
    assert(!identity.driver.reads); identity.zeroOutput(); identity.close();
    Fixture clock; clock.submit(); clock.driver.micros = 99;
    assert(clock.call(MellowNativeGpuPollEvidence) == MellowNativeGpuStatusQuarantined);
    clock.zeroOutput(); clock.close();
}
static void quarantineBeforeSubmit() {
    for (unsigned cause = 0; cause < 3; ++cause) {
        Fixture f;
        if (cause == 0) {
            --f.driver.micros;
            assert(f.call(MellowNativeGpuQuery) == MellowNativeGpuStatusQuarantined);
            f.zeroOutput();
            ++f.driver.micros;
        } else if (cause == 1) {
            ++f.driver.device.physicalPciRegistryId;
            assert(f.call(MellowNativeGpuQuery) == MellowNativeGpuStatusWrongIdentity);
            f.zeroOutput();
            --f.driver.device.physicalPciRegistryId;
        } else {
            assert(f.session.close() == MellowNativeGpuStatusBusy);
            assert(f.driver.closes == 1);
        }
        assert(f.session.state() == MellowNativeGpuStateQuarantined && f.session.held());
        // Restoring identity/time, or retrying after uncertain close, must not
        // turn a quarantined connection back into a submission-capable one.
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            f.request = {};
            f.request.generation = 7; f.request.nonce = 91; f.request.count = 3;
            f.request.timeoutMicros = 1000;
            for (auto &word : f.reply.output) word = 0xdeadbeef;
            assert(f.call(MellowNativeGpuSubmitEvidence) == MellowNativeGpuStatusQuarantined);
            assert(f.reply.state == MellowNativeGpuStateQuarantined);
            assert(!f.reply.jobId && !f.reply.nonce && !f.reply.count && !f.reply.fenceSequence);
            assert(!f.driver.submits && !f.driver.polls && !f.driver.reads);
            assert(f.session.held()); f.zeroOutput();
        }
        f.request = {};
        assert(f.call(MellowNativeGpuQuery) == MellowNativeGpuStatusQuarantined);
        f.zeroOutput();
        const unsigned closes = f.driver.closes;
        assert(f.session.close() == MellowNativeGpuStatusBusy);
        assert(f.driver.closes == closes + 1 && f.session.held());
        assert(f.session.state() == MellowNativeGpuStateQuarantined);
        // The synthetic backend's quiescence acknowledgement is required even
        // when the connection never admitted an evidence job.
        f.driver.quiesced = true;
        assert(f.session.close() == MellowNativeGpuStatusOk);
        assert(f.driver.closes == closes + 2 && !f.session.held() && !f.driver.held);
        assert(f.session.state() == MellowNativeGpuStateClosed);
        assert(f.session.close() == MellowNativeGpuStatusOk);
        assert(f.driver.closes == closes + 2 && !f.driver.submits);
        f.zeroOutput();
    }
}
static void unknownAcceptance() {
    Fixture f; f.driver.submitResult = MellowNativeGpuStatusQuarantined;
    f.request.generation = 7; f.request.nonce = 21; f.request.count = 1; f.request.timeoutMicros = 1000;
    assert(f.call(MellowNativeGpuSubmitEvidence) == MellowNativeGpuStatusQuarantined);
    assert(f.driver.held && f.session.held()); f.zeroOutput();
    f.request.jobId = f.reply.jobId; f.request.timeoutMicros = 0;
    assert(f.call(MellowNativeGpuCloseEvidence) == MellowNativeGpuStatusBusy);
    assert(f.driver.held); f.zeroOutput(); f.close();
}
int main() {
    completion(); malformedAndReplay(); corruptObservations(); deadlinesAndReset(); quarantineBeforeSubmit(); unknownAcceptance();
    std::puts("native session: correlation, GPU-fence admission, readback, timeout, reset and retirement passed; gpu_execution=false");
}
