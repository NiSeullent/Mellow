// Production NativeGpuIOKit/Session methods, with an explicit host OS shim and
// synthetic owner. This exercises lifecycle/transactions, never GPU execution.
#include "NativeGpuIOKit.hpp"
#include <vector>

static unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while (0)
namespace NG = MellowNativeGpuKernel;

class Owner final : public MellowNativeGpu {
public:
    enum class Failure { None, Prepare, MissingOps, Identity, WrongRegistry, InvalidGeneration, Clock };
    Failure failure {Failure::None};
    bool abortBusy {}, closeBusy {};
    unsigned preparations {}, aborts {}, closes {}, identities {}, allocations {}, disposals {};
    uint64_t resourceOwner {};
    uint32_t *resource {};
    std::vector<uint64_t> owners;
    ~Owner() override { CHECK(resource == nullptr && allocations == disposals); }
    bool locked() const { return nativeLock() && nativeLock()->held; }
private:
    void dispose() {
        CHECK(resource != nullptr);
        delete[] resource; resource = nullptr; resourceOwner = 0; ++disposals;
    }
    MellowNativeGpuStatus prepareNativeDriver(uint64_t clientOwner, NG::DriverOps &candidate) override {
        CHECK(locked() && refs >= 2 && IOUserClient::lastCreated && IOUserClient::lastCreated->refs >= 2);
        CHECK(clientOwner && !resource);
        ++preparations; owners.push_back(clientOwner);
        // Real host allocation models the lifetime of a driver's partial
        // resource transaction. This buffer is explicitly not a DMA/GPU pin.
        resource = new uint32_t[16]; resource[0] = 0xcafef00dU;
        resourceOwner = clientOwner; ++allocations;
        candidate = {this, identity, now, submit, poll, readback, close};
        if (failure == Failure::MissingOps) candidate.readback = nullptr;
        return failure == Failure::Prepare ? MellowNativeGpuStatusUnavailable : MellowNativeGpuStatusOk;
    }
    MellowNativeGpuStatus abortNativeDriver(uint64_t clientOwner) override {
        CHECK(locked() && resource && clientOwner == resourceOwner && resource[0] == 0xcafef00dU);
        ++aborts;
        if (abortBusy) return MellowNativeGpuStatusBusy;
        dispose(); return MellowNativeGpuStatusOk;
    }
    static MellowNativeGpuStatus identity(void *opaque, uint64_t owner, NG::Identity &info) {
        auto &self = *static_cast<Owner *>(opaque);
        CHECK(self.locked() && self.resource && owner == self.resourceOwner);
        ++self.identities;
        if (self.failure == Failure::Identity) return MellowNativeGpuStatusUnavailable;
        auto *pci = OSDynamicCast(IOPCIDevice, self.getProvider()); CHECK(pci);
        info = {self.getRegistryEntryID(), pci->getRegistryEntryID(), 7, pci->vendor, pci->device, 12, 70};
        if (self.failure == Failure::WrongRegistry) ++info.serviceRegistryId;
        if (self.failure == Failure::InvalidGeneration) info.generation = 0;
        return MellowNativeGpuStatusOk;
    }
    static uint64_t now(void *opaque) {
        auto &self = *static_cast<Owner *>(opaque); CHECK(self.locked());
        return self.failure == Failure::Clock ? 0 : 100;
    }
    static MellowNativeGpuStatus submit(void *opaque, uint64_t owner, uint32_t nonce, uint32_t count,
                                        uint64_t, NG::Job &job) {
        auto &self = *static_cast<Owner *>(opaque); CHECK(self.locked() && owner == self.resourceOwner);
        job = {7, owner + 100, nonce, count}; return MellowNativeGpuStatusPending;
    }
    static MellowNativeGpuStatus poll(void *, uint64_t, const NG::Job &, NG::Observation &) {
        return MellowNativeGpuStatusPending;
    }
    static MellowNativeGpuStatus readback(void *, uint64_t, const NG::Job &, uint32_t *, uint32_t) {
        return MellowNativeGpuStatusUnavailable;
    }
    static MellowNativeGpuStatus close(void *opaque, uint64_t owner, const NG::Job &) {
        auto &self = *static_cast<Owner *>(opaque);
        CHECK(self.locked() && self.resource && owner == self.resourceOwner);
        ++self.closes;
        if (self.closeBusy) return MellowNativeGpuStatusBusy;
        self.dispose(); return MellowNativeGpuStatusOk;
    }
};
struct Fixture {
    IOPCIDevice *pci {new IOPCIDevice};
    Owner *owner {new Owner};
    HostTask task {};
    Fixture() {
        CHECK(owner->init() && owner->attach(pci) && owner->start(pci));
        CHECK(owner->refs == 1 && NativeGpuHost::liveLocks == 1);
    }
    ~Fixture() {
        CHECK(!owner->resource && IOUserClient::liveClients == 0 && owner->refs == 1);
        owner->stop(pci); owner->detach(pci); owner->release(); pci->release();
        CHECK(OSObject::live.empty() && NativeGpuHost::liveLocks == 0 && NativeGpuHost::heldLocks == 0);
        CHECK(NativeGpuHost::releasesUnderLock == 0);
    }
    IOReturn connect(MellowNativeGpuClient *&out) {
        IOUserClient *handler = nullptr;
        const auto result = owner->newUserClient(&task, nullptr, MELLOW_NATIVE_GPU_CONNECT_TYPE, nullptr, &handler);
        out = OSDynamicCast(MellowNativeGpuClient, handler);
        if (result != kIOReturnSuccess) CHECK(!handler && !out);
        else CHECK(out);
        return result;
    }
};
static MellowNativeGpuRequest queryRequest() {
    MellowNativeGpuRequest request {};
    request.version = MELLOW_NATIVE_GPU_ABI_VERSION; request.size = sizeof(request); request.correlation = 1;
    return request;
}
static void finishClient(MellowNativeGpuClient *client) {
    CHECK(client->clientClose() == kIOReturnSuccess);
    CHECK(client->refs == 1 && !client->nativeSession().held());
    client->release();
}
static void partialPrepare() {
    Fixture f; f.owner->failure = Owner::Failure::Prepare; f.owner->abortBusy = true;
    MellowNativeGpuClient *unused = nullptr;
    CHECK(f.connect(unused) == kIOReturnNotReady);
    auto *pending = OSDynamicCast(MellowNativeGpuClient, IOUserClient::lastCreated);
    CHECK(pending && OSObject::alive(pending) && pending->refs == 1 && f.owner->refs == 2);
    CHECK(!pending->nativeSession().held() && pending->nativeSession().state() == MellowNativeGpuStateCold);
    CHECK(f.owner->resource && f.owner->preparations == 1 && f.owner->aborts == 1 && f.owner->closes == 0);
    MellowNativeGpuReply reply {}; const auto request = queryRequest();
    CHECK(f.owner->callNative(pending, pending->nativeSession(), MellowNativeGpuQuery, request, reply) == kIOReturnNotReady);
    CHECK(f.connect(unused) == kIOReturnNotReady);
    CHECK(f.owner->preparations == 1 && f.owner->refs == 2 && IOUserClient::liveClients == 1);
    CHECK(!f.owner->retryQuarantinedClient());
    CHECK(OSObject::alive(pending) && f.owner->resource && f.owner->aborts == 2 && f.owner->closes == 0);
    f.owner->abortBusy = false;
    CHECK(f.owner->retryQuarantinedClient());
    CHECK(!OSObject::alive(pending) && IOUserClient::liveClients == 0 && f.owner->refs == 1 && !f.owner->resource);
    CHECK(f.owner->aborts == 3 && f.owner->closes == 0);
    f.owner->failure = Owner::Failure::None;
    MellowNativeGpuClient *client = nullptr; CHECK(f.connect(client) == kIOReturnSuccess);
    CHECK(f.owner->owners.size() == 2 && f.owner->owners[0] == 1 && f.owner->owners[1] == 2);
    finishClient(client);
}
static void rejectedCandidate(Owner::Failure failure, bool busyAbort) {
    Fixture f; f.owner->failure = failure; f.owner->abortBusy = busyAbort;
    MellowNativeGpuClient *unused = nullptr; CHECK(f.connect(unused) == kIOReturnNotReady);
    CHECK(f.owner->preparations == 1 && f.owner->aborts == 1 && f.owner->closes == 0);
    if (failure == Owner::Failure::MissingOps) CHECK(f.owner->identities == 0);
    else CHECK(f.owner->identities == 1);
    if (busyAbort) {
        auto *pending = OSDynamicCast(MellowNativeGpuClient, IOUserClient::lastCreated);
        CHECK(pending && pending->refs == 1 && f.owner->refs == 2 && f.owner->resource);
        CHECK(!pending->nativeSession().held() && pending->nativeSession().state() == MellowNativeGpuStateCold);
        CHECK(!f.owner->retryQuarantinedClient() && f.owner->resource && f.owner->closes == 0);
        f.owner->abortBusy = false;
        CHECK(f.owner->retryQuarantinedClient() && !OSObject::alive(pending));
    }
    CHECK(!f.owner->resource && IOUserClient::liveClients == 0 && f.owner->refs == 1);
    f.owner->failure = Owner::Failure::None;
    MellowNativeGpuClient *client = nullptr; CHECK(f.connect(client) == kIOReturnSuccess);
    CHECK(f.owner->owners.size() == 2 && f.owner->owners[0] == 1 && f.owner->owners[1] == 2);
    finishClient(client);
}
static void acceptedClose() {
    Fixture f; MellowNativeGpuClient *client = nullptr;
    CHECK(f.connect(client) == kIOReturnSuccess);
    CHECK(client->refs == 2 && f.owner->refs == 2 && client->nativeSession().held());
    CHECK(client->nativeSession().state() == MellowNativeGpuStateReady && f.owner->aborts == 0);
    auto request = queryRequest(); MellowNativeGpuReply reply {};
    CHECK(f.owner->callNative(client, client->nativeSession(), MellowNativeGpuQuery, request, reply) == kIOReturnSuccess);
    CHECK(reply.status == MellowNativeGpuStatusOk && reply.serviceRegistryId == f.owner->getRegistryEntryID());
    f.owner->closeBusy = true;
    CHECK(client->clientClose() == kIOReturnBusy);
    CHECK(client->terminated && client->refs == 2 && f.owner->refs == 2 && f.owner->resource);
    CHECK(client->nativeSession().held() && client->nativeSession().state() == MellowNativeGpuStateQuarantined);
    client->release(); // Simulated connection reference dies; provider lease stays.
    CHECK(client->refs == 1);
    MellowNativeGpuClient *unused = nullptr; CHECK(f.connect(unused) == kIOReturnNotReady);
    CHECK(f.owner->preparations == 1 && f.owner->refs == 2 && IOUserClient::liveClients == 1);
    CHECK(!f.owner->retryQuarantinedClient() && f.owner->resource && OSObject::alive(client));
    f.owner->closeBusy = false;
    CHECK(f.owner->retryQuarantinedClient());
    CHECK(!OSObject::alive(client) && f.owner->refs == 1 && !f.owner->resource && f.owner->aborts == 0);
    CHECK(f.owner->closes == 3);
    CHECK(f.connect(client) == kIOReturnSuccess);
    CHECK(f.owner->owners.size() == 2 && f.owner->owners[1] == 2);
    finishClient(client);
    CHECK(f.owner->retryQuarantinedClient());
}
int main() {
    partialPrepare();
    for (auto failure : {Owner::Failure::MissingOps, Owner::Failure::Identity, Owner::Failure::WrongRegistry,
                         Owner::Failure::InvalidGeneration, Owner::Failure::Clock}) {
        rejectedCandidate(failure, false); rejectedCandidate(failure, true);
    }
    acceptedClose();
    CHECK(OSObject::live.empty() && IOUserClient::liveClients == 0 && NativeGpuHost::liveLocks == 0);
    CHECK(NativeGpuHost::releasesUnderLock == 0);
    std::printf("{\"passed\":true,\"assertions\":%u,\"iokit_runtime_tested\":false,\"gpu_executed\":false}\n", checks);
}
