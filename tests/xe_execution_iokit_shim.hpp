// Host-only boundary for real XeExecutionIOKit + XeMemoryIOKit source.
// The existing OS shim emits the same synthetic DMA segment sequence for each
// descriptor. Give each test pin a disjoint sequence BEFORE mock GPU binding.
// This constructs simulated mapper output, never physical/IOMMU evidence.
#pragma once
#include "native_memory_iokit_shim.hpp"
#include "../Mellow/XeMemory.hpp"
namespace XeExecutionShim {
inline void disjointDma(XeMemory::Pin &pin, size_t ordinal) {
    auto *pages = const_cast<uint64_t *>(pin.dmaPages);
    for (size_t i = 0; i < pin.pageCount; ++i)
        pages[i] = 0x800000 + uint64_t(ordinal) * 0x4000000 + i * XeMemory::PageSize;
}
// Deliberate malformed trusted-kernel fixture: redirect the private resource's
// page-array pointer AND the matching VM pin to storage inside the staging
// object. Locate its pointer word in the shim's real IOMalloc resource, without
// duplicating Resource layout or adding a production test hook. Restore it
// before real production cleanup. This never models a normal IOKit allocation.
class AliasedPinPages {
public:
    AliasedPinPages(XeMemory::Pin &pin, uint64_t *pages) : pin_(pin), original_(pin.dmaPages) {
        auto found = NativeMemoryShim::allocations.find(pin.cookie);
        if (found == NativeMemoryShim::allocations.end()) std::abort();
        auto *bytes = static_cast<uint8_t *>(pin.cookie);
        size_t matches = 0;
        for (size_t offset = 0; offset + sizeof(original_) <= found->second; offset += alignof(void *)) {
            const uint64_t *word = nullptr; std::memcpy(&word, bytes + offset, sizeof(word));
            if (word == original_) { field_ = bytes + offset; ++matches; }
        }
        if (matches != 1) std::abort();
        const uint64_t *replacement = pages;
        std::memcpy(field_, &replacement, sizeof(replacement)); pin_.dmaPages = pages;
    }
    ~AliasedPinPages() { std::memcpy(field_, &original_, sizeof(original_)); pin_.dmaPages = original_; }
    AliasedPinPages(const AliasedPinPages &) = delete;
    AliasedPinPages &operator=(const AliasedPinPages &) = delete;
private:
    XeMemory::Pin &pin_;
    const uint64_t *original_;
    uint8_t *field_ {};
};
}
