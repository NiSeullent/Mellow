# Metal feature matrix

This matrix is a test contract. It does not infer support from a bundle name,
feature flag, successful boot, or visible framebuffer. At this snapshot, every
hardware result is **UNVERIFIED**.

Status vocabulary:

- `SOURCE PATH`: related matching, routing, or patch code exists.
- `UNKNOWN`: the responsible Apple/hardware contract has not been established.
- `NOT IMPLEMENTED`: no Mellow-owned implementation or validated compatible
  implementation exists.
- `BLOCKED`: a prerequisite test has not passed.
- `VERIFIED`: reserved for a linked, deterministic experiment that excludes CPU
  fallback. No row currently qualifies.

| Capability | Needed for | Current status | Current evidence or limitation | Hardware result | Next decisive test |
| --- | --- | --- | --- | --- | --- |
| `MTLDevice` creation | Any Metal use | `SOURCE PATH` | TGL bundle/profile names and load diagnostics exist; object validity is not proven. | **UNVERIFIED** | Enumerate devices and properties; correlate the returned registry ID with the physical IGPU. |
| Truthful GPU family/feature exposure | API correctness | `UNKNOWN` | TGL identity/topology are compatibility values and may not describe Xe-LPG. | **UNVERIFIED** | Record all capability queries, then keep each bit disabled until its conformance test passes. |
| Command queue creation | Submission | `SOURCE PATH` | IOAccelerator and TGL accelerator paths are patched; no queue object/completion trace is retained. | **UNVERIFIED** | Create one queue, submit an empty command buffer, correlate kernel submission, IRQ, and completion. |
| Command-buffer ordering and callbacks | Metal semantics | `BLOCKED` | Stamp/wait forced-success paths can hide missing execution. | **UNVERIFIED** | Submit ordered writes and verify callback order with forced-success diagnostics disabled. |
| Buffer allocation and storage modes | Resources | `UNKNOWN` | Apple TGL allocation paths are inherited; cache/coherency behavior is unverified. | **UNVERIFIED** | Fill shared/private buffers with sentinels, execute a GPU copy, synchronize, and compare all bytes. |
| GPU virtual addressing / residency | Resources and execution | `SOURCE PATH` | GGTT/aperture/context patches exist without a target PTE/residency audit. | **UNVERIFIED** | Trace VA-to-PTE mappings and controlled fault/unmap behavior for one buffer. |
| Texture allocation and layouts | Rendering/compute | `UNKNOWN` | No layout, tiling, compression, pitch, or plane correctness result exists. | **UNVERIFIED** | Round-trip uncompressed 2D textures through blit and compare per-pixel bytes. |
| Resource heaps and aliasing | Advanced resources | `UNKNOWN` | No heap residency or aliasing evidence. | **UNVERIFIED** | Allocate aliased heap resources and verify hazard/lifetime behavior. |
| Blit encoder | First execution primitive | `BLOCKED` | No deterministic GPU copy result excludes CPU fallback. | **UNVERIFIED** | GPU copy patterned buffers; prove engine progress, fence completion, and byte equality. |
| Compute pipeline creation | Compute | `UNKNOWN` | No retained compiler or pipeline-state result. | **UNVERIFIED** | Compile a minimal add kernel and retain compiler diagnostics/pipeline metadata. |
| Compute dispatch | Compute | `BLOCKED` | Depends on VM, submission, shader ABI, and synchronization. | **UNVERIFIED** | Vector add with randomized inputs, guard regions, GPU counters, and exact output comparison. |
| Render pipeline creation | Rendering | `UNKNOWN` | No vertex/fragment pipeline evidence. | **UNVERIFIED** | Build a minimal pass-through vertex and solid-color fragment pipeline. |
| Rasterization/render target | Rendering | `BLOCKED` | A visible framebuffer is not render-engine evidence. | **UNVERIFIED** | Off-screen triangle, read back, compare coverage/color against a reference image. |
| Vertex/fragment interpolation | Rendering correctness | `BLOCKED` | No stage/ABI or coordinate-convention validation. | **UNVERIFIED** | Vertex-color triangle with pixel tolerances and edge-rule checks. |
| Texture sampling/samplers | Rendering correctness | `BLOCKED` | Format, coordinate, filter, and binding semantics unknown. | **UNVERIFIED** | Nearest/linear sampling over a diagnostic texture with edge/address-mode cases. |
| Blending and color write masks | Rendering correctness | `BLOCKED` | No fixed-function mapping evidence. | **UNVERIFIED** | Layer known colors and compare exact/allowed-tolerance output per format. |
| Depth/stencil | Rendering correctness | `BLOCKED` | No depth format, compare, or load/store result. | **UNVERIFIED** | Overlapping geometry with all compare modes and stencil transitions. |
| MSAA and resolves | Rendering correctness | `BLOCKED` | Sample counts and resolve layout unknown. | **UNVERIFIED** | Render edges at each exposed sample count and validate resolve output. |
| Pixel/texture formats | Resource correctness | `UNKNOWN` | No validated compatibility list; capability spoofing is insufficient. | **UNVERIFIED** | Table-driven upload/sample/render/read-back per exposed format. |
| Argument buffers/resource binding | Shader ABI | `UNKNOWN` | Binding-table and metadata compatibility have not been established. | **UNVERIFIED** | Bind multiple buffers/textures/samplers through each exposed argument-buffer tier. |
| Threadgroup memory and barriers | Compute semantics | `BLOCKED` | SIMD width, SLM layout, and barrier encoding are unknown. | **UNVERIFIED** | Multi-threadgroup reduction with adversarial scheduling and repeated result checks. |
| Atomics | Compute semantics | `BLOCKED` | Atomic width/scope/order mapping is unverified. | **UNVERIFIED** | Contended atomic counters for every exposed type/scope with exact totals. |
| Fences, events, and hazards | Synchronization | `SOURCE PATH` | Existing waits/forced stamps may mask failures; Metal semantics are unproven. | **UNVERIFIED** | Cross-encoder producer/consumer tests with timeout and negative ordering cases. |
| Indirect commands/draws | Advanced execution | `UNKNOWN` | No command encoding compatibility evidence. | **UNVERIFIED** | Enable only after direct paths pass; compare direct and indirect output. |
| Tessellation | Optional rendering | `UNKNOWN` | No compiler/fixed-function evidence. | **UNVERIFIED** | Keep unexposed until a tessellation conformance sample passes. |
| Sparse resources | Optional resources | `UNKNOWN` | Residency/page-fault semantics are unverified. | **UNVERIFIED** | Keep unexposed until map/unmap/fault tests pass. |
| Memoryless textures | Tile/render semantics | `UNKNOWN` | Hardware and Apple-driver mapping are unknown. | **UNVERIFIED** | Keep unexposed until load/store and lifetime tests pass. |
| Shader stages and MSL types | Compiler correctness | `NOT IMPLEMENTED` / inherited path unknown | Mellow has no compiler backend; compatibility with Apple TGL output is unproven. | **UNVERIFIED** | Capture diagnostics and validate scalar/vector/matrix types stage by stage. |
| Shader ISA and metadata | Actual GPU execution | `NOT IMPLEMENTED` / inherited path unknown | No proof that TGL-emitted binaries and metadata execute correctly on Xe-LPG. | **UNVERIFIED** | Inspect a minimal compiled artifact where lawful, then correlate GPU execution and output. |
| Pipeline cache/serialization | Stability/performance | `UNKNOWN` | No cold/warm cache or invalidation result. | **UNVERIFIED** | Repeat pipeline creation across process restart and OS reboot with version checks. |
| IOSurface/display presentation | Compositing | `SOURCE PATH` | Framebuffer and CoreDisplay workarounds exist; hardware-backed presentation is unproven. | **UNVERIFIED** | Present a known surface, capture scanout/output, and correlate render/display engine activity. |
| WindowServer acceleration | System integration | `BLOCKED` | Desktop visibility and crash avoidance do not exclude software rendering. | **UNVERIFIED** | Correlate WindowServer Metal device, GPU counters, frame timing, and CPU fallback checks. |
| Fault reporting and recovery | Robustness | `UNKNOWN` | No defined Metal error propagation or reset recovery result. | **UNVERIFIED** | Inject a bounded invalid command/resource reference on a disposable test setup and verify recovery. |
| Sleep/wake and long-duration load | Stability | `BLOCKED` | Functional milestones have not passed. | **UNVERIFIED** | Only after correctness: repeated sleep/wake and one-hour mixed load with fault monitoring. |

## Promotion rule

A row becomes `VERIFIED` only when its experiment records inputs, expected and
actual outputs, physical GPU execution evidence, CPU-fallback exclusion, exact
software versions, and reproducible commands. Any capability that fails must be
disabled or reported unsupported; it must not remain exposed behind a forced
feature flag.
