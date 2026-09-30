# Intel backend expansion contract

The Intel entries in [gpu-families.json](../porting/gpu-families.json) select
reference-source paths for future porting. Every entry has
`runtime_device_admission=false` and `physical_gpu_verified=false`. The inventory
does not add a PCI match, change boot arguments, select firmware, or register a
Metal device. It is a bounded starting set, not a complete catalogue of every
unsupported Intel GPU or a claim that every member of a family works.

`source_targets` identifies selected source-intake recipes. It does not assert
that all listed upstream drivers are equally mature, that their kernel/user
interfaces are interchangeable, or that an upstream match establishes a Darwin
backend. A family label must never activate an MMIO, VM, firmware, or submission
profile without an exact physical device and IP match.

## Pinned reference paths

Intel references use Linux commit
`0d9ff90a5422cc7509258aaaba1e7481df4d332a`. The
[PCI definitions](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/include/drm/intel/pciids.h)
and [i915 device table](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/i915/i915_pci.c)
provide identity and profile references. Ice Lake uses the i915 intake path;
its individual GT variants require separate evaluation. The selected intake
paths for Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake, DG1, and DG2 include
both i915 and Xe. This is source availability, not automatic migration between
their submission or userspace contracts.

The [Xe device table](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/xe/xe_pci.c)
records these distinctions:

| Selected family | Xe discovery reference | Xe DMA width reference | Porting implication |
| --- | --- | --- | --- |
| Tiger/Rocket/Alder/Raptor Lake | Pre-GMD PCI profile, Xe-LP | 39 bits | Separate legacy IP and memory profile |
| DG1 | Pre-GMD PCI profile, Xe-LP+ | 39 bits | Discrete memory and device lifetime require review |
| DG2 / Alchemist | Pre-GMD PCI profile, Xe-HPG | 46 bits | Local memory and page-size policy require review |
| Meteor/Arrow Lake | GMD-derived IP | 46 bits | Discover exact graphics/media IP and stepping |
| Lunar Lake | GMD-derived IP, Xe2-LPG | 46 bits | Separate Xe2 VM and command profile |
| Battlemage | GMD-derived IP, Xe2-HPG | 46 bits | Separate Xe2 discrete-memory profile |

`upstream_dma_address_bits` and `upstream_discovery` in the JSON describe this
pinned Xe table. They are not measurements of an attached device or settings
applied by Mellow. Several listed Xe profiles require upstream `force_probe`;
the existence of a source entry is not an upstream production-support promise.

The table does not classify all Ice Lake devices as macOS-unsupported.
WhateverGreen's [Intel documentation](https://github.com/acidanthera/WhateverGreen/blob/master/Manual/FAQ.IntelHD.en.md)
documents native Iris Plus support from macOS 10.15.4 and specific device IDs.
An unsupported GT variant, an operating-system support removal, and a new GPU
architecture are different porting cases. Older GT1/Atom families are outside
this initial inventory and need explicit reference profiles before expansion.

## Existing Mellow scope

[kern_model.hpp](../Mellow/kern_model.hpp) still binds its legacy experiment to
specific physical Meteor Lake and Arrow Lake CPU/GPU pairs.
[kern_mellow.cpp](../Mellow/kern_mellow.cpp) obtains PCI identity before consulting
injected properties. Changing a family inventory must not bypass those checks
or turn the TGL spoof ID into physical hardware evidence.

The native readiness contract in
[RuntimeReadiness.hpp](../Mellow/RuntimeReadiness.hpp) remains specific to
`8086:7D41` and graphics GMD 12.70, with `BackendOwnerIntegrated=false`.
[TahoeDiagnostic](TAHOE-DRIVER-IMPLEMENTATION.md) provides a narrowly admitted
PCI/query/prepared-DMA experiment, not GuC submission or Metal acceleration.
The new inventory neither changes nor generalizes these runtime interfaces.

[PortedXe page-table helpers](../Drivers/PortedXe/XePageTable.hpp) implement an
explicit Xe-LPG encoding contract. Existing
[GuC firmware code](../Mellow/XeGuCFirmware.hpp) expects the pinned MTL firmware,
46-bit DMA, and a particular GGTT/PAT/ADS protocol. Reusing these contracts for
Xe-LP or Xe2 by adding PCI IDs would be incorrect. Firmware bytes, version,
authentication, WOPCM, ADS, engines, and reset lifetime must be reviewed for each
selected profile. Firmware provenance and authentication are different proofs.

The [Linux Xe firmware documentation](https://docs.kernel.org/gpu/xe/xe_firmware.html)
describes CSS-based GuC images and different HuC/GSC layouts. A successful GuC
boot or minimal hardware-configuration ADS does not establish a submission
profile, authenticated media firmware, or running shader.

## Required implementation and evidence

An implementer must establish all of the following for one exact device before
expanding runtime admission:

1. Physical PCI/subsystem identity, revision, engine topology, graphics/media IP,
   exclusive device ownership, bounded BAR access, and power/forcewake lifetime.
2. Actual device-mapper DMA ownership, address width, GGTT/PPGTT encoding, PAT/MOCS,
   page sizes, residency, publication, and completed invalidation. Failed or
   uncertain teardown retains backing until a verified quiesce/reset boundary.
3. Device-specific firmware provenance, permitted acquisition, authentication,
   full submission ADS, transport, interrupt, fence, and reset-generation rules.
4. Validated native command and resource ABI, command rejection, queue ordering,
   completion observations, and error propagation through the user/kernel boundary.
5. A compatible userspace provider and target compiler. Intel's
   [compute runtime](https://github.com/intel/compute-runtime) and
   [graphics compiler](https://github.com/intel/intel-graphics-compiler) are source
   references; source presence does not connect their Linux runtime ABI to XNU
   or supply the Apple Objective-C Metal driver ABI.
6. Independent compute/readback, render/pixel, timeout, reset, and stress evidence
   bound to the physical device and exact OS/driver/compiler build. IOSurface,
   system Metal registration, display scanout, WindowServer, sleep/wake, and
   multimonitor integration have separate acceptance results.

Current [implementation status](IMPLEMENTATION-STATUS.md) distinguishes portable
MSL/AIR lowering, host GL/CL execution, partial Xe algorithms, and diagnostic
kext linkage from native macOS acceleration. Intake artifacts, generated
constants, host tests, a compiled kext, or a visible desktop cannot upgrade a
family's `physical_gpu_verified` value. Preserve missing, unsupported, and
not-run states until the corresponding physical evidence exists.
