# NVIDIA source intake and backend contracts

This document defines source references and the execution contracts that remain to be
implemented. The `nouveau` and `nvidia-open` intake targets do not build or execute a
Darwin NVIDIA GPU driver. All recipes retain an empty `pci_device_ids` list. Selecting
a recipe does not admit a physical device, report Metal capability or produce a
WindowServer acceleration receipt.

The current Mellow runtime has separate C++ compute/render objects and host GL/CL
execution evidence. That evidence does not validate a native NVIDIA provider.
See [implementation status](IMPLEMENTATION-STATUS.md),
[support matrix](GPU-SUPPORT-MATRIX.md) and
[RFC D09](PLATFORM-DECISIONS.md).

## Explicit adapter selection

The CLI names are source-intake targets. The `adapter_contract` records the intended
kernel/userspace relationship; it is not an implemented entry point.

| CLI target | Adapter contract | Architecture references | Current result |
| --- | --- | --- | --- |
| `nouveau` | `nouveau-nvk` | Maxwell, Pascal, Volta, Turing, Ampere, Ada, consumer Blackwell | Source review only; no Darwin backend |
| `nvidia-open` | `nvidia-rm` | Turing, Ampere, Ada, Blackwell; selected release/device verification still required | Source review only; no Darwin backend |

`nvidia-open` keeps its existing CLI name for compatibility. The open RM adapter
excludes Maxwell, Pascal and Volta. NVIDIA states this boundary in its
[official kernel module guide](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/610/kernel-modules.html).
The existing pinned NVIDIA source at
[`e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/README.md)
is release 610.57.04 and requires GSP firmware and userspace from that same release.
This document does not verify a complete source or binary dependency closure.

The current [Mesa NVK documentation](https://docs.mesa3d.org/drivers/nvk.html)
describes Linux Vulkan support from Kepler through Ada and consumer Blackwell, and
requires Linux 6.6 or newer. This repository's Nouveau inventory begins at Maxwell
to match the requested work. The architecture list is nonexhaustive; it is not an
allowlist of working macOS GPUs. Maxwell generations, chipset stepping, firmware
and power management need separate review. Nouveau's
[feature matrix](https://nouveau.freedesktop.org/FeatureMatrix.html) also cautions that
it is updated infrequently. These live upstream documents were reviewed on
2026-09-30; their URLs do not pin source content to an immutable revision.

## Source ownership and provenance

[Backend recipes](../porting/backend-recipes.json) record architecture, firmware,
userspace and provenance requirements. They describe required work rather than
attesting that any requirement passed. `architectures` contains family reference
IDs from [gpu-families.json](../porting/gpu-families.json); `architecture_scope`
states their source-review boundary. An empty list, including the generic AMD
recipe's architecture list, does not mean all devices are supported.

The Nouveau target selects Linux `drivers/gpu/drm/nouveau/` and explicit DRM/Linux
headers. It must contain a selected Nouveau driver file; common headers alone do
not identify an adapter. Mesa NVK requires a separately pinned source tree and
compiler/winsys review. Linux and Mesa revisions, configurations and hashes must
be recorded independently. See the primary
[Linux Nouveau tree](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/nouveau)
and [Mesa Nouveau/NVK tree](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau).

The RM target retains its existing source admission rules and additionally permits
`src/nvidia-modeset/` references. NVIDIA separates OS-agnostic RM/modeset code from
Linux kernel interfaces. That separation identifies port boundaries; it does not
make the Linux interface a Darwin interface. The pinned
[OS interface header](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/common/inc/os-interface.h)
and [modeset OS interface header](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-modeset/nvidia-modeset-os-interface.h)
are concrete references for allocation, PCI/MMIO, locks, work, timers and callbacks.

Each selected source requires measured file hashes, immutable revision membership,
build configuration, transitive dependency review and preserved license/copyright
notices. A caller-supplied commit string and a file's SPDX marker alone do not prove
membership or permit every combined artifact. Firmware and proprietary userspace
retain their own terms; see [licensing](LICENSING.md).

## Execution contracts still required

The two adapters must keep distinct resource and submission semantics:

- `nouveau-nvk`: a reviewed Darwin replacement for the selected Nouveau DRM/NVKM
  memory, VM-bind, channel, submission and synchronization interface, plus a
  compatible port of Mesa NVK's compiler and winsys. Select Falcon/ACR or GSP
  initialization from the exact chipset source; a pre-Turing device does not gain
  GSP support by selecting an RM image.
- `nvidia-rm`: an XNU implementation of the selected RM operating-system interface,
  PCI owner, DMA/IOMMU mappings, GPU virtual memory, authenticated matching GSP
  boot/RPC, channel lifecycle, interrupts, fences and reset teardown. Matching
  NVIDIA Linux userspace is an upstream requirement, not a ready Darwin library.
  A reviewed Darwin userspace/compiler contract remains necessary.

NVIDIA RM/UAPI and Nouveau/NVK winsys are not interchangeable. Any future bridge
must implement and validate that translation explicitly. Changing adapters
invalidates prior memory, synchronization, compiler, firmware and reset evidence.

An executable provider must reject a wrong device, stale reset epoch, incompatible
resource or compiler ABI, missing firmware, incomplete submission and unobserved
completion. A successful return, register read or device-name string cannot replace
real GPU submission, fence correlation and independently checked readback.

Display and system graphics require further contracts. Apple documents
[`IOFramebuffer`](https://developer.apple.com/documentation/kernel/ioframebuffer?language=objc)
as simple framebuffer operation and distinguishes separate acceleration through
`IOAccelerator`. Native framebuffer/modeset, offscreen compute/render, IOSurface
ownership/synchronization, Apple Metal device registration, WindowServer,
physical scanout, sleep/wake and multimonitor operation each need their own target
OS/device evidence. No recipe advertises these capabilities.

## TinyGPU as a separate future option

[TinyGPU's upstream documentation](https://docs.tinygrad.org/tinygpu/) describes
macOS 13 or newer, USB4/Thunderbolt and NVIDIA Ampere or newer for tinygrad compute.
The actual
[`ops_nv.py` PCI path](https://github.com/tinygrad/tinygrad/blob/master/tinygrad/runtime/ops_nv.py)
and [`NVDev` implementation](https://github.com/tinygrad/tinygrad/blob/master/tinygrad/runtime/support/nv/nvdev.py)
provide useful references for PCI transport, GSP RPC, memory and compute submission.

This is a distinct DriverKit/userspace transport and compute stack. It is not the
Nouveau NVK winsys, and it is not a Mellow NVIDIA kext, Metal implementation or
WindowServer driver. No TinyGPU adapter, installer or executable is added here.
Adopting it would require a separately reviewed source/compiler/firmware contract,
resource lifetime integration and physical evidence for the chosen machine.

## Readiness and validation boundary

Source-intake results retain `driver_ready=false`, empty implemented entry points
and no admitted PCI IDs. `--require-ready` must continue to fail after emitting
review artifacts. A normal source-intake exit code only reports that requested
review artifacts were generated.

Regression tests should verify the canonical `nouveau` name, compatibility of
`nvidia-open`, adapter identity, family reference boundaries, cross-adapter source
rejection, modeset source inventory, deterministic provenance and closed readiness
gates. Such tests validate host policies and artifacts. GPU execution or Metal
acceptance requires a separate physical test record and cannot be inferred from
these results.
