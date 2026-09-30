# Explicit Apple Metal compute adapter

The separate CGL render selector adapter is documented in [RenderREADME.md](RenderREADME.md).

`MellowCreateDevice(openCLGPUIndex, &error)` creates an application-selected,
Objective-C selector adapter over the production `MellowMTL` compute objects.
Initialization selects an installed **GPU** OpenCL device and executes the
provider's verified GPU bootstrap. It returns `nil` with `NSError` if that
provider is unavailable or initialization fails. Error user info includes typed
initialization status and whether GPU bootstrap submission was attempted.
It does not select a CPU fallback or install a driver.

This is a bounded compute subset. The objects are typed as Apple Metal objects
at the entry point so the supported Metal selectors can be called from ordinary
Objective-C++ code. They do **not** claim complete protocol conformance:
`conformsToProtocol:@protocol(MTLDevice)` is false, unimplemented selectors raise
`MellowAppleMetalUnsupportedException`, and no Apple GPU family is advertised.
The factory does not register with `MTLCreateSystemDefaultDevice`,
`MTLCopyAllDevices`, Metal.framework, IOAccelerator, or WindowServer. An
unsupported Intel or NVIDIA GPU still needs a working native kernel/firmware
and userspace GPU provider before this adapter can execute on it.

## Implemented selectors and boundaries

Every object supports `label`, and resource objects return their actual adapter
`device`. Creation uses the production C++ object constructors and GPU compiler;
command execution uses the production OpenCL queue and completion/readback path.

| Object | Implemented behavior |
| --- | --- |
| Device | `name`, `maxBufferLength`, `newCommandQueue`, `newBufferWithLength:options:`, `newBufferWithBytes:length:options:`, `newLibraryWithSource:options:error:`, `newComputePipelineStateWithFunction:error:`. `supportsFamily:` and `supportsFeatureSet:` return `NO`. |
| Buffer | `length`, stable `contents`, shared/default cache `storageMode`, `cpuCacheMode`, `resourceOptions`. Only 4–16384 bytes in `uint32_t` multiples and exactly default shared options. |
| Library | `functionNames`, `newFunctionWithName:`. Source is validated synchronously through the real production MSL parser, not accepted as an unchecked string. Only one kernel entry; compilation options must be `nil`. Function specialization returns `nil` with an unsupported error. |
| Function | `name`, kernel `functionType`. |
| Compute pipeline | The actual GPU pipeline is compiled when created. `maxTotalThreadsPerThreadgroup` is the adapter's admitted limit of one. `threadExecutionWidth` raises unsupported because the provider does not expose physical Metal SIMD width. |
| Queue | `commandBuffer`. Each command retains its originating queue/device. |
| Command buffer | `commandQueue`, `device`, `status`, `error`, `retainedReferences`, `computeCommandEncoder`, serial `computeCommandEncoderWithDispatchType:`, `addCompletedHandler:`, `commit`, `waitUntilCompleted`. Render and blit requests return `nil` and make the command fail. |
| Compute encoder | `setComputePipelineState:`, `setBuffer:offset:atIndex:`, `dispatchThreads:threadsPerThreadgroup:`, `endEncoding`. Only buffer index zero, offset zero, exact 1D grid matching buffer elements, and `(1,1,1)` threadgroups. |

The production shader subset accepts a writable `device uint *` at `buffer(0)`
and scalar `uint [[thread_position_in_grid]]`, bounded source/expression syntax,
and at most 64 dispatches per command. It does not support textures, arbitrary
AIR/metallib loading through Apple selectors, atomics, barriers, threadgroup
memory, indirect dispatch, other encoders, render/compute sharing, or function
constants. `RenderObjects` owns a separate OpenGL device; it is not silently
substituted for this compute device.

Errors on `NSError` creation selectors return `nil` and populate the error.
Invalid binding/dispatch, unsupported render/blit, and an abandoned encoder
produce an error command and invoke registered completion handlers without GPU
submission. Unsupported selectors and APIs without an error return raise the
named exception. Do not catch an unsupported encoding operation and then treat
the command as successful; check the completed command's `status` and `error`.

## Ownership and asynchronous completion

Encoding/commit calls for a command are single-threaded, matching the underlying
C++ object contract. Status queries, completion registration before commit, and
waiting are synchronized. A device worker serializes all of that adapter
device's command queues. `commit` enqueues real driver work and returns without
waiting for GPU completion. Completion handlers run after driver completion and
copied readback; `waitUntilCompleted` also waits until all handlers return.

The committed worker retains the command and Objective-C staging buffers; the
production command retains each C++ pipeline and buffer. The caller may release
the library, function, pipeline, encoder, and queue after encoding. The worker
releases copied callback blocks after invocation. A callback that captures the
command itself does not remain in the command's stored handler list after
commit. Callback exceptions are caught so remaining handlers and waiters still
complete.

`contents` is fixed CPU staging memory, not native GPU shared allocation.
Before execution, its words are uploaded to the production buffer; completed
readback updates the same CPU address. The application must wait before reading
or changing contents for work in flight, and must exclude concurrent CPU writes.
This adapter cannot police writes through a raw pointer. Waiting for unfinished
work from any completion handler on the same device worker raises unsupported,
preventing a serial-worker deadlock. Repeated commit, late completion-handler
registration, and encoding after commit are rejected.

`MellowCopyAdapterCapabilities` records the actual provider strings, driver
reported IDs, and bootstrap verification facts. Driver-reported IDs are not
verified physical PCI identity. `MellowCopyCommandExecutionEvidence` returns
actual OpenCL execution events after driver completion, including profiling,
event ownership, and resource cleanup. Those events do not by themselves prove
arithmetic correctness; the acceptance client separately verifies all output.
Scheduled callbacks and Metal GPU timestamps are unsupported because the
underlying objects do not expose those Apple scheduling/timing contracts.

## Native acceptance

`compute-acceptance.mm` calls this factory directly; it never calls Apple's
default Metal-device factory. It compiles a uint32 affine kernel on the actual
GPU, encodes two dispatches, releases external library/function/pipeline/queue
references, verifies asynchronous callback/readback ordering, and compares every
result against an independent CPU calculation. Negative cases cover invalid
binding, offset, dispatch size, threadgroup geometry, unsupported encoders,
abandoned/active encoders, post-commit operations, and same-worker waits.

It emits JSON to stdout:

- `PASSED`, exit 0: all independent readback, event, lifetime, and negative
  checks passed for the selected installed OpenCL GPU.
- `NOT_RUN`, exit 77: no usable GPU provider is available before any bootstrap
  submission attempt, or the acceptance host is outside macOS 15/26 x86_64.
  This is not GPU acceptance.
- `FAILED`, exit 1: compilation/execution or an acceptance check failed.
  A submitted or attempted GPU bootstrap failure is recorded as `FAILED`.

Every receipt states `systemMetalRegistered=false`,
`windowServerIntegrated=false`, `fullMetalProtocolConformance=false`, and
`physicalPCIIdentityVerified=false`. A successful run is evidence for this
opt-in subset and the recorded provider, not every GPU model or native system
Metal operation.

Use `Tools/build-apple-userspace.py` for the coordinated native build and
packaging. With an installed macOS userspace SDK, a direct equivalent compute
build from the repository root is:

```sh
xcrun --sdk macosx clang++ -std=c++17 -arch x86_64 -mmacosx-version-min=15.0 \
  -fobjc-arc -fblocks -I Runtime -I Userspace/AppleMetal \
  Userspace/AppleMetal/MellowAppleMetal.mm \
  Userspace/AppleMetal/compute-acceptance.mm \
  Runtime/PlatformRuntime.cpp Runtime/OpenCLProvider.cpp Runtime/ShaderJit.cpp \
  Runtime/AirDecoder.cpp Runtime/MetalObjects.cpp \
  -framework Foundation -framework Metal -o /tmp/mellow-compute-acceptance
/tmp/mellow-compute-acceptance 0
```

These are build/run instructions, not a report that they were executed. Testing
both macOS 15 and macOS 26 requires their installed SDKs and runtime hosts. A
kernel-only SDK cannot compile the Foundation/Metal adapter. A native acceptance
process needs an external deadline because a vendor driver may block inside a
synchronous OpenCL call; the adapter does not invent a GPU timeout/reset owner.

## Apple API references

- [Source library creation](https://developer.apple.com/documentation/metal/mtldevice/makelibrary%28source%3Aoptions%3A%29?language=objc): source creation completes before returning a library/error.
- [Command buffer](https://developer.apple.com/documentation/metal/mtlcommandbuffer): status, commit, and wait lifecycle.
- [Completed handlers](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addcompletedhandler%28_%3A%29?language=objc): register before commit; invoke after GPU execution completes.
- [Threadgroup and grid sizes](https://developer.apple.com/documentation/metal/calculating-threadgroup-and-grid-sizes): execution width describes physical pipeline SIMD geometry and is not inferred from an accepted group size.
- [Resource objects](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Mem-Obj/Mem-Obj.html): native storage/cache/resource semantics. The staging boundary above is explicit.
