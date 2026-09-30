# NVIDIA GSP firmware DMA owner

`Drivers/PortedNvidiaGsp/FirmwareOwner.*` connects the existing bounded ELF
extractor and LibOS Radix3 implementation to the production
`MellowNative::MemoryOwner`. In a kernel build, `initializeNative` directly uses
the already attached real `MellowNative::NativeMemoryIOKit::backend()`. This
method neither attaches a PCI device nor supplies missing physical authority.
No existing parser, Radix3, memory adapter or NVIDIA MMU source is modified.

The owner allocates, copies and synchronizes four held DMA resources: an embedded
Radix3 image, the actual selected signature, the actual selected boot binary, and
WPR metadata. `prepare()==Ok` means these bytes have been staged for DMA. It does
not mean authenticated firmware, successful hardware boot, a usable RM service,
native GPU acceleration, Metal registration or system WindowServer adoption.
There is currently no implemented physical GSP bootstrap/authentication/RPC
authority in this module. Absent any of those callbacks, `boot()` returns
`Unavailable` before attempting a device operation and retains every resource.

## Pinned provenance and upstream contract

The source pin is NVIDIA/open-gpu-kernel-modules commit
`e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb` (610.57.04); it is a revision pin,
not a measured hash of downloaded source or firmware. Only primary source web
pages were read. No firmware or external source files were acquired for this
work. The relevant NVIDIA files use MIT licensing; the adjacent
`Drivers/PortedNvidiaGsp/LICENSE.MIT` retains the permission notice. The port
retains NVIDIA's 2019–2026 and 2021–2024 copyright notices in its implementation.

* [kernel_gsp.c](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c):
  `kgspPrepareBootBinaryImage_IMPL` selects actual HAL bindata and an
  `RM_RISCV_UCODE_DESC`; allocates contiguous system memory rounded to 4096;
  copies the boot image; and preserves its **raw** image size and code, data and
  manifest offsets. `_kgspCreateSignatureMemdesc` allocates a contiguous
  signature extent rounded to 256, copies the selected signature, and records
  this **rounded** extent. Neither copying operation authenticates firmware.
* [gsp_fw_wpr_meta.h](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h):
  the revision-1 handoff is exactly 256 bytes, with magic
  `0xdc3aae21371a60b3`. Booter consumes raw system-DMA addresses and subsequently
  verifies and locks WPR. CPU preparation starts with `verified=0`; a CPU-written
  `0xa0a0a0a0a0a0a0a0` is not a proof of that device operation.
* [kernel_gsp_tu102.c](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c):
  `kgspPopulateWprMeta_TU102` obtains actual FB capacity, VBIOS reservations,
  display/VGA workspace, FRTS, heap requirements and carveout ownership before
  filling the WPR layout. Bootstrap also owns FWSEC/FRTS, RISC-V reset, signed
  Booter, arguments and RPC/event infrastructure. `GSP_INIT_DONE` must come from
  the actual response with successful `rpc_result`, not a CPU completion flag.
* [kernel_gsp_gh100.c](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/arch/hopper/kernel_gsp_gh100.c):
  `kgspPopulateWprMeta_GH100` has a different secure-layout handoff. A common
  fabricated Turing framebuffer reservation is therefore unsuitable for all
  chips. The physical `populateWpr` callback owns the actual selected HAL layout.

The port seeds and checks these little-endian revision-1 fields:

| Byte offset | Field / value |
| --- | --- |
| 0, 8 | magic, revision 1 |
| 16, 24 | actual Radix3 root DMA address, raw `.fwimage` byte count |
| 32, 40 | actual contiguous boot-binary DMA address, raw boot-binary byte count |
| 48, 56, 64 | actual HAL descriptor code/data/manifest offsets within that binary |
| 72, 80 | actual contiguous signature DMA address, signature extent rounded to 256 |
| 200 | initial boot count 0 |
| 242, 243 | zero ABI padding |
| 248 | initial verified value 0 |

Remaining metadata fields require the actual HAL/FB/VBIOS/partition/CrashCat
owner. They are not guessed, filled from GPU names, or checked against an
invented universal layout. `populateWpr` may update only the exact 256-byte WPR
span; it must preserve the seeded fields and leave the unused allocation tail
unchanged. Rejecting that contract retains resources and prevents DMA staging.

## Physical authority and lifetime

All entrypoints, source bytes, metadata, ordinary CPU writers, controller
callbacks, power/reset/IRQ transitions and detach share one sleepable physical
owner lock. Place this fixed-size owner off the kernel stack. Calls may sleep.
The owner and all backend contexts must survive until `close()==Ok`; the
destructor does not release uncertain DMA or a live controller hold.

`FirmwareBootAuthority` is a trusted kernel callback contract, never a user
client input. It must be backed by the same exclusive measured NVIDIA device,
exact mapper, actual reset/power/IRQ ownership and live device epoch as the DMA
backend. `retain` acquires the real controller/context/device/mapper lifetime
hold before any allocation. A false retain is a clean rejection; a false release
preserves that hold. On success `release` is called only after epoch-wide stop
and every inverse DMA cleanup succeeds.

`select` must obtain the full signature section name from actual chip/HAL and CC
state, the exact matching driver release, and actual matching signed boot
binary/descriptor storage. Borrowed source ranges remain immutable through the
synchronous `prepare` call. PCI IDs, family names, a matching ELF string or
signature presence cannot provide this authority. The existing extractor checks
that the caller-selected `.fwversion` and signature section match those supplied
contracts; it does not authenticate their origin or signature cryptography.

The internal `MemoryOwner` is private and never exposed, so no caller can retire
its raw pins directly. Four private handles and immutable actual mapper page
arrays remain held throughout staging and boot. None is mapped into GPU VA and
no DMA/CPU address is converted into a GPU VA. A temporary caller-owned scratch
array audits all DMA page values across every resource before owner writes into
DMA buffers. Numerical uniqueness is necessary; actual direct/coherent backing
and physical exclusivity beyond those values remain the real adapter's proof.

The image accepts scattered DMA backing through the actual LibOS Radix3. Boot
binary, signature and WPR describe contiguous byte ranges, so their full
allocations must have adjacent DMA pages. A scattered Booter range returns
`Unavailable`; the owner does not mislabel its first address as a contiguous
mapping. Every complete page must fit the actual mapper/device address width.
All source, scratch, owner, CPU allocation and immutable page-array spans must
be disjoint and free of integer overflow before the owner modifies DMA buffers.
Padding and unused Radix3 entries are zero. `prepare` calls the actual backend's
`ForDevice` synchronization on every resource and requires each success.

`MemoryBackend::quiesce` must stop **GSP and Booter** as well as all GPU, display,
IRQ and worker consumers and prevent future accesses for the entire held epoch.
`close` always calls that authority before releasing a raw pin, including after
partial allocation, partial synchronization, bootstrap failure or an uncertain
device operation. False quiescence retains every resource; uncertain descriptor
or DMA completion keeps remaining resources and context quarantined. A timeout,
parser result, CPU flag or retry does not repair an unaccounted IOMMU mapping.

This object is single-use. The lifetime identity is the exact device
`{vendor, device, registryId, epoch}` plus the nonzero owner and generation 1.
Closing it does not reset that identity or allow reuse; a replacement requires
a different owner object and authoritative physical epoch/lease management.

## Boot and capacity boundaries

Before calling any bootstrap operation, the owner requires all three physical
callbacks: `bootstrap`, `authenticated`, and `waitInitDone`. The first callback
owns actual FWSEC/FRTS/RISC-V/Booter/arguments/RPC/MMIO effects. The second must
prove real signed firmware authentication and the locked WPR handoff. The third
must prove a successful actual `GSP_INIT_DONE` RPC for this same owner and epoch.
The owner sets `bootstrapAttempted` before invoking bootstrap and reports the
later proof stages separately. Any failure keeps DMA and context held until the
actual stop/cleanup authority succeeds. A CPU-updated WPR verified field is
explicitly rejected before bootstrap. There is no built-in success callback or
mock/fallback path in production.

This first owner uses four actual allocations and one embedded image allocation.
Its capacity is the configured production `MemoryOwner` bound: at most 64 MiB
per allocation, a configured total up to 2 GiB, and current actual adapter
DMA/GPU width admission of 39–48 bits. The Radix3 format's 512 GiB limit does not
override the much smaller allocation limit. A larger actual image is rejected
before allocation. No supported chipset, firmware, all-model or hardware-ready
claim follows from these format or capacity limits. Maxwell/Pascal/Volta boot
paths are not implied by this GSP owner.

## Local verification

`tests/ported_nvidia_gsp_owner_test.cpp` links the actual production owner,
MemoryOwner, firmware extractor and Radix3. Its CPU-backed callback fixtures are
explicitly synthetic and acquire no firmware or device. Independent vectors
check raw DMA addresses, little-endian topology, exact image/signature/boot
copies, zero padding and WPR offsets. Negative cases exercise required authority,
matching release, descriptor bounds, resource limits, source overlap, scratch,
DMA alias/scatter/width, partial acquisition, synchronization, WPR corruption,
epoch revocation, missing physical callbacks, authentication/RPC failures,
quiescence failure, uncertain unpin and retained controller release. They also
check that no GPU VA map is called and that stop precedes inverse DMA retirement.

The direct Clang AddressSanitizer/UndefinedBehaviorSanitizer build and binary
passed 27,070 checks; a direct GCC build passed the same suite. Freestanding
x86_64 objects for macOS 15 and 26 compiled without heap allocation, exceptions
or RTTI. These tests validate production ownership logic against
synthetic callbacks. Actual IOKit allocation, firmware authentication, hardware
boot/RPC, native GPU execution, Metal and system WindowServer remain `NOT_RUN`.
The parent agent owns aggregate runner/project registration and native SDK
compilation; a freestanding object alone is not a loadable or working driver.
