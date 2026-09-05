# GPU support matrix

> **Design draft; implementation status is separate.**
> [PLATFORM-DECISIONS](PLATFORM-DECISIONS.md) and [PLATFORM-ARCHITECTURE](PLATFORM-ARCHITECTURE.md)
> take precedence over conflicting assumptions below. [IMPLEMENTATION-STATUS](IMPLEMENTATION-STATUS.md)
> records the runnable policy/intake code. No GPU, Metal, WindowServer or display acceptance has passed.

## 한글 요약

**현재 어떤 GPU도 Mellow를 통해 가속되지 않는다.** 모든 행이 증거 등급 `S` 또는 그 이하이며,
`L`·`F`·`R` 기록은 0건이다. Intel Xe 계열만 소스 경로가 존재하고, 그마저 호출되지 않는다.
AMD·NVIDIA GPU 실행 backend는 미구현이다. source-intake target은 driver 지원이 아니다.
이 표는 계획이 아니라 **실적**을 기록한다. 계획은 [ROADMAP.md](ROADMAP.md)에 있다.

---

Status vocabulary and evidence levels are defined in [EVIDENCE-POLICY.md](EVIDENCE-POLICY.md).
This document records **what has been achieved**, not what is intended. Intent lives in
[ROADMAP.md](ROADMAP.md).

## Summary

| | |
| --- | --- |
| GPUs accelerated through Mellow | **0** |
| Backends with a composition root | **0** |
| Rows at evidence level `L`, `F`, or `R` | **0** |
| Executable non-Intel GPU backends | **none** |

## Per-GPU status

### Intel — Xe-LPG / Xe2

The only family with any source path. Device table at
[Mellow/kern_model.hpp](../Mellow/kern_model.hpp).

| Device | Name | Backend | Evidence | Status |
| --- | --- | --- | --- | --- |
| `8086:7D40` | Intel Graphics (Meteor Lake) | `xe` | `S` | `SOURCE PATH` — recognized in the device table only |
| `8086:7D45` | Intel Graphics (Meteor Lake) | `xe` | `S` | `SOURCE PATH` |
| `8086:7D55` | Intel Arc Graphics (Meteor Lake) | `xe` | `S` | `SOURCE PATH` |
| `8086:7DD5` | Intel Graphics (Meteor Lake) | `xe` | `S` | `SOURCE PATH` |
| `8086:7D41` | Intel Graphics 4-Core (Arrow Lake-U) | `xe` | `S` | `SOURCE PATH` — the only device with backend code written for it |
| `8086:7D51` | Intel Graphics (Arrow Lake-H) | `xe` | `S` | `SOURCE PATH` |
| `8086:7D67` | Intel Graphics (Arrow Lake-S) | `xe` | `S` | `SOURCE PATH` |

Even for `7D41`, the backend is unreachable: the `Xe*` modules have no call sites and no IOKit
owner, recorded at [Mellow/RuntimeReadiness.hpp:84](../Mellow/RuntimeReadiness.hpp) and analysed in
[NATIVE-XE-BACKEND-AUDIT.md](NATIVE-XE-BACKEND-AUDIT.md). Source readiness evaluation predicts
stage `physical-provider`, first missing bit `bar0-mapped`; no physical capture establishes this state.

Recognition in a device table does not imply support of any kind.

### Intel — Gen9 through Gen12 (legacy path)

| Family | Backend | Evidence | Status |
| --- | --- | --- | --- |
| Ice Lake (ICL) | `applecompat` | `S` | `SOURCE PATH` — `AppleIntelICLLPGraphicsFramebuffer` exists on Tahoe |
| Tiger Lake (TGL) | `applecompat` | — | **`DEPRECATED`** — no Apple TGL kext exists on Tahoe; see [LEGACY-DISPOSITION.md](LEGACY-DISPOSITION.md) |

### AMD

| Target | ASIC | Backend | Evidence | Status |
| --- | --- | --- | --- | --- |
| RX 9070 | Navi 48 (RDNA 4, gfx12) | `amdgpu` | — | `NOT IMPLEMENTED` |
| RDNA 2 / 3 generally | — | `amdgpu` | — | `NOT IMPLEMENTED` |

**No AMD code exists in this tree.** `0x1002` appears twice in the source and neither occurrence is
a PCI vendor ID — one is commented-out dead code in
[Mellow/HDMI.cpp](../Mellow/HDMI.cpp), the other is a GuC HXG action number in
[Mellow/XeGuCTransport.cpp](../Mellow/XeGuCTransport.cpp).

Upstream source is available; per-file notices and the full DRM/Linux dependency closure require
review before integration. See [LICENSING.md](LICENSING.md).

### NVIDIA

| Target | ASIC | Backend | Evidence | Status |
| --- | --- | --- | --- | --- |
| RTX 3080 | GA102 (Ampere) | NVIDIA adapter undecided | — | `NOT IMPLEMENTED` |
| RTX 3090 | GA102 (Ampere) | NVIDIA adapter undecided | — | `NOT IMPLEMENTED` |
| Turing through Ada generally | — | NVIDIA adapter undecided | — | `NOT IMPLEMENTED` |

**No executable NVIDIA GPU backend exists.** Source-intake support is not runtime driver support.

Upstream source is available: NVIDIA's `open-gpu-kernel-modules`, dual MIT/GPL-2.0, supporting
Turing and later — which covers GA102. This RM/UAPI path requires matching GSP and userspace
components; it is not interchangeable with Mesa NVK/Nouveau. Select and validate the adapter
contract first. The project currently fetches firmware rather than redistributing it; see RFC D09.

## Per-plane readiness, all backends

| Plane | Component | Status |
| --- | --- | --- |
| 4 | MellowMTL — Metal object model | `NOT IMPLEMENTED` |
| 4 | MellowJIT — AIR ingestion | `NOT IMPLEMENTED` |
| 3 | MellowRT — router and resource model | PlatformRuntime policy contracts exist; live provider/resource execution is not implemented |
| 3 | Host provider (Apple GL/CL) | `NOT IMPLEMENTED` |
| 3 | Mellow provider (`libMellowGL` / `libMellowCL`) | `NOT IMPLEMENTED` |
| 2 | MGAL interfaces | `NOT IMPLEMENTED` — the underlying separation exists in `Xe*` at `S` |
| 2 | MELLOW-UAPI | `NOT IMPLEMENTED` |
| 2 | Composition root | `NOT IMPLEMENTED` — this is the central gap |
| 1 | MellowKPI | `NOT IMPLEMENTED` |
| 1 | `xe` backend | `SOURCE PATH` at `S`, unreachable |
| 1 | `applecompat` backend | `SOURCE PATH` at `S`; TGL half deprecated |
| 1 | `amdgpu`, selected NVIDIA adapter | `NOT IMPLEMENTED` |
| 0 | `mellow-port` | Intake/report generation exists; complete semantic driver port remains unimplemented |

## What has actually been executed

To be precise about where the project genuinely stands, these things did run:

| Artifact | What ran | What it does not show |
| --- | --- | --- |
| [compiler-evidence/](../compiler-evidence) | Intel `ocloc` 26.27.39122.11 / IGC 2.38.2 compiled OpenCL C for `-device 0x7d41`, producing a 6,944-byte ZEBIN and 1,636-byte SPIR-V with verified EU disassembly | Nothing about Metal. This is OpenCL C, not MSL, and the two use unrelated test vectors |
| [validation/xe-tests.json](../validation/xe-tests.json) | Production `Xe*` sources compiled and executed on a host against emulated MMIO and ownership callbacks | Nothing about hardware. The OS and GPU boundaries are explicit test backends |
| [abi-evidence/](../abi-evidence) | Real Apple binaries from macOS 26.6.2 build 25G83 parsed; 378/378 kext imports resolved against KPI export sets | Symbol names and addresses only — no selector numbers, vtable slots, or struct layouts |
| [tests/xe_guc_firmware_results.json](../tests/xe_guc_firmware_results.json) | The pinned 320,320-byte Intel GuC blob parsed by production code; hash, length, and version verified | No device ever authenticated it |

Every one of these carries explicit negative-claim fields, and none of them raises any row above
`S` or `B`.

## How a row moves

A row advances only through the readiness ladder in
[Mellow/RuntimeReadiness.hpp](../Mellow/RuntimeReadiness.hpp), stage by stage, each requiring the
previous:

`Configuration → PhysicalProvider → AddressSpace → Firmware → Execution → KernelProvider →
Userspace → Ready`

The bring-up sequence per GPU is in [ADDING-A-GPU.md](ADDING-A-GPU.md). Capability bits are exposed
only after an `F`-level test, per [EVIDENCE-POLICY.md](EVIDENCE-POLICY.md).
