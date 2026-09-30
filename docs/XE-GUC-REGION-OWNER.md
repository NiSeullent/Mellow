# GuC pinned-region ownership

`XeGuCRegions::Owner` connects the existing `XeMemory::Pin` allocation ABI to the
actual `XeGgtt::Manager` and `XeGuCFirmware::Loader` lifetime algorithms. It owns
up to eight private allocations, their GGTT reservations/publication, backing
references and cleanup. It does not start a service or supply the physical
hardware owner. Native GPU execution, Metal and WindowServer readiness remain
false.

## Required physical authority

The caller must retain one actual admitted 8086:7D41/GMD12.70 PCI owner, its power
and reset epoch, exact device mapper and MMIO access. All calls, CPU writers,
GuC users, GGTT writers, reset/power transitions and references share one
sleepable serialization domain. The caller must authorize only one Loader/reset
attempt for an epoch. This callback ABI contains no Loader borrower identity and
cannot discover another boot plan elsewhere in the driver.

`Hardware::acquireSpace` must grant actual exclusive, in-bounds GGTT ranges and
exclude firmware, WOPCM, stolen/display and reserved address uses. An empty PTE
is not proof of ownership. PAT3 must be read from the actual applicable GT, with
MCR handling, and equal 2. PTE access, posted-write completion and all applicable
GT/media TLB invalidations must be real operations. No permissive defaults are
provided. A failed acquire must retain no lease; a failed release keeps the
actual lease and requires explicit retry while its authority remains alive.

The two quiescence callbacks deliberately differ:

- `resetAllowed` establishes actual context/IRQ exclusion permitting Loader's
  reset/start operation. It does not establish that a running GuC stopped using
  ADS/log/firmware. This is the exact role of Loader's `Backend::quiesced` callback
  in its `start` and `reset` implementations.
- `consumersQuiesced` establishes no present or future GuC/GPU/display access to
  any region in the epoch, maintained through reference release, PTE clearing,
  invalidate completion and DMA/descriptor completion. Region release, backing
  release and retirement independently require this stronger authority.

The actual hardware admission, range allocator and consumer-stop implementation
are still missing integration contracts. This module does not infer them from a
boot argument, elapsed timeout, reset permission or software state flag.

## Pin and CPU contract

The portable `Pins` interface accepts only the exact `XeMemory::Pin` backend ABI.
It never casts a `NativeMemoryIOKit::DmaPin` cookie. Its resolver must verify the
private pin's owner, context and complete allocation extent before CPU/DMA-array
reads. CPU and DMA-array bounds are checked for alignment, end-address overflow
and overlap with one another and existing private regions before writes. DMA
pages must satisfy the actual 46-bit, 4 KiB encoding contract.

The trusted backend must own nonaliasing real backing and IOVM pages both within
each Pin and across all live Pins, including other owners. For the IOKit factory
this authority comes from the private buffer descriptor's own allocation and
the retained exact device mapper/DMA mapping. The resolver verifies that private
association and extent; it does not independently re-audit the IOMMU's mapping
table or underlying physical page allocator. Numerical DMA-address uniqueness
alone would not prove physical nonaliasing. CPU/metadata overlap checks provide
a separate local defense and do not replace that allocation authority.

The kernel factory `makeIOKitPins` uses `XeMemory::makeIOKitPinBackend` and
`resolveDirectPinnedBuffer`. The helper verifies the private Resource's context,
owner, complete extent, original CPU pointer, current quota, retained 4 KiB
mapper, prepared descriptor and exact command preparation range/ID. The command
must expose the original descriptor rather than a full bounce copy. Each freshly
enumerated DMA page must match the stored Pin and translate at its first and
last byte to the original descriptor's physical page. The association, CPU view,
preparation and quota are checked again after inspection. Inactive mappers and
cleanup-uncertain Resources are refused. `pin.cookie` remains a trusted private
kernel handle, not an arbitrary userspace address. The generic `kernelBuffer`,
`resolvePinnedBuffer` and VM bounce-copy behavior are preserved.

Before a Loader consumes a retained region, synchronization calls the actual
pin backend's DMA synchronization operation and a CPU barrier, then checks the
canonical direct pin, physical admission and actual published PTEs again. Lost
authority quarantines the region while preserving its firmware/GGTT/pin holds.
IOKit `IODMACommand::synchronize` covers bounce-buffer copies generally; its
success does not prove engine cache flushing or job completion. This factory
now rejects bounced firmware backing, but the physical owner must separately
prove coherent cache attributes and actual device/IOMMU/GPU-IOTLB readiness.
Those requirements also apply to a future CTB or GPU-written fence allocation.

Apple's byte-address `mapToPhysicalAddress` can return its input for an unmapped
address. A matching number alone therefore cannot prove active hardware
translation. The retained original prepared command and its fresh IOVA receipts
are required, together with the independent physical ownership proofs. See
[pinned XNU DMA getters](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/iokit/Kernel/IODMACommand.cpp#L443)
and [pinned AppleVTD fallback](https://github.com/apple-oss-distributions/IOPCIFamily/blob/4822b27a36e2de70e231ecf2bf3021384fab6ec2/AppleVTD.cpp#L2231).

The separate `XeGuCFirmware::IOKitBinding` constructor now takes the exact
`XeMemory::IOKitContext` and physical reset epoch. It compares the device's direct
`iommu-parent` with that context's retained mapper before resolving private pins.
Retain/publication and synchronization use the same direct inspector;
synchronization rechecks physical admission, the pin and authoritative GGTT
publication after the blocking call, then verifies admission once more. The
read-only pin inspection is followed by admission checks before acquiring a
hold or calling DMA synchronization, so a successful hold is never hidden by
a later rejected check.
An allocation generation is never substituted for the reset epoch. These checks
do not instantiate the missing physical firmware/VM/context owner.

## Lifetime and failure behavior

Handles bind slot, owner, epoch and monotonically assigned generation. Forged
Region metadata is compared against the private canonical record before a
backend resolver call. Export and Loader retain/publication checks re-read the
actual published PTEs through `XeGgtt::Manager`.

Each Loader region hold is exclusive because its retain ABI has no borrower
identity. A second retain fails without adding a reference or resolving CPU
memory. Retained firmware, ADS and log cannot be overwritten or retired through
the owner API. A caller must also keep exported CPU pointers private and obey
the serialization/immutability contract; C++ cannot revoke a raw kernel pointer.

Retirement checks actual consumer quiescence, then uses Manager's authoritative
PTE clear/readback and invalidate inverse before unpinning. `close` drains in
reverse acquisition order, including reused slots. Failed/uncertain hardware
inverses preserve the allocation and GGTT hold for explicit recovery. A failed
DMA/descriptor completion, or an unpin that reports success while retaining its
token, permanently quarantines that pin in this owner. A retry, NotReady or GPU
reset cannot recover a possibly lost IOMMU cleanup authority. The owner has no
destructor that releases possibly live backing; caller-owned context, mapper and
hardware authority must outlive successful final close or retained quarantine.

The returned Loader backend deliberately leaves both `preloadAdsValid` and
`goldenAdsValid` absent. It does
not fabricate engine mappings, full ADS register sets, golden contexts,
platform workarounds, PPGTT/LRC state or a submission-capable firmware profile.

## Verification scope

`tests/xe_guc_region_owner_tests.cpp` links the actual production Owner,
XeGgtt Manager, PTE encoder, GuC Loader, firmware validator and GuC transport.
Only physical hardware and allocation callbacks are simulated and explicitly
marked as such. It tests exact handles and canonical regions, no-mutation input
failures, CPU/DMA-array alias and overflow, publication/inverse failures, failed
DMA cleanup quarantine, reference lifetime, reverse retirement and distinct
reset/release authority. A real Loader rejects zero-filled firmware through its
actual firmware validator and retains all three region holds when consumers are
not quiescent; no real firmware image or GPU boot success is claimed.

`tests/xe_guc_dma_iokit_tests.cpp` additionally links the production standalone
IOKit firmware binding, Xe DMA adapter and ForceWake against explicit simulated
OS/MMIO interfaces. It checks distinct allocation generation/reset epoch,
foreign pins, descriptor/page bouncing, inactive or unassociated PCI mappers,
admission loss during inspection and synchronization, publication loss and
quiescence-gated hold release. The latest targeted source-bound receipt is
[direct-dma-checkpoint.json](../validation/native-gpu/direct-dma-checkpoint.json).

`tests/xe_dma_completion_tests.cpp` links actual XeMemoryIOKit against a labeled
host OS shim. Resolver regressions cover wrong context/owner/length/mapper,
descriptor-length and association mismatch, changed pin metadata, absent CPU
view, completed pins and permanently uncertain descriptor/DMA completion.

Clang ASan/UBSan runs validate these algorithms. Building the same Owner test
with `KERNEL` and `XE_REGION_OWNER_IOKIT_SHIM_TEST` links the actual IOKit factory
and backend against the host OS shim and executes resolver/synchronization
wiring, including wrong context/owner/extent and completed-pin rejection.
Real Darwin SDK compilation and
physical IOKit/GGTT/GuC execution require separate evidence; a host shim object
is not a native SDK object or hardware acceptance result.
