# Native GPU memory and NVIDIA publication owners

This stage adds production C++ components shared by the kext target and host
tests. It is an intermediate implementation of the full Intel/NVIDIA port.
It does not establish physical GPU execution, a loadable native kext, Metal,
or WindowServer acceleration. No unsupported PCI ID is automatically admitted.

## Implemented path

`Mellow/NvidiaMmioIOKit.cpp` uses actual IOKit PCI reads and a retained provider
memory-descriptor snapshot to map BAR0. It checks the exact NVIDIA device,
registry instance, BDF, driver ownership, display-function type, assigned BAR,
descriptor tag/address/length, and D0 state. It reads BOOT0/BOOT1 and checks PCI
state again. Descriptor and map ownership are balanced on failure. Live reads
revalidate the descriptor, mapping, ownership and power state.

The caller owns a sleepable lock shared with power/reset/stop. Its epoch is a
caller-maintained generation; BOOT reads cannot independently attest a reset
epoch. The adapter never writes PCI configuration or MMIO registers. It does
not call PCI memory accessors that may change tunnel power policy. This initial
adapter rejects tunneled providers; a reviewed tunnel-specific provider is
still needed for that path.

`Drivers/NativeNvidia/Probe.cpp` recognizes exact chipsets enumerated by the
pinned Nouveau source. Maxwell, Pascal, Volta, Turing, Ampere, Hopper, Ada and
both current Blackwell architecture groups can be **observed**, independently
of channel negotiation. Unknown chipsets, changed identities, unsupported
endianness and Turing-or-newer virtual GPU markers are rejected. Recognition
never chooses an engine class or enables a driver.

`Drivers/NativeGpu/MemoryOwner.cpp` owns bounded allocation metadata, quotas,
DMA-page validation, GPU mappings and per-job references. GPU VA publication
and teardown belong to authoritative adapter callbacks. Unmap success must
include completed GPU TLB invalidation before DMA backing can be released.
Uncertain pin/map/unmap failures retain ownership and stop new admission.
Successful quiescence never invents job completion or silently drops holds.

`Mellow/NativeMemoryIOKit.cpp` connects this owner to real IOKit descriptors and
IODMACommand. It retains the exact provider's direct IOMapper, validates actual
4 KiB IOVM segments and their original physical pages, rejects bounced backing,
and requires the actual physical owner to prove coherence and live admission.
GPU mapping/TLB/retirement remain mandatory physical VM operations. DMA and
descriptor completion errors latch cleanup uncertainty: a later NotReady,
successful descriptor clear or GPU reset cannot reclaim that authority.

The existing `Mellow/XeMemoryIOKit.cpp` teardown now explicitly checks DMA
completion before clearing the descriptor. Apple's `clearMemoryDescriptor(true)`
calls `complete()` while ignoring its return, so clear success alone cannot
prove that an IOMMU mapping was retired. The repair preserves the pin, mapper
and charge on completion uncertainty and rejects further synchronization or CPU
buffer access through that quarantined pin.

`Drivers/NativeNvidia/GpFifoQueue.cpp` writes encoded entries into a held,
CPU-visible GPFIFO ring. It retains command and data allocations, seals the
private pushbuffer through the actual engine owner, synchronizes backing,
orders stores, writes GP_PUT, orders that publication, reads the current work
token and rings the doorbell. A dedicated GPU tracking semaphore controls
retirement. GP_GET, IRQ arrival and a timeout are not completion evidence.
Publication uncertainty keeps job resources until completion or proven stop.

The first reviewed queue slice is negotiated AmpereA (`0xC56F`), an ordinary
non-confidential channel and coherent system-memory ring/fence. It reserves
one sentinel ring slot and rejects other channel formats explicitly. Extended
Hopper/Blackwell entries already exist in the imported encoder, but do not
implicitly enable those queues. The transport must supply actual bound
registers, engine sealing, system ordering and quiescence; missing callbacks
fail. Ring and fence backing require a coherent, non-bounced physical adapter.

`Drivers/NativeNvidia/CopyPushbuffer.cpp` supplies the copy-engine sealing
adapter. It accepts only the imported encoder's exact 44-byte virtual copy on
negotiated C56F/C6B5/subchannel 4, rechecks held source/destination/private-command
and fence allocations against MemoryOwner, and resolves the actual VM/engine
ownership. It appends host WFI, SYS_MEMBAR and a SYS-flushed CE one-word GPU
semaphore release. The resulting 96-byte pushbuffer is written transactionally
as little endian; the CPU never completes the fence. Other methods, classes,
privileged packets and unowned addresses are rejected before mutation.

## Build and validation

The Xcode kext target includes these production units and the imported NVIDIA
encoder. The cross-build source resolver follows file-reference identities and declared source-group paths
and rejects ambiguous references or paths outside the checkout. Same-named
legacy and portable Xe implementations remain distinct. The approved host runner links tests against the same
production `.cpp` files, obtains the local non-system dependency set from the
compiler, and rejects changed source/header/runtime inputs. Independent direct
compiler checks compile the native units against the vendored Darwin kernel SDK
for macOS 15 and 26 and record source/object hashes. The changed cross-build
script has source syntax/read review only; it was not executed in this task.

The IOKit harness substitutes OS types through an include path and compiles
the production adapter unchanged. It checks failed map creation, malformed
capability chains, stale BAR descriptors, live descriptor changes and balanced
references. It does not model actual PCI transactions, cache coherence or a
physical device. Darwin `MH_OBJECT` compilation does not verify a linked kext,
macOS-version-specific exported APIs, load, or execution.

## Remaining native integration

The physical VM/channel owner must still supply GPU page tables, completed TLB
invalidation, firmware/engine initialization, register leases, command sealing,
completion and reset/power exclusion. Maxwell/Pascal/Volta need their Nouveau
initialization path; NVIDIA open RM is a separate Turing-or-newer path. Intel
still needs complete integration of its existing Xe memory, GuC, context and
submission owners. macOS userspace acceleration, native Metal and display
integration remain separate required parts of the overall port.

## Primary sources

- [Pinned Nouveau chipset, endianness and vGPU decoding](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/nouveau/nvkm/engine/device/base.c#L2993)
- [Pinned NVIDIA channel publication and tracking semaphore](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_channel.c)
- [Pinned NVIDIA Turing host publication inherited by Ampere](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_turing_host.c)
- [Pinned NVIDIA current work submission token contract](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/common/inc/nv_uvm_types.h)
- [Apple PCI configuration and mapping implementation](https://github.com/apple-oss-distributions/IOPCIFamily/blob/main/IOPCIDevice.cpp)
- [Apple BAR descriptor construction](https://github.com/apple-oss-distributions/IOPCIFamily/blob/main/IOPCIBridge.cpp)
- [Apple DMA prepare, synchronization and completion](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IODMACommand.h)

The NVIDIA encoder's notices and pinned source provenance are preserved in
`Drivers/PortedNvidia/`. It was copied from the disjoint local `1edd` worktree;
no remote source was downloaded or executed by this integration.

## Local snapshot integration

The selected 34 files were read from the dirty local peer worktree at base
`cd187f463ba0e70238f3202fc8b2dea81e216b04`. Their bytes were measured before
and after copying into this isolated source tree; the peer files remained
unchanged. The selection excludes its newer Intel GGTT bridge, NVIDIA MMU work,
project/runner files and planning metadata. Exact hashes and local adaptations
are recorded in `porting/native-owner-snapshot.json`. Its earlier peer tests are
not used as acceptance of later files; the selected copy is tested independently.

These units are registered in the target as available production components.
They have no instantiated physical firmware/VM/channel owner in the diagnostic
service or userspace runtime yet. Merely compiling these objects does not add
unsupported PCI admission or make the Metal/app runtime use this native queue.

The subsequent exact Intel pin resolver checks context, owner, allocation size,
actual descriptor length, mapper and prepared DMA state before exposing CPU
backing. The GuC-region owner uses that exact ABI, exclusive Loader holds and
independent consumer quiescence for reverse GGTT/DMA retirement. Its native
IOKit factory is compiled unchanged against the clearly labeled OS shim in a
separate host variant; simulated OS execution remains distinct from native KPI
acceptance. The a46eb68 aggregate had twenty passing suites. The subsequent
IOKit execution staging and GSP DMA-owner integration passes twenty-two suites
with actual compiler-reported local input hashes checked before and after build.
These remain host/OS-shim checks, not physical GPU acceptance.
