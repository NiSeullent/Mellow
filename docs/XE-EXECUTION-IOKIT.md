# IOKit staging for the Xe evidence execution

`Mellow/XeExecutionIOKit.hpp/.cpp` supplies the `stageHeaps` part of an existing
`XeContext::ExecutionBackend` using the production `XeMemory::VirtualMemory`
and `XeMemoryIOKit` allocation/DMA adapters. It does not construct a native GPU
owner. There is no default admission, simulated production fence or automatic
boot/submission hook.

Construct `IOKitExecutionStaging(vm, iokitContext, physicalBackend)` off-stack,
then pass `staging.backend()` to `EvidenceExecution`. All of the existing
physical backend callbacks except `stageHeaps` are mandatory and are delegated
to that exact backend and its original opaque context. Its old `stageHeaps` is
replaced. A missing admission, fresh-context, context retain/release,
direct-coherent context barrier or quiescence callback returns an empty backend.
The physical backend must remain an actual trusted kernel device owner; a
caller predicate or IOUserClient flag cannot satisfy that contract.

The owner must construct the adapter with its own canonical PPGTT VM and the
IOKit context for that same physical device's retained mapper. The current
generic `ExecutionBackend::admitted(context, policy)` ABI does not expose a VM
or mapper identity query. The adapter therefore cannot independently connect
an arbitrary constructor VM to the physical owner's PPGTT/device: the actual
owner must establish that integration, along with its admitted context root.
Exact pin resolution verifies the supplied canonical IOKit context; it does not
turn a pin from a different physical device into valid context admission.

The VM, mapper, IOKit context, adapter, physical backend and GuC/fence objects
must remain alive through successful `EvidenceExecution::close()`. Use their
existing shared sleepable ownership/reset lock for every operation and backend
callback. The adapter does not acquire a separate lock, run in interrupt context
or permit callback reentry. Its bounded DMA-page scratch occupies about 256 KiB,
so it must not be placed on a kernel stack. It owns no pins or VM uses; the
execution scheduler retains those resources. A failed delegated context release
keeps the adapter's exact context hold available for the existing close retry.

## What staging does

The six handles remain ordered as instruction, indirect, surface, batch, input,
output. Before writing any heap, the adapter requires all six to resolve to the
exact owner and allocation generation, with `Bound` state and exactly one VM
use. Requiring one use avoids overwriting buffers held by another job. The
first four allocations must each be 4096 bytes at the addresses in `Prepared`.
Input and output must cover the requested element count; each is limited to
64 MiB, matching the production IOKit pin adapter's default allocation limit.
The complete input/output extents are checked even when the kernel touches a
smaller prefix.

Before changing its report or scratch, staging rejects `Prepared` and handle
storage overlapping the entire staging object. It also verifies that the VM,
IOKit context, returned allocation records and DMA arrays lie outside that
object. Allocation storage is checked immediately after the VM's own
owner/generation lookup and before this adapter reads the record; DMA storage
is checked before any page-array dereference or scratch mutation. This protects
immutable kernel inputs against deliberately malformed aliases. Early storage
rejection leaves `report()` unchanged, so the callback's boolean result remains
the submission decision.

For every handle, `resolvePinnedBuffer` verifies the private IOKit resource's
actual owner, IOKit context, full descriptor length, retained mapper, prepared
descriptor/DMA command association, page count and cleanup state. Its cookie is
a trusted private kernel handle, never an arbitrary client pointer. Staging
rejects overlapping GPU VA or CPU extents, repeated pins/handles, overlapping
DMA metadata arrays, CPU overlap with any allocation/DMA metadata or staging
object, and repeated DMA pages throughout all six complete allocations. DMA
pages must be aligned and within the existing 46-bit address limit. A bounded
in-place heapsort of copied page values checks aliases in O(n log n) time without
allocating at submission. It does not mutate the retained DMA arrays.

The input and output surface descriptors are compared to freshly encoded
descriptors for those exact allocation addresses, complete byte lengths and
the policy argument that the physical owner admitted. This policy is only an
argument snapshot: admission is called again, rather than cached as a fact.
The physical owner also must still report a fresh stopped context before the
copy and after synchronization.

The adapter copies **four complete 4096-byte heaps**, including their zero
padding, from immutable `Prepared`. It verifies the resulting CPU bytes through
volatile readback. `Prepared` contains no input/output initial values, so these
two buffers preserve the caller's existing contents. The caller must initialize
them through its own held-allocation memory path before submission. The adapter
then calls the production `synchronizeForDevice` on all six pins. This performs
the actual `IODMACommand::synchronize(kIODirectionOut)` operation, including any
required DMA bounce copy. It is neither GPU cache flush nor job completion.

`report()` records completed CPU copies and successful DMA synchronization
calls. `Staged` means four CPU heaps were copied and all six DMA calls returned
success while physical admission/freshness still held. It does not mean that a
context was registered, a GPU ran, output was correct, or a GPU fence completed.
On a DMA failure the report preserves the actual partial synchronization count;
CPU copies can already have occurred. There is no rollback or fabricated
completion. The execution's existing error and close paths keep its resources
until the physical owner supplies authoritative quiescence and inverses.

## Ring/LRC and native execution remain physical-owner requirements

`LiveContext::ringCpu` and `lrcTailCpu` are bare pointers. They do not identify
the IOKit descriptor, exact backing handle/generation, ring/LRC byte offset,
full extent or held GGTT mapping generation. The current `XeMemoryIOKit` pin
adapter also permits DMA bounce backing. A successful heap DMA synchronization
therefore cannot establish the `synchronizeContext` contract's direct-coherent
ring/LRC visibility. This adapter delegates that callback unchanged to the
mandatory physical owner and does not treat the pointers as proof.

A concrete native owner still needs to retain exact ring/LRC backing allocation
handles and their prepared descriptors, verify complete byte offsets/extents and
pointer identity, hold the exact GGTT generations with fresh PTE/TLB evidence,
and establish that the DMA view is the original coherent descriptor backing.
That last check requires actual descriptor/DMA mapping metadata and page-level
mapping identity, not a new caller-provided `coherent=true` flag. It must also
establish the existing backend's actual primed LRC, PPGTT root, PAT/MOCS,
topology/workarounds/preemption, device mapper/reset epoch, loaded GuC/full ADS,
IRQ transport routing and authoritative completion/quiescence. No currently
missing hardware owner is filled in by this staging helper.

## Verification scope

`tests/xe_execution_iokit_tests.cpp` compiles the real staging, VM, IOKit pin/DMA,
dispatch, context execution, GuC transport and fence sources against a test-only
OS boundary. The test fixture assigns distinct synthetic mapper addresses
before simulated GPU binding because the shared OS shim otherwise emits the
same addresses for each descriptor. That adjustment is explicitly simulation.

Defensive regressions also deliberately place handles in the report, `Prepared`
in private scratch, allocation metadata in that object and a private pin's DMA
array in scratch. These are malformed trusted-kernel inputs, not normal IOKit
paths. The shim redirects the actual simulated private resource pointer only
for the DMA-array case and restores it before canonical cleanup. Each rejection
must preserve the immutable bytes and avoid DMA synchronization/publication.

Tests exercise byte-exact heap copying before DMA synchronization, unchanged
input/output, maximum supported full extents, stale/foreign/unheld or multiply
held allocations, same-slot generation reuse, descriptor/context mismatches,
CPU/source and complete DMA extent aliases, partial DMA failure, delegated
context barrier failure and resource retention/close retry. Integration tests
execute the actual scheduler and transport control-byte path; correlated GuC
replies and GPU writes in that host test are simulated separately from staging.

Direct host ASan/UBSan and vendored kernel-header macOS 15/26 object checks are
recorded in the task evidence directory. Vendored-header cross compilation is
not a genuine macOS SDK/KPI link or a native GPU run. Native GPU execution,
system Metal registration, WindowServer acceleration and display scanout remain
unverified. No driver load or hardware submission is performed by these tests.
