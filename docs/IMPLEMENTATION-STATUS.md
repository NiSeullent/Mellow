# Platform implementation status

The runnable platform now includes an actual host OpenCL provider and a source-derived part of the
Xe memory subsystem. [VERIFICATION-2026-09-06](VERIFICATION-2026-09-06.md) records the evidence
boundaries. Neither the architecture nor a passing algorithm test grants complete GPU/Metal support.

## Implemented and exercised

- `Runtime/PlatformRuntime.hpp/.cpp`: bounded provider/route policy, resource transition admission,
  reset-generation completion checks and cache identity. 108 synthetic policy checks pass with
  ASan/UBSan. Direct OpenCL C is explicit and cannot satisfy the default Metal translation route.
- `Runtime/OpenCLProvider.*`: actual OpenCL context/queue/compiler/buffer/event ownership.
  It validates a bootstrap workload before advertising Compute/OrderedQueue, then uses MellowRT
  planning and completion correlation for real dispatches. The bounded input is OpenCL C 1.2,
  one in-place uint buffer and an independent acceptance reference; MSL/AIR is not accepted.
  Windows Intel driver 32.0.101.6737 reports 8086:7D41 through its advertised Intel query extension.
  Its final Windows stress run verified 10,000 consecutive submissions (2,560,000 uint results),
  matching independent expected/readback stream hashes, on one queue/device/session epoch.
  Forty-five injected lifecycle checks pass under ASan/UBSan; six report-failure tests also pass.
- `Drivers/PortedXe/`: Linux Xe page-entry algorithms (six unchanged function bodies) and adapted
  scatter/gather GGTT mapping/clear loops with pin, write, invalidation and release contracts.
  Addresses/PAT/permissions are validated. Host and actual QEMU Linux guest tests each pass
  18,721 checks; their MMIO/DMA/invalidations are simulated boundaries.
- `Mellow/XeMemory.cpp` and `PortedXeBindings.cpp`: call the ported PTE/PDE encoding inside the
  existing kext source target while preserving its 46-bit, 4K, system-memory and read-only contract.
  Actual cross compilation/linking produces Mellow.kext 0.4.2, a 31-unit x86_64 MH_KEXT_BUNDLE.
  Its 378 unresolved kernel/Lilu imports introduce no new names relative to the previous kext;
  their resolution on an installed Tahoe kernel remains untested.
- `Tools/mellow-port.py` and `Tools/mellow_port/`: source intake, hashes, licensing facts,
  lexical inventory, gap reports and limited integer extraction. Eleven tests pass. The separate
  PortedXe subsystem is a reviewed manual port, not output demonstrating automatic driver conversion.

## What has not been implemented or validated

Mellow-owned Objective-C Metal objects, MSL/AIR/metallib lowering, Metal JIT, a GL rendering provider,
cross-API resource sharing, complete LinuxKPI/DRM and vendor driver ports, NVIDIA/AMD driver binding,
system Metal registration and WindowServer integration remain work.

The new memory algorithms do not create a real Darwin DMA/IOMMU mapping, perform physical GGTT
writes, authenticate GuC firmware or prove actual GPU interrupts/fences. Their test callbacks are
explicitly simulated. The Windows OpenCL provider uses the installed Intel Windows driver; it
does not execute the ported kernel backend. It cannot establish macOS support for 7D41.

No physical Tahoe host was available for this verification. Actual kext loading, installation,
Recovery GUI, Metal compute/render/stress, WindowServer and sleep/wake have not passed.
The separate QEMU Recovery experiment reached boot.efi/kernel collection loading, not a verified
XNU boot or Recovery desktop. QEMU algorithm tests do not close that boot blocker.

## Reproduce the implemented paths

Python 3.9 or newer and a C++17 compiler are required for the portable source/tool tests.

```sh
python3 Tools/run-platform-tests.py --cxx g++ --out build/platform-tests --sanitize
python3 -m unittest discover -s tests -p test_mellow_port.py -v
python3 Tools/run-ported-xe-tests.py --cxx g++ --out build/ported-xe-tests
python3 Tools/run-opencl-runtime-regressions.py --cxx g++ --out build/opencl-regressions --sanitize
```

On Windows with an installed Intel OpenCL driver and MinGW compiler:

```powershell
python Tools/run-opencl-runtime.py --cxx C:/msys64/mingw64/bin/g++.exe --out build/opencl-runtime --compute
python Tools/run-opencl-runtime.py --cxx C:/msys64/mingw64/bin/g++.exe --out build/opencl-stress --compute --iterations 10000 --timeout 180
```

Omit `--compute` for compile-only validation. Actual driver calls are supervised in a separate
process with a deadline. Driver failure, unexpected device identity, event ownership mismatch,
readback mismatch or cleanup failure cannot produce successful runtime completion.

Use [run-ported-xe-emulator.py](../Tools/run-ported-xe-emulator.py) from Linux/WSL with QEMU and
an explicitly supplied Linux kernel. It boots an initramfs and runs the compiled tests as a child
of guest PID1. Success requires the exact guest protocol, nonzero checks, expected scope flags,
process/VM exits and stable input/source hashes. It is not a Xe GPU device model.

## Historical evidence

[platform-foundation.json](../validation/platform-foundation.json) describes the earlier 102-check
policy/intake snapshot. [opencl-windows-substrate.json](../validation/opencl-windows-substrate.json)
is the earlier independent probe, which bypassed MellowRT. Their recorded source hashes and scope
remain historical; use the current verification record for the integrated provider and port.
