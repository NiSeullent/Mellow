# Platform implementation status

This file distinguishes the platform foundation from the target architecture and the legacy kext.

## Implemented in the platform foundation

- `Runtime/PlatformRuntime.hpp` and `.cpp`: freestanding C++17 provider/route policy,
  resource transition admission, reset-generation completion checks and JIT cache identity.
  These are host-testable policy components; they do not call OpenGL, OpenCL or a GPU.
- `Tools/mellow-port.py` and `Tools/mellow_port/`: explicit Linux source intake, hashes,
  licensing facts, conservative inventory, gap reports and limited generated source/build inputs.
  Successful artifact generation does not assert a compiled or functional driver.
- `Tools/run-platform-tests.py`, `tests/platform_runtime_test.cpp` and
  `tests/test_mellow_port.py`: software tests with synthetic providers and source fixtures.
- `Tools/probe-opencl-substrate.py`: bounded discovery and opt-in OpenCL GPU compute through an
  installed host driver. This standalone probe bypasses MellowRT/JIT/Metal, records readback and
  event profiling, and does not infer physical PCI identity from a friendly device name.

## Recorded verification

[platform-foundation.json](../validation/platform-foundation.json) records C++ policy checks
with ASan/UBSan and Python source-intake tests. Two separately fetched, pinned upstream inputs
identified 17 Xe register constants and 3 amdgpu constants. Generation emitted the 17 Xe constants;
the amdgpu file's missing SPDX expression caused source-derived generation to be withheld.
The remaining semantic contracts stay unimplemented and both plans explicitly report `driver_ready=false`.

[opencl-windows-substrate.json](../validation/opencl-windows-substrate.json) records an actual
Windows execution using Intel OpenCL Graphics / NEO, driver 32.0.101.6737. Three nonce-varied
256-element `x * 7 + 3` submissions passed readback and nonzero ordered GPU event timestamps.
The result is `PASS_OPENCL_GPU_SUBSTRATE_ONLY`: Mellow, Metal, macOS and physical PCI attribution
are not validated by that result. The probe tool hash is recorded in the report.

## Not implemented by this change

`libMellowMTL`, a Mellow-owned Objective-C `MTLDevice`, AIR/metallib lowering, MSL JIT,
actual GL/CL providers, MellowGL/MellowCL, XNU LinuxKPI, Mesa winsys adaptation,
NVIDIA/AMD driver binding, system Metal registration and WindowServer integration remain work.
No placeholder backend is registered to make those features appear present.

The native Xe modules under `Mellow/` retain their previous experimental scope. The new portable
runtime is not wired into that kext or the Galaxy Book EFI. Existing native test reports refer to
their recorded snapshots, not to the new platform or a physical acceleration result.

## Verification commands

Run `python3 Tools/run-platform-tests.py --out build/platform-tests` with a C++17 compiler,
and `python3 -m unittest discover -s tests -p test_mellow_port.py` for the source intake tool.
The source-intake host needs Python 3.9 or newer; portable policy tests also need a working C++17
compiler. A macOS SDK is not required for these
portable tests. The native kext continues to use its separate Xcode workflow.

Run `python Tools/probe-opencl-substrate.py --report build/opencl-discovery.json` to discover an
installed OpenCL GPU runtime. Add `--compute` to run the small fixed kernel under a 30-second worker
deadline. This command does not install or modify a driver and does not fall back to a CPU device.

Read [PLATFORM-ARCHITECTURE](PLATFORM-ARCHITECTURE.md) for the implementation contracts and
[PLATFORM-DECISIONS](PLATFORM-DECISIONS.md) for reviewed assumptions and primary sources.
No Metal family, GPU, or operating-system support claim is granted by host tests.
