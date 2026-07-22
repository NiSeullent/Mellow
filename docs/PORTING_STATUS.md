# Mellow porting status

Snapshot: 2026-07-22. This document tracks evidence, not ambition. Unless a row
explicitly cites a captured result, every hardware result is **UNVERIFIED**.
Seeing an iGPU in IORegistry, loading a framebuffer, or reaching a desktop does
not by itself prove hardware acceleration.

## Evidence and classification

Evidence levels are cumulative:

| Level | Meaning |
| --- | --- |
| `S` | A source path exists and has been reviewed. This proves neither compilation nor execution. |
| `B` | A reproducible build completed and its artifact, commit, toolchain, and log were retained. |
| `L` | A hardware log shows that the intended path executed on the physical target. |
| `F` | A deterministic functional test produced the expected GPU result with CPU fallback excluded. |
| `R` | Repeated functional and recovery tests passed across reboot, sleep/wake, and sustained load. |

No `L`, `F`, or `R` evidence is recorded in this repository snapshot.

Changes must also carry one of these classifications:

| Classification | Use |
| --- | --- |
| **Diagnostic** | Adds observation or bypasses a check solely to locate a failure. |
| **Workaround** | Masks a known mismatch without implementing the underlying contract. |
| **Feature-limited implementation** | Implements a documented subset and rejects the rest. |
| **Prototype implementation** | Exercises a plausible path whose correctness and recovery are incomplete. |
| **Stabilization candidate** | Has deterministic functional evidence and is undergoing regression testing. |
| **Production-level implementation** | Has defined compatibility, recovery, regression, and performance evidence. |
| **Not implemented** | No Mellow-owned implementation exists at this layer. |

Device-ID spoofing, assertion bypasses, forced return values, and capability-bit
changes are diagnostics or workarounds. They must never be promoted merely
because the system boots.

## Current layer-by-layer status

| Layer | Current source state | Classification | Evidence | Hardware result |
| --- | --- | --- | --- | --- |
| Scope gate | `MellowCore::init` admits Intel family 6 models `AA/AC/B5/C5/C6`; `processPatcher` reads the physical PCI vendor/device before injected properties and checks an explicit CPU/GPU pair. | Feature-limited safety implementation | `S` | **UNVERIFIED** |
| Product/build identity | Mellow bundle, scheme, workflow, boot-argument namespace, license, and attribution are present. No build result is attached here. | Build infrastructure | `S`; `B` pending | **UNVERIFIED** |
| Apple driver dependency | The plug-in registers and patches Apple TGL framebuffer/accelerator paths and expects compatible TGL user-space bundles. Mellow does not contain a replacement Apple driver stack. | Prototype integration | `S` | **UNVERIFIED** |
| PCI discovery | Physical `8086:<device>` and revision are read through `IOPCIDevice`; OpenCore's `9A49` property spoof is separate. | Diagnostic plus safety gate | `S` | **UNVERIFIED** |
| BAR/MMIO mapping | BAR0 and BAR2 mapping helpers exist. The BAR0 path emits the read-only `E0001 BAR0 mapped` marker with length and two bounded display-register reads, but no target log proves it has executed safely. | Diagnostic / prototype implementation | `S` | **UNVERIFIED** |
| Firmware/DMC | `mellow-dmc` selects `adlp`, `tgl`, `icl`, or `skip`; missing defaults to `adlp`. The profiles reuse Apple initialization and compatibility MMIO sequences; they are not a validated Xe-LPG firmware loader. | Workaround / prototype | `S` | **UNVERIFIED** |
| Reset and power wells | Inherited TGL/ADL-P-oriented reset, force-wake, C-state, and power-well routes are present. Register semantics and timeout recovery have not been validated for the listed Ultra devices. | Prototype implementation | `S` | **UNVERIFIED** |
| Display/framebuffer | The TGL framebuffer can be selected, topology and connector-related paths are patched, and target-panel timings are opt-in. Link training, modeset, hotplug, and scanout correctness have no recorded target evidence. | Prototype plus device-specific workaround | `S` | **UNVERIFIED** |
| GPU topology | The compatibility topology is selected by the physical Ultra ID, including an 8-subslice/64-EU compatibility shape for `7D41`. This is an Apple-driver-facing model, not proof of physical resource discovery. | Workaround | `S` | **UNVERIFIED** |
| GGTT/GPU virtual memory | The code contains inherited GGTT, aperture, context, and page-table patches. No address-map audit, fault trace, or read-back test proves Xe-LPG VM correctness. | Prototype implementation | `S` | **UNVERIFIED** |
| Command submission | Inherited ring/context/submission hooks and several versioned experimental patches exist. There is no minimal NOP, copy, or heartbeat test with head/tail and completion evidence. | Diagnostic / prototype | `S` | **UNVERIFIED** |
| Interrupts and fences | Interrupt-, stamp-, and wait-related hooks exist, including forced-success paths. A forced completion is not proof of an interrupt or fence. No interrupt-counter correlation is retained. | Diagnostic / workaround | `S` | **UNVERIFIED** |
| Fault detection/recovery | Logging and scattered timeout guards exist. A defined reset-on-hang state machine and fault-injection result do not. | Diagnostic; implementation incomplete | `S` | **UNVERIFIED** |
| IOAccelerator connection | Mellow patches `IOAcceleratorFamily2` and Apple TGL accelerator entry points. Capability-check bypasses and forced return values do not establish a correct IOAccel ABI. | Workaround / prototype | `S` | **UNVERIFIED** |
| Metal bundle discovery | DYLD hooks can log TGL Metal/GL/VA bundle validation and contain version-specific CoreDisplay experiments. Seeing a bundle is only load-path evidence. | Diagnostic / workaround | `S` | **UNVERIFIED** |
| Metal device and queues | No retained result proves a real `MTLDevice`, command queue, completed command buffer, or non-CPU execution. | Not verified; underlying implementation unknown | `S` path only | **UNVERIFIED** |
| Shader compiler/ISA | Mellow has no MSL/Metal-IR-to-Xe-LPG compiler, ISA translator, or independently validated TGL-binary compatibility layer. | Not implemented | none | **UNVERIFIED** |
| Rendering and compute | No deterministic blit, compute, triangle, texture, blending, or depth result is recorded. | Not verified | none | **UNVERIFIED** |
| WindowServer compositing | Version-specific crash guards exist. Avoiding a crash, showing pixels, or forcing an accessor result does not prove hardware compositing. | Diagnostic / workaround | `S` | **UNVERIFIED** |
| Lifecycle/stability | Reboot, modeset, hotplug, sleep/wake, memory pressure, long load, and multi-process tests are not recorded. | Not tested | none | **UNVERIFIED** |

The current ceiling is **prototype/workaround**. Nothing is a stabilization
candidate or production-level implementation yet.

## Phase gates

1. **Phase 0 — environment:** pin macOS/build versions, retain a CI artifact,
   record a Mellow-disabled baseline, and prove a known-good recovery boot.
2. **Phase 1 — identification and mapping:** complete `E0001`; prove physical
   identity, expected kext selection, and positive BAR0 mapping evidence without
   interpreting a missing error as success.
3. **Phase 2 — execution primitive:** submit one bounded NOP or copy, correlate
   ring head/tail, interrupt count, fence completion, and output bytes.
4. **Phase 3 — compute:** run a deterministic buffer kernel and verify every
   output byte while excluding CPU fallback.
5. **Phase 4 — render:** render off-screen first, then scan out a triangle with
   capture and corruption checks.
6. **Phase 5 — Metal:** prove device, queue, library, pipeline, execution, and
   result correctness in that order. Update the feature matrix per test.
7. **Phase 6/7 — stability and optimization:** begin only after functional
   evidence exists; preserve recovery and regression data.

## Safety and recovery rules

- Test only with an OpenCore picker entry that can disable Mellow and with a
  separately preserved known-good EFI. Confirm that recovery path before the
  first Mellow boot.
- Keep physical access or an independently tested remote/serial path. Do not
  perform the first experiment on the only bootable installation.
- Start with one change per commit and one hypothesis per experiment. Record
  the exact commit, artifact hash, macOS build, EFI hash, and boot arguments.
- Do not combine Mellow and WhateverGreen during isolation testing. Do not use
  `-mellow7d41timings` outside the exact reviewed `7D41` panel experiment.
- Treat `mellow-dmc=skip` as “skip Mellow compatibility writes and pass through
  Apple's initializer,” not as a guarantee of zero MMIO writes.
- Never use arbitrary `/dev/mem` access, blind MMIO writes, voltage/clock
  changes, or firmware flashing. A register write requires a cited definition,
  a target-generation review, a rollback plan, and a bounded test.
- Stop immediately on a panic, GPU hang, display corruption, input loss, or
  repeated timeout. Boot the known-good entry, preserve panic/log evidence, and
  classify the experiment as failed; do not stack another workaround on it.
- Diagnostic bundles can contain serial numbers, paths, and other private
  metadata. Review and redact them before sharing.

## Updating this file

Every status promotion must link to an experiment record containing raw logs,
expected and actual results, a fallback check, and an artifact/commit identity.
Source comments such as `V###`, “fixed,” “working,” or “loaded” are historical
labels only and do not count as evidence for an Ultra target.
