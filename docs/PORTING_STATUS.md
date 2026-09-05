# Porting status

> **Scope and precedence:** [PLATFORM-DECISIONS](PLATFORM-DECISIONS.md),
> [PLATFORM-ARCHITECTURE](PLATFORM-ARCHITECTURE.md), and
> [IMPLEMENTATION-STATUS](IMPLEMENTATION-STATUS.md) govern conflicting draft assumptions.
> Runtime policy and source-intake tools are runnable; the Metal facade, shader JIT, live GL/CL
> providers and integrated XNU GPU backend are not implemented. No Mellow GPU/Metal PASS exists.

## 한글 요약

**증거를 기록하며, 야심을 기록하지 않는다.** 확대개편에 맞춰 범위를 "7D41 포팅"에서
"평면별·backend별 포팅 상태"로 재조정했다. 증거 사다리 `S/B/L/F/R`와 7개 분류는 그대로다.
IORegistry에 iGPU가 보이거나, framebuffer가 로드되거나, 데스크톱에 도달하는 것은 그 자체로
하드웨어 가속을 증명하지 않는다.
**Mellow 실행 경로의 `L`·`F`·`R` 증거는 없다.** 정책/intake 테스트와 별도의 Windows
OpenCL 기판 성공은 아래처럼 별도 범위로 기록한다.

---

Snapshot: 2026-09-06. This document tracks **evidence, not ambition**. Unless a row explicitly
cites a captured result, every hardware result is `UNVERIFIED`. Seeing an iGPU in the IORegistry,
loading a framebuffer, or reaching a desktop does not by itself prove hardware acceleration.

Evidence levels, change classifications, and the no-manufactured-success rule are defined
normatively in [EVIDENCE-POLICY.md](EVIDENCE-POLICY.md) and summarized here.

## Evidence levels

Cumulative. A component at `F` must also satisfy `S`, `B`, and `L`.

| Level | Meaning |
| --- | --- |
| `S` | A source path exists and has been reviewed. This proves neither compilation nor execution. |
| `B` | A reproducible build completed and its artifact, commit, toolchain, and log were retained. |
| `L` | A hardware log shows that the intended path executed on the physical target. |
| `F` | A deterministic functional test produced the expected GPU result with CPU fallback excluded. |
| `R` | Repeated functional and recovery tests passed across reboot, sleep/wake, and sustained load. |

**No Mellow execution path has `L`, `F`, or `R` evidence.** The Windows vendor OpenCL
substrate probe is a separate observation and does not promote this driver stack.

## Classifications

| Classification | Use |
| --- | --- |
| **Diagnostic** | Adds observation or bypasses a check solely to locate a failure. |
| **Workaround** | Masks a known mismatch without implementing the underlying contract. |
| **Feature-limited implementation** | Implements a documented subset and rejects the rest. |
| **Prototype implementation** | Exercises a plausible path whose correctness and recovery are incomplete. |
| **Stabilization candidate** | Has deterministic functional evidence and is undergoing regression testing. |
| **Production-level implementation** | Has defined compatibility, recovery, regression, and performance evidence. |
| **Not implemented** | No Mellow-owned implementation exists at this layer. |

Device-ID spoofing, assertion bypasses, forced return values, and capability-bit changes are
diagnostics or workarounds. They must never be promoted merely because the system boots.

## Status by plane

### Plane 4 — MellowMTL and MellowJIT

| Layer | Current source state | Classification | Evidence | Hardware result |
| --- | --- | --- | --- | --- |
| Metal object model | No Mellow-owned `MTLDevice` exists. `Info.plist` names unverified Apple TGL bundles not found in the inspected Recovery inputs. | Not implemented | none | **UNVERIFIED** |
| Attachment — interposition | Planned. The `cs_validate_page` hook exists but its body is gated to Sonoma and is dead on Tahoe. | Not implemented | none | **UNVERIFIED** |
| Attachment — driver plug-in | Planned. The `gpu_bundle_find_trusted` discovery hook exists but is commented out at [Mellow/DYLDPatches.cpp:113](../Mellow/DYLDPatches.cpp). | Not implemented | none | **UNVERIFIED** |
| AIR ingestion | Format survey only; see [AIR-ABI.md](AIR-ABI.md). | Not implemented | none | **UNVERIFIED** |
| Shader lowering | No shader translation path exists; Runtime cache identity is policy only, not a compiler. | Not implemented | none | **UNVERIFIED** |
| Offline compiler harness | Intel `ocloc` 26.27.39122.11 / IGC 2.38.2 compiled OpenCL C for `-device 0x7d41`; 6,944-byte ZEBIN with verified EU disassembly. **This is OpenCL C, not Metal.** | Feature-limited tooling | `B` | **UNVERIFIED** |
| Acceptance harnesses | [Tools/metal-probe.swift](../Tools/metal-probe.swift) and [Userspace/metal_session.py](../Userspace/metal_session.py) are careful clients with strong attribution checks. Never compiled or executed — no macOS or Metal in the build environment. | Feature-limited implementation | `S` | **UNVERIFIED** |

### Plane 3 — MellowRT

| Layer | Current source state | Classification | Evidence | Hardware result |
| --- | --- | --- | --- | --- |
| Workload router | Runtime/PlatformRuntime policy contracts and host tests exist; no live provider adapter. | Feature-limited policy implementation | Host tests, see IMPLEMENTATION-STATUS | **UNVERIFIED** |
| Resource model and storage modes | Route/resource contract validation exists; live allocation/storage adapters remain planned. | Feature-limited policy prototype | Host tests, see IMPLEMENTATION-STATUS | **UNVERIFIED** |
| Host provider (Apple GL/CL) | Planned. | Not implemented | none | **UNVERIFIED** |
| Mellow provider (Mesa-derived) | Planned. | Not implemented | none | **UNVERIFIED** |

### Plane 2 — MGAL and MELLOW-UAPI

| Layer | Current source state | Classification | Evidence | Hardware result |
| --- | --- | --- | --- | --- |
| Logic/platform separation | **Already achieved.** Every `Xe*` module is freestanding C++17 with a POD ops struct; host tests compile production sources with `-Werror`. | Feature-limited implementation | `S`, `B` | **UNVERIFIED** |
| Named MGAL interfaces | Not extracted. Device identity is duplicated: `7D41` appears 39 times across 26 files, admission logic in ≥4 binding files. | Not implemented | none | **UNVERIFIED** |
| Composition root | **Absent — the central gap.** Nothing constructs the MMIO, page-table, firmware, transport, interrupt, fence, or context objects. `BackendOwnerIntegrated = false` at [Mellow/RuntimeReadiness.hpp:84](../Mellow/RuntimeReadiness.hpp). | Not implemented | none | **UNVERIFIED** |
| MELLOW-UAPI | No `IOUserClient` exists. The `IOResources` personality has no backing class — the `IOService` subclass at [Mellow/kern_start.cpp:52](../Mellow/kern_start.cpp) is commented out. | Not implemented | none | **UNVERIFIED** |
| Readiness ladder | 20-bit fail-closed gate with ordered stages; monotonic; `mayAdvertiseMetal` requires all bits. | Feature-limited implementation | `S` | Source-predicted stage `physical-provider`, first missing `bar0-mapped`; no physical capture |

### Plane 1 — Backends and MellowKPI

| Layer | Current source state | Classification | Evidence | Hardware result |
| --- | --- | --- | --- | --- |
| MellowKPI | No `linux/*` or `drm/*` headers exist. | Not implemented | none | **UNVERIFIED** |
| `xe` — MMIO / forcewake | Real BAR0 mapping helpers, bounds and ordering, GMD_ID read, GT/render forcewake with ACK and timeout. | Prototype implementation | `S`, `B` | **UNVERIFIED** |
| `xe` — memory and page tables | 48-bit VA, owner/generation, 46-bit DMA, PTE/PDE encoding, pin/bind/retire; 4-level table build with rollback and seal. | Prototype implementation | `S`, `B` | **UNVERIFIED** |
| `xe` — firmware and GuC transport | CSS parse, WOPCM, DMA upload and authentication polling; CTB/HXG packets with credits and epochs. | Prototype implementation | `S`, `B` | **UNVERIFIED** — no device ever authenticated the blob |
| `xe` — submission and execution | **Two rival stacks.** `SubmissionQueue` and `EvidenceExecution` are unconnected; `XeMemorySubmission.hpp` and `XeInterruptDispatch.hpp` are referenced only from `tests/` and are outside the kext include closure. | Prototype implementation | `S` | **UNVERIFIED** |
| `xe` — interrupt and fence | Tile/master/identity handling, `IOFilterInterruptEventSource` and workloop, coherent GGTT qword fence reads. | Prototype implementation | `S`, `B` | **UNVERIFIED** |
| `xe` — ZEBIN loader | ELF64/IntelGT parse, staging, relocation. Hardcoded to one kernel named `.text.mellow_evidence` with exactly eight arguments. | Feature-limited implementation | `S`, `B` | **UNVERIFIED** |
| **`xe` — reachability** | **No call sites. `#include "Xe` appears nowhere outside `Mellow/Xe*`.** Compiling into the kext is structural evidence only. | Not implemented | — | **UNVERIFIED** |
| `applecompat` — ICL | Solve/route lists against `AppleIntelICLLPGraphicsFramebuffer`, which was found in the inspected Tahoe Recovery inputs. | Prototype / workaround | `S` | **UNVERIFIED** |
| `applecompat` — TGL | **`DEPRECATED`.** TGL kext absent from inspected Recovery inputs; full installed-system inventory and provenance unverified. See [LEGACY-DISPOSITION.md](LEGACY-DISPOSITION.md). | Workaround | `S` | **UNVERIFIED** |
| `amdgpu`, selected NVIDIA adapter | Source-intake targets exist; executable GPU backends do not. | Not implemented | none | **UNVERIFIED** |
| Display | `DisplayMergeNub` has a backing IOKit class in source; runtime attach is unverified. Link training, modeset, hotplug, and scanout have no recorded target evidence. | Prototype plus device-specific workaround | `S` | **UNVERIFIED** |

### Plane 0 — `mellow-port`

| Layer | Current source state | Classification | Evidence | Hardware result |
| --- | --- | --- | --- | --- |
| Source intake/report and bounded generation | `Tools/mellow-port.py inspect/plan/generate` exist. No semantic driver translation, XNU binding or backend build. | Feature-limited analysis tooling | Tests, see IMPLEMENTATION-STATUS | — |
| Firmware fetch and verify | [tests/xe_submission_fetch_firmware.py](../tests/xe_submission_fetch_firmware.py) checks size and SHA-256, opens exclusive-create, refuses to write on mismatch. Intel GuC only. | Feature-limited implementation | `B` | — |
| Cross-build | [Tools/cross-build.py](../Tools/cross-build.py) parses the pbxproj source phase, synthesizes `module_info.c`, verifies `MH_KEXT_BUNDLE`, hashes inputs before and after. | Feature-limited implementation | `B` | — |
| ABI validation | [Tools/tahoe-abi.py](../Tools/tahoe-abi.py) resolves imports against real KPI export sets; 378/378 resolved; 15 parser test methods with 28 negative subcases. | Feature-limited implementation | `B` | — |

## Current ceiling

**Prototype / workaround.** Nothing in this repository is a stabilization candidate or a
production-level implementation.

The most consequential single fact: the Intel backend is well-structured, host-tested, and
**completely unreachable**. Structural quality is not execution evidence.

## Phase gates

Full sequencing in [ROADMAP.md](ROADMAP.md). The hardware bring-up order is unchanged:

1. **Environment** — pin macOS and build versions, retain a CI artifact, record a Mellow-disabled
   baseline, prove a known-good recovery boot.
2. **Identification and mapping** — prove physical identity, expected driver selection, and
   positive BAR0 mapping evidence, without interpreting a missing error as success.
3. **Execution primitive** — submit one bounded NOP or copy; correlate ring head/tail, interrupt
   count, fence completion, and output bytes.
4. **Compute** — deterministic buffer kernel; verify every output byte with CPU fallback excluded.
5. **Render** — offscreen first, then scanout, with capture and corruption checks.
6. **Metal** — device, queue, library, pipeline, execution, result correctness, in that order.
   Update [METAL_FEATURE_MATRIX.md](METAL_FEATURE_MATRIX.md) per test.
7. **Stability and optimization** — only after functional evidence exists.

## Safety and recovery rules

- Test only with an OpenCore picker entry that can disable Mellow, and with a separately preserved
  known-good EFI. Confirm that recovery path before the first Mellow boot.
- Keep physical access or an independently tested remote/serial path. Do not perform the first
  experiment on the only bootable installation.
- One change per commit, one hypothesis per experiment. Record the exact commit, artifact hash,
  macOS build, EFI hash, and boot arguments.
- Do not combine Mellow and WhateverGreen during isolation testing.
- Treat `mellow-dmc=skip` as "skip Mellow compatibility writes and pass through Apple's
  initializer", not as a guarantee of zero MMIO writes.
- Never use arbitrary `/dev/mem` access, blind MMIO writes, voltage or clock changes, or firmware
  flashing. A register write requires a cited definition, a target-generation review, a rollback
  plan, and a bounded test.
- Stop immediately on a panic, GPU hang, display corruption, input loss, or repeated timeout. Boot
  the known-good entry, preserve evidence, classify the experiment as failed, and do not stack
  another workaround on it.
- Diagnostic bundles can contain serial numbers, paths, and other private metadata. Review and
  redact before sharing.

## Updating this file

Every status promotion must link to an experiment record containing raw logs, expected and actual
results, a fallback check, and an artifact/commit identity.

Source comments such as `V###`, "fixed", "working", or "loaded" are historical labels only and do
not count as evidence.

## Separate Windows substrate observation

[opencl-windows-substrate.json](../validation/opencl-windows-substrate.json) records
`PASS_OPENCL_GPU_SUBSTRATE_ONLY`: the installed Windows Intel OpenCL driver executed the probe.
`mellow_runtime_used=false`, `mellow_jit_used=false`, `metal_tested=false`,
`macos_driver_tested=false`, `mellow_gpu_acceleration_pass=false`.
The OpenCL-reported GPU identity was not correlated to physical PCI (`physical_pci_identity_verified=false`).
This demonstrates a Windows substrate only; it does not promote a Mellow/backend/family capability.
