# Native GPU user/kernel boundary

The source now contains a Mellow-owned IOKit evidence interface and a concrete
binding to the existing Xe submission, GPU fence and DMA readback code. The four
new kernel translation units are included in the Mellow Xcode target. They do
not publish a working accelerator: the retained native PCI/firmware/context
owner required to instantiate them is still missing. Metal registration,
WindowServer acceleration and physical GPU execution remain unverified.

## Implemented path

| Source | Operation |
|---|---|
| `Mellow/NativeGpuABI.h` | Versioned 64-byte requests and 1,152-byte replies, with explicit service/physical identity, reset generation, job, correlation and bounded output. |
| `Mellow/NativeGpuSession.*` | One owned evidence job per connection; real driver callbacks are required for submission, completion, readback and retirement. |
| `Mellow/NativeGpuIOKit.*` | Abstract PCI-backed IOService and administrator IOUserClient; fixed structure sizes, privileged synchronous calls, exclusive connection and retained uncertain work. |
| `Mellow/XeNativeEvidence.*` | Calls `XeContext::EvidenceExecution`, verifies the actual `XeFence::Timeline` observation, retains VM uses through readback, synchronizes the IOKit DMA pin and copies actual output bytes. |
| `Runtime/NativeGpuIOKit.*` | Actual `IOServiceOpen`/`IOConnectCallStructMethod`, exact service registry selection and validated bounded replies. Non-Darwin returns unavailable. |
| `Mellow/XeGgttIOKit.*` | Physical 8086:7D41 main12.70/media13.00 BAR0 PTE access, MCR-aware PAT3, retained backing validation and bounded main/media MMIO TLB invalidation during stopped-GuC bootstrap. |

The evidence ABI has Query, SubmitEvidence, PollEvidence, ReadEvidence and
CloseEvidence selectors. It runs the kernel owner's fixed reviewed program;
applications cannot supply executable bytes, GPU addresses or readiness flags.
This subset is not the full DRM-shaped GEM/VM/EXEC/SYNCOBJ/IOSurface UAPI.
The current userspace transport admits only physical Intel 8086:7D41/GMD12.70.
NVIDIA and other Intel families need their own real submission owners before
admission can be expanded.

## Ownership and completion

The native service derives from an abstract base with no default matching or
registration. Its concrete owner must retain all PCI, MMIO, firmware, GGTT,
PPGTT, engine, IRQ and fence objects and serialize their operations with the
same sleepable lock used by this interface. It supplies operations for the
newly minted client owner; obtaining those operations must not acquire new
per-client GPU resources before the session can close them.

Each connection accepts one submission attempt and strictly increasing request
correlations. Acceptance yields pending state, never readable output. Completion
requires the original generation/job/nonce/count, acquire-ordered GPU fence1,
and a monotonic observation before the submission deadline. First polling after
the deadline and completion sampled after that deadline both fail. Elapsed time
does not cancel hardware work. Readback is allowed only after verified completion;
partial failed reads are erased before the reply.

Quarantine is terminal for new work even if a later clock/identity query succeeds.
Cleanup remains available through the original owner. The underlying Xe execution
also preserves a failed/timeout result when a late fence arrives; it cannot
release scheduler uses by converting that job into successful content.

The Xe binding holds the six actual VM objects independently of scheduler-use
references, so GPU completion cannot free output before CPU readback. It reads
the output pin after device-to-CPU synchronization and identity revalidation.
No expected-value computation exists in the kernel output path.

Before any ring publication, the native binding validates the stopped context,
initial fence0 and held input/output pins, writes fixed uint32 input
`(i * 2654435761U) ^ 0xa5a55a5aU`, initializes output to byte sentinel `0xcd`
and performs actual device DMA synchronization. It rechecks identity and samples
time again afterward. The heap-staging backend must preserve those data bytes.
Retiring output remains readable while this exact binding's independent use is
held; a retire request cannot free that backing before real close.

Disconnect attempts real close. A failed close retains the service, client and
owned resources; the concrete driver may retry after actual stop/reset, but the
close callback must still prove retirement. Provider teardown, a timeout, a
changed epoch or destruction of a C++ wrapper cannot release GPU-live memory.

## GGTT bootstrap restriction

The IOKit GGTT adapter requires exclusive epoch/range/backing authority from a
retained PCI owner. It verifies physical PCI identity, D0, bus mastering, both
graphics/media IPs, forcewake, actual pins and the translated BAR0 layout.
Zero PTEs do not establish range ownership. A nonzero write must refer to one of
the exact held DMA pages; foreign live entries cannot be overwritten or cleared.

PTE, PAT and raw TLB writes require both GuCs actually held in reset and a
bootstrap lease that excludes all other consumers. The domain remains held
across both GTs, posted reads and completion waits. Main PAT3 uses the MCR
semaphore/selector; media PAT3 uses its non-MCR register. Uncertain writes,
lost wake ownership or failed flushes quarantine the ranges and pins. After
firmware starts, mapping/PAT read validation and backing retention remain
available; mutation requires a completed reset or a future GuC-action backend.

Register/layout evidence is pinned to
[Linux Xe 0d9ff90a](https://github.com/torvalds/linux/tree/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/xe).
The bounded bit-clear waits additionally follow
[Fei Yang's April 18, 2026 Intel Xe patch](https://lore.kernel.org/intel-xe/20260418000349.1398567-1-fei.yang@intel.com/).
The source does not claim that patch is merged or that these operations have
passed a physical Darwin run.

## Verification and remaining integration

The [source-bound verification checkpoint](../validation/native-gpu/source-checkpoint.json)
records actual source hashes, object hashes, bounded host test results and the
unverified full-driver fields. Each synthetic test keeps GPU execution false.

The portable session test passes with strict Clang warnings, AddressSanitizer
and UndefinedBehaviorSanitizer. Its callbacks are explicitly synthetic. It
covers correlation replay, pending read rejection, every mismatched completion
field, failed partial readback, first-poll and crossing-deadline timeouts, reset
loss, clock regression, quarantine before submission, unknown submission and
retained failed retirement. The Xe context execution regression additionally
passes 483 checks, including ordinary success, deadline-bound first observation,
late fences after timeout, attempted recovery after a terminal failure and
deadline crossings during heap staging, context synchronization and GuC notify.
The required real owner clock is sampled again before tail/register publication
and after notify; a blocking copy cannot reuse the earlier timestamp as proof.
Its result explicitly records `gpu_execution=false`.

The new kernel sources compile to x86_64 Mach-O objects with the local
MacKernelSDK and Clang, separately from a full kext link. The userspace transport's
non-Darwin branch compiles locally. An Apple userspace SDK and actual macOS
execution are not available in this workspace, so the Darwin userspace branch,
native Metal adapters and IOSurface presenter still need actual builds/runs.

The next native-owner integration must construct real GGTT leases/pins, bootstrap
exact firmware, construct full ADS/engine state, publish an owned LRC/PPGTT,
route interrupts and hold reset authority. Full ADS needs separate preload
initialization and post-load captured golden-LRC publication; a zero bootstrap
LRC is not a captured golden context. Only that owner can instantiate and
register the new service. System Metal/compiler/resource integration and
WindowServer/scanout remain required for the full requested port; these bounded
IPC and kernel object checks do not establish either.

Public IOKit IPC contracts:
[XNU IOUserClient](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOUserClient.h),
[IOKitUser IOKitLib](https://github.com/apple-oss-distributions/IOKitUser/blob/main/IOKitLib.h).
