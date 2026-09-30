# Native macOS userspace build and acceptance

The native sources now have an explicit build entry point at `Tools/build-apple-userspace.py`. It builds the actual compute and render Metal-selector adapters, CGL/IOSurface rendering runtime, app presentation bridge and acceptance clients as x86_64 code. It does not install a driver or register a system Metal device.

Native macOS SDK compilation and physical execution are **NOT_RUN** in the development Linux environment. The new runner was authored and its Python syntax checked without executing the runner. The commands below require separate approval before execution because the existing execution approval did not cover this new script.

## Existing native prerequisites

Use Python 3.9 or newer and an x86_64 Mac running macOS 15 or 26 with an already installed Xcode or Command Line Tools SDK. The runner discovers the active SDK and `clang++` through `xcrun`, checks the SDK version/frameworks, and performs no download or installation. Apple's [Command Line Tools documentation](https://developer.apple.com/documentation/xcode/installing-the-command-line-tools) identifies the bundled SDK/tools; its [build settings reference](https://developer.apple.com/documentation/xcode/build-settings-reference) documents deployment targets and Objective-C ARC.

The default minimum deployment target is macOS 15.0, allowing the same built application subset to be tested on 15 and 26. `--deployment-target 26.0` requests a separate 26.0-minimum build and requires a 26.0-or-newer SDK. Neither deployment option establishes GPU-model coverage or successful execution on the other OS version.

An existing accelerated host OpenCL GPU is required for compute. An existing accelerated CGL 4.1 renderer is required for render, and a logged-in graphical AppKit session is required for window presentation. CPU and software-renderer substitutes are rejected. Before requested execution the runner queries the current process's translation status and ARM capability, records the results and rejects Rosetta/Apple Silicon. Apple's [Rosetta documentation](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment) explains why an x86_64 executable can still be translated. These process checks do not establish a physical PCI device identity.

## Build only

From the source checkout on the approved Mac, use a new or empty sibling directory:

```sh
python3 Tools/build-apple-userspace.py --out ../mellow-native-15 --deployment-target 15.0
```

For a separate minimum-26 build:

```sh
python3 Tools/build-apple-userspace.py --out ../mellow-native-26 --deployment-target 26.0
```

The runner creates object files, `libMellowAppleUserspace.dylib`, `mellow-compute-acceptance`, `mellow-metal-render-acceptance`, `mellow-windowserver-acceptance`, `metal-inventory`, and `apple-userspace-build.json` inside the selected output directory. It refuses output paths that overlap the checkout or contain existing files. It compiles production C++ separately from the Objective-C++ adapters and clients; only `.mm` units receive `-fobjc-arc -fblocks`.

The actual library units are `PlatformRuntime`, `OpenCLProvider`, `ShaderJit`, `AirDecoder`, `MetalObjects`, `OpenGLProvider`, `RenderShaderJit`, `RenderObjects`, `SurfaceSnapshot`, `MellowAppleMetal.mm`, `MellowAppleRenderMetal.mm` and `SurfacePresenter.mm`. Framework linkage includes Foundation, Metal, OpenGL, IOSurface, CoreFoundation, QuartzCore, CoreGraphics, AppKit and IOKit. The inventory executable is linked independently of the Mellow library.

The dylib has an `@rpath` install name and clients use `@loader_path`; keep the acceptance executables beside the built library. The runner records exact commands, diagnostics, source and artifact SHA256 hashes, SDK/compiler identity and source-change checks. It reads each linked Mach-O header/load commands and checks the actual x86_64 architecture, macOS minimum and SDK version, library install name and client dependency. This inspection uses Apple's public [Mach-O declarations](https://github.com/apple-oss-distributions/cctools/blob/main/include/mach-o/loader.h). `BUILT_ONLY` means compilation/linking and metadata checks succeeded and no acceptance executable was run. Missing native tools produce `NOT_RUN` and exit 77, with no substitute binary.

## Explicit physical acceptance

To build and execute all three clients in one approved invocation, use a fresh output directory:

```sh
python3 Tools/build-apple-userspace.py --out ../mellow-native-acceptance --deployment-target 15.0 --run-compute --run-metal-render --run-render --opencl-gpu-index 0 --frames 90 --interval-ms 33 --timeout 60
```

Compute runs two actual affine dispatches through the explicitly selected Mellow Metal-selector adapter and the existing host OpenCL GPU. Its receipt checks independent uint readback, production event ownership/profiling, resource release, retained lifetimes and completion callbacks. It does not replace `MTLCreateSystemDefaultDevice` or claim full Metal protocol conformance.

`--run-metal-render` selects the separate render adapter through Metal selectors and performs two offscreen GPU frames. Compute and render retain separate provider devices. The client checks actual readback against an independent pixel oracle and records GPU fence/resource/IOSurface completion. It creates no window by default.

`--run-render` creates a dedicated application window, renders MSL into a BGRA8 IOSurface through the real CGL provider, waits for the GPU fence, checks every pixel against an independent triangle/gradient oracle, compares IOSurface snapshot pixels with the actual GL readback, and submits immutable images through a dedicated CALayer. GPU rendering and the subsequent CPU snapshot copy are recorded separately. A CALayer transaction receipt establishes application submission; it does not establish physical scanout or installation of a WindowServer GPU driver.

The launcher restricts presentation to 2–600 frames and 1–1000 ms intervals, with scheduled presentation time no greater than 30 seconds. It bounds each acceptance worker externally. Driver calls can still block inside a client until that worker is terminated. A timed-out process has unknown completion and cannot provide success evidence.

For separately approved direct execution of already built clients, the exact argument contracts are:

```sh
../mellow-native-15/mellow-compute-acceptance 0
../mellow-native-15/mellow-metal-render-acceptance
../mellow-native-15/mellow-windowserver-acceptance --present --report ../mellow-native-15/manual-windowserver.json --frames 90 --interval-ms 33
../mellow-native-15/metal-inventory > ../mellow-native-15/metal-inventory.json
```

`metal-inventory` reads actual system Metal devices, registry provider chains, Objective-C instance/class method types, declared ivar offsets and display associations; the build runner never executes it automatically. It records bounds/truncation and never reads private ivar values or invokes private methods. Public Metal enumeration can initialize existing system providers internally; the probe's false driver-load field means it did not explicitly request a driver load. Its output is diagnostic evidence about the existing system rather than a Mellow registration success receipt.

## Evidence limits

The build launcher validates each client's explicit success schema rather than accepting exit zero alone. It keeps the original native JSON receipts and labels successful requested runs `PASSED_LIMITED_APP_SCOPE`. Failure, unavailable GPU/window context, missing reports or changed input sources/binaries cannot become a successful hardware-support result. Client `NOT_RUN` requires explicit evidence that no GPU submission was attempted; unknown completion remains a failure. A missing native environment is `NOT_RUN`; if an earlier requested client already passed, a later unavailable client produces `PARTIAL_UNAVAILABLE` with the earlier result preserved.

All launcher reports retain `system_metal_registered=false`, `system_windowserver_acceleration_verified=false`, `all_gpu_models_verified=false`, and `physical_scanout_verified=false`. Native CGL/OpenCL execution uses an existing host GPU driver. It does not supply native acceleration for a GPU that has no accelerated host provider. Full Apple Metal ABI, system device registration, WindowServer driver adoption, unsupported Intel/NVIDIA hardware bring-up and independent physical display verification require their own implementation and evidence.
