# Metal feature matrix

> **Scope and precedence:** [PLATFORM-DECISIONS](PLATFORM-DECISIONS.md),
> [PLATFORM-ARCHITECTURE](PLATFORM-ARCHITECTURE.md), and
> [IMPLEMENTATION-STATUS](IMPLEMENTATION-STATUS.md) govern conflicting draft assumptions.
> Runtime policy and source-intake tools are runnable; the Metal facade, shader JIT, live GL/CL
> providers and integrated XNU GPU backend are not implemented. No Mellow GPU/Metal PASS exists.

## 한글 요약

이 표는 **시험 계약**이다. 번들 이름, 기능 플래그, 부팅 성공, 화면 출력으로 지원을 추론하지 않는다.
아래 Mellow Metal 기능 행은 **모두 하드웨어 `UNVERIFIED`**다. Windows OpenCL 기판 시험은
별도 성공 기록이며 Mellow 실행이나 Metal 지원으로 승격하지 않는다.
확대개편에 맞춰 두 가지가 바뀌었다 — 능력별로 **어느 경로(`cl`/`gl`/`native`)가 그것을 제공하는지**
명시하고, backend별 열을 추가했다. 능력 비트는 `F` 등급 시험 통과 후에만 켠다.

---

This matrix is a test contract. It does not infer support from a bundle name, a feature flag, a
successful boot, or a visible framebuffer. Every Mellow capability row below is hardware
`UNVERIFIED`; the separate Windows OpenCL substrate result is described after the matrix.

Status vocabulary and evidence levels are defined in [EVIDENCE-POLICY.md](EVIDENCE-POLICY.md):

- `SOURCE PATH` — related matching, routing, or patch code exists.
- `PLANNED` — described in a design document; no implementation exists.
- `UNKNOWN` — the responsible platform or hardware contract has not been established.
- `NOT IMPLEMENTED` — no Mellow-owned or validated compatible implementation exists.
- `BLOCKED` — a prerequisite test has not passed.
- `VERIFIED` — reserved for a linked, deterministic experiment that excludes CPU fallback.
  **No row currently qualifies.**

## Backend columns

Per-backend readiness is tracked in [GPU-SUPPORT-MATRIX.md](GPU-SUPPORT-MATRIX.md). Summarized:

| Backend | Target | Overall |
| --- | --- | --- |
| `xe` | Intel Xe-LPG / Xe2 | `SOURCE PATH`, unreachable — no IOKit owner |
| `applecompat` | Intel ICL via Apple's framebuffer | `SOURCE PATH`; TGL half `DEPRECATED` |
| `amdgpu` | RX 9070 (Navi 48) | `NOT IMPLEMENTED` |
| Selected NVIDIA adapter, TBD | RTX 3080 / 3090 (GA102) | `NOT IMPLEMENTED` |

Because no backend has reached `Execution` on the readiness ladder, **no capability row differs by
backend yet**. Per-backend columns are added to the tables below when the first backend produces a
result that another does not.

## Capability rows

The `Route` column names which [Plane 3](WORKLOAD-RUNTIME.md) route is intended to provide the
capability — this is new, and it is what makes the matrix actionable under the MELLOW architecture.

### Device and submission

| Capability | Route | Status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- | --- |
| `MTLDevice` creation | any | `NOT IMPLEMENTED` | No Mellow-owned `MTLDevice` exists. `Info.plist` names unverified Apple bundles not found in the inspected Recovery inputs. | **UNVERIFIED** | Enumerate a Mellow device; correlate its registry ID with a physical PCI device. |
| Truthful family/feature exposure | any | `NOT IMPLEMENTED` | PlatformRuntime validates adapter-supplied capability contracts; real Metal queries/provider evidence are absent. | **UNVERIFIED** | Record all capability queries; keep each bit disabled until its conformance test passes. |
| Command queue creation | any | `NOT IMPLEMENTED` | No queue object or completion trace is retained. | **UNVERIFIED** | Create one queue, submit an empty command buffer, correlate submission and completion. |
| Command-buffer ordering and callbacks | any | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Submit ordered writes; verify callback order with forced-success diagnostics disabled. |
| Error propagation | any | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Force a failure; confirm `MTLCommandBufferStatusError` with a populated `error`. |

### Resources

| Capability | Route | Status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- | --- |
| Buffer allocation, `Shared` / `Private` | `cl`, `gl` | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Fill with sentinels, execute a GPU copy, synchronize, compare all bytes. |
| `Managed` storage mode | `cl`, `gl` | `NOT IMPLEMENTED` | The most error-prone mode to emulate — fails as stale data, not as an error. | **UNVERIFIED** | Differential test against the `cpu` reference across `didModifyRange` boundaries. |
| `Memoryless` storage mode | — | `NOT IMPLEMENTED` | Meaningful only for tile-based rendering. | **UNVERIFIED** | Keep unexposed on desktop backends. |
| GPU virtual addressing / residency | `native` | `SOURCE PATH` | `XeMemory` and `XePageTable` exist with host tests; no owner constructs them. | **UNVERIFIED** | Trace VA-to-PTE mappings and controlled fault/unmap behavior for one buffer. |
| Texture allocation and layouts | `gl` | `NOT IMPLEMENTED` | No layout, tiling, compression, pitch, or plane result. | **UNVERIFIED** | Round-trip uncompressed 2D textures through blit; compare per-pixel bytes. |
| Pixel and texture formats | `gl`, `cl` | `NOT IMPLEMENTED` | Format list is populated from passing tests, never from a capability table. | **UNVERIFIED** | Table-driven upload/sample/render/read-back per exposed format. |
| Resource heaps and aliasing | `native` | `UNKNOWN` | — | **UNVERIFIED** | Allocate aliased heap resources; verify hazard and lifetime behavior. |
| IOSurface interop | `gl` | `NOT IMPLEMENTED` | Import/export, format, lifetime and sync contracts remain unimplemented. | **UNVERIFIED** | Write from one provider, read from another, compare bytes. |

### Shaders

| Capability | Route | Status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- | --- |
| AIR ingestion | any | `NOT IMPLEMENTED` | Format survey only; see [AIR-ABI.md](AIR-ABI.md). Needs a versioned SDK adapter or separately implemented frontend; framework symbols are insufficient. | **UNVERIFIED** | Parse a metallib produced by the local toolchain; recover entry points and bindings. |
| AIR/MIR → OpenCL C; optional supported IL, compute (J1) | `cl` | `PLANNED` | The project's highest-risk component; also its cheapest early proof. | **UNVERIFIED** | Compile a Metal kernel through MellowJIT; run on Apple's OpenCL; compare every output byte. |
| Threadgroup memory and barriers (J2) | `cl` | `PLANNED` | SIMD width, SLM layout, barrier encoding all unresolved. | **UNVERIFIED** | Multi-threadgroup reduction with adversarial scheduling, repeated. |
| Atomics | `cl` | `PLANNED` | Width, scope, and ordering mapping unverified. | **UNVERIFIED** | Contended counters for every exposed type and scope, with exact totals. |
| AIR → GLSL, graphics (J3) | `gl` | `PLANNED` | Bounded by OpenGL 4.1 core on macOS. | **UNVERIFIED** | Offscreen triangle; per-pixel comparison against a reference. |
| Native ISA via backported Mesa | `native` | `NOT IMPLEMENTED` | Requires a working backend and a ported compiler. | **UNVERIFIED** | — |
| ZEBIN loading (Intel) | `native` | `SOURCE PATH` | [Mellow/XeZebin.cpp](../Mellow/XeZebin.cpp) is well-built but hardcoded to one kernel: exactly one text section named `.text.mellow_evidence`, exactly eight arguments. | **UNVERIFIED** | Generalize, then load an arbitrary compiled kernel. |
| Pipeline cache and serialization | any | `PLANNED` | Key must include AIR hash, backend ID, driver version, JIT version. | **UNVERIFIED** | Cold and warm cache across process restart and reboot, with version invalidation. |

### Rendering

| Capability | Route | Status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- | --- |
| Blit encoder | `gl`, `native` | `NOT IMPLEMENTED` | First execution primitive. | **UNVERIFIED** | GPU copy patterned buffers; prove engine progress, fence completion, byte equality. |
| Compute pipeline and dispatch | `cl` | `NOT IMPLEMENTED` | Depends on AIR ingestion and the resource model. | **UNVERIFIED** | Vector add with randomized inputs, guard regions, exact output comparison. |
| Render pipeline creation | `gl` | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Build a pass-through vertex and solid-color fragment pipeline. |
| Rasterization and render targets | `gl` | `NOT IMPLEMENTED` | A visible framebuffer is not render-engine evidence. | **UNVERIFIED** | Offscreen triangle; compare coverage and color against a reference. |
| Vertex/fragment interpolation | `gl` | `NOT IMPLEMENTED` | Coordinate conventions differ between Metal and GL and must be reconciled explicitly. | **UNVERIFIED** | Vertex-color triangle with pixel tolerances and edge rules. |
| Texture sampling and samplers | `gl` | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Nearest and linear sampling with edge and address-mode cases. |
| Blending and write masks | `gl` | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Layer known colors; compare per format. |
| Depth and stencil | `gl` | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Overlapping geometry across all compare modes and stencil transitions. |
| MSAA and resolves | `gl` | `UNKNOWN` | — | **UNVERIFIED** | Render edges at each exposed sample count; validate resolve output. |

### Synchronization and binding

| Capability | Route | Status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- | --- |
| `MTLFence` — intra-buffer | any | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Cross-encoder producer/consumer with timeout and negative ordering cases. |
| `MTLEvent` — cross-buffer | any | `NOT IMPLEMENTED` | — | **UNVERIFIED** | Ordered cross-buffer dependency with a deliberate deadlock case. |
| `MTLSharedEvent` — cross-process | any | `UNKNOWN` | Needs a shared primitive on both provider kinds. | **UNVERIFIED** | Two processes signalling and waiting on one event. |
| Mixed-route command buffers | multiple | `NOT IMPLEMENTED` | Initially **rejected** rather than emitted with unguaranteed ordering. | **UNVERIFIED** | Compute on `cl` then render on `gl`, ordering asserted. |
| Argument buffers Tier 1 | any | `UNKNOWN` | Encoding in AIR not yet surveyed. | **UNVERIFIED** | Bind multiple buffers, textures, and samplers through the tier. |
| Argument buffers Tier 2 | any | `NOT IMPLEMENTED` | Metal 3 gated. | **UNVERIFIED** | — |
| Indirect command buffers | `native` | `UNKNOWN` | — | **UNVERIFIED** | Enable only after direct paths pass; compare direct and indirect output. |

### Metal 3 and beyond

All gated. `supportsFamily(.metal3)` is answered `false` until the corresponding conformance
subset passes.

| Capability | Status | Note |
| --- | --- | --- |
| Mesh shaders | `NOT IMPLEMENTED` | — |
| MetalFX upscaling | `NOT IMPLEMENTED` | — |
| Fast resource loading | `NOT IMPLEMENTED` | — |
| Ray tracing / acceleration structures | `NOT IMPLEMENTED` | — |
| Tessellation | `UNKNOWN` | Keep unexposed until a conformance sample passes. |
| Sparse resources | `UNKNOWN` | Keep unexposed until map/unmap/fault tests pass. |
| `MTL4*` surface | **out of scope** | See [METAL-EMULATION.md](METAL-EMULATION.md). |

### System integration

| Capability | Status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- |
| Display presentation | `SOURCE PATH` | `DisplayMergeNub` is the only live IOKitPersonality; hardware-backed presentation unproven. | **UNVERIFIED** | Present a known surface, capture scanout, correlate engine activity. |
| WindowServer acceleration | `BLOCKED` | Requires [Strategy A](METAL-EMULATION.md), which requires a private ABI that has not been recovered. | **UNVERIFIED** | Correlate WindowServer's Metal device, GPU counters, frame timing, CPU-fallback checks. |
| Fault reporting and recovery | `UNKNOWN` | — | **UNVERIFIED** | Inject a bounded invalid command on a disposable setup; verify recovery. |
| Sleep/wake and long-duration load | `BLOCKED` | Functional milestones have not passed. | **UNVERIFIED** | Only after correctness: repeated sleep/wake and one-hour mixed load. |

## Promotion rule

A row becomes `VERIFIED` only when its experiment records inputs, expected and actual outputs,
physical GPU execution evidence, CPU-fallback exclusion, exact software versions, and reproducible
commands.

Any capability that fails must be disabled or reported unsupported. It must not remain exposed
behind a forced feature flag.

**A result produced through the `cpu` route never promotes a row.** That route exists to generate
reference values for differential testing, and using it as evidence of acceleration is precisely
the failure mode this matrix exists to prevent. See [WORKLOAD-RUNTIME.md](WORKLOAD-RUNTIME.md).

## Separate Windows substrate observation

[opencl-windows-substrate.json](../validation/opencl-windows-substrate.json) records
`PASS_OPENCL_GPU_SUBSTRATE_ONLY`: the installed Windows Intel OpenCL driver executed the probe.
`mellow_runtime_used=false`, `mellow_jit_used=false`, `metal_tested=false`,
`macos_driver_tested=false`, `mellow_gpu_acceleration_pass=false`.
The OpenCL-reported GPU identity was not correlated to physical PCI (`physical_pci_identity_verified=false`).
This demonstrates a Windows substrate only; it does not promote a Mellow/backend/family capability.
