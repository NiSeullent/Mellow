# Bounded GGTT owner component

`Mellow/XeGgtt.cpp` is production, allocation-free kernel-compatible C++ and is included in the actual Mellow Xcode target. It is not itself a complete physical GGTT driver. No live instance is attached to default boot, and no GuC submission capability is enabled by this addition.

## Implemented behavior

The manager accepts at most 32 externally authorized GGTT ranges and 64 live mappings, at most 16,384 pages per mapping. It bounds all addresses to the 32-bit GGTT aperture and uses 4 KiB pages. Reservations validate alignment, overflow, overlap, owner identity, reset epoch and generation freshness. The allocator skips occupied ranges without scanning unbounded address space.

Publication validates and retains immutable device-mapped backing before reading its DMA list. It uses the existing `Mellow::PortedXe::encodeGgtt` for 46-bit system-memory addresses and a separately verified PAT selection. It first checks that every granted slot is zero; zero alone never establishes authority to use a range. It writes each PTE, reads it back, completes an explicit invalidation callback, and verifies the complete mapping again before publishing it.

Consumers retain a precise mapping generation. Retirement requires authoritative quiescence of all consumers, then clears only entries that are still zero or match this manager's expected mapping. It reads zero back and requires invalidation completion before releasing backing. Partial writes, failed readback, unknown mapping contents, uncertain reset state, or failed release retain resources in quarantine rather than returning a success that could cause DMA use-after-free. A quarantine may require a separately proven device reset/space reconstruction; no timer-based automatic recovery is implemented.

## Native adapter and required physical owner

`Mellow/XeGgttIOKit.*` now supplies actual BAR0 PTE reads/writes, retained-pin
validation, main MCR/media non-MCR PAT3 programming and main/media TLB
completion waits. Mutations require both GuCs held in reset with exclusive
bootstrap authority. Read/publication checks remain available after startup.
The adapter retains its PCI, authority, wake, ranges and pins on uncertain
operations. Its new kernel source compiles with the local MacKernelSDK; actual
physical execution is unverified. See [native GPU boundary](NATIVE-GPU-BOUNDARY.md)
for its precise limits and pinned source evidence.

The concrete owner must own the physical 8086:7D41 / GMD 12.70 device, establish real D0/reset/forcewake serialization, determine actual GGTT size and firmware/display/stolen/WOPCM exclusions, supply exclusively leased ranges, retain valid IOMMU mappings and prove consumer retirement. It must initialize and retain the adapter with these genuine leases. Those owner callbacks still have no default success implementation. Their contracts cannot be satisfied with a boot argument or an unrecognized interrupt.

This branch does not guess a safe GGTT region or install speculative MMIO writes into the user's initial EFI. The component advances the memory-management implementation but does not close the native GPU-owner or Metal ABI gaps.

## Provenance and verification

The encoder calls the already attributed MIT Linux Xe algorithms in `Drivers/PortedXe`, pinned by its `provenance.json` to Linux commit `0d9ff90a5422cc7509258aaaba1e7481df4d332a`. The manager itself is new MIT code. It does not copy a GPL-only driver into an incompatible license.

```sh
python3 Tools/run-xe-ggtt-tests.py
python3 Tools/run-255u-tests.py --cxx clang++ --out build/255u-tests
```

The regression source exercises the real manager against simulated PCI-memory boundaries. It covers the normal allocation/publication/retirement lifecycle and negative cases for stale owner/epoch/generation, an occupied initial PTE, read/write/readback/invalidation failures, retirement refusal, and resource quarantine/retry. The runner compiles and executes these cases with sanitizers; passing results must be recorded separately from this source description. These checks do not prove actual device execution, IOKit attachment, firmware authentication or Metal acceleration.
