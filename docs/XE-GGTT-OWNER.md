# Bounded GGTT owner component

`Mellow/XeGgtt.cpp` is production, allocation-free kernel-compatible C++ and is included in the actual Mellow Xcode target. It is not itself a complete physical GGTT driver. No live instance is attached to default boot, and no GuC submission capability is enabled by this addition.

## Implemented behavior

The manager accepts at most 32 externally authorized GGTT ranges and 64 live mappings, at most 16,384 pages per mapping. It bounds all addresses to the 32-bit GGTT aperture and uses 4 KiB pages. Reservations validate alignment, overflow, overlap, owner identity, reset epoch and generation freshness. The allocator skips occupied ranges without scanning unbounded address space.

Publication validates and retains immutable device-mapped backing before reading its DMA list. It uses the existing `Mellow::PortedXe::encodeGgtt` for 46-bit system-memory addresses and a separately verified PAT selection. It first checks that every granted slot is zero; zero alone never establishes authority to use a range. It writes each PTE, reads it back, completes an explicit invalidation callback, and verifies the complete mapping again before publishing it.

Consumers retain a precise mapping generation. Retirement requires authoritative quiescence of all consumers, then clears only entries that are still zero or match this manager's expected mapping. It reads zero back and requires invalidation completion before releasing backing. Partial writes, failed readback, unknown mapping contents, uncertain reset state, or failed release retain resources in quarantine rather than returning a success that could cause DMA use-after-free. A quarantine may require a separately proven device reset/space reconstruction; no timer-based automatic recovery is implemented.

## Required trusted adapter — not supplied

The backend must own the physical 8086:7D41 / GMD 12.70 device, establish real D0/reset/forcewake serialization, determine actual GGTT size and firmware/display/stolen/WOPCM exclusions, supply exclusively leased ranges, retain valid IOMMU mappings, prove PAT programming, implement ordered hardware PTE reads/writes and all applicable workarounds, observe actual TLB invalidation completion, and prove consumer retirement. The callbacks deliberately have no default success implementation. Their contracts cannot be satisfied with a boot argument or an unrecognized interrupt.

This branch does not guess a safe GGTT region or install speculative MMIO writes into the user's initial EFI. The component advances the memory-management implementation but does not close the native GPU-owner or Metal ABI gaps.

## Provenance and verification

The encoder calls the already attributed MIT Linux Xe algorithms in `Drivers/PortedXe`, pinned by its `provenance.json` to Linux commit `0d9ff90a5422cc7509258aaaba1e7481df4d332a`. The manager itself is new MIT code. It does not copy a GPL-only driver into an incompatible license.

```sh
python3 Tools/run-xe-ggtt-tests.py
python3 Tools/run-255u-tests.py --cxx clang++ --out build/255u-tests
```

Tests execute the real manager with simulated PCI-memory boundaries, including valid allocation/publication/retirement, stale owner/epoch/generation, overlapping reservations, unauthorized initial entries, malformed backing, write/read/invalidation failures and quarantine. Sanitizers exercise host memory behavior. They do not prove actual device execution, IOKit attachment, firmware authentication or Metal acceleration.
