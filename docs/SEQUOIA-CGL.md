# macOS CGL provider

The bounded Mellow render-object API can now request an owned, offscreen OpenGL 4.1 core context through Apple's public CGL API. A dedicated worker owns the context, pipeline compilation, FBO, RGBA8 texture, VAO, GPU fence and readback. The existing WGL path is preserved. Linux still returns explicitly unsupported rather than pretending to render.

The pixel-format request requires accelerated rendering and no software recovery. The chosen virtual screen is queried again, the actual GL version/core profile and identity strings are checked, and known software renderer strings are rejected. These are driver-reported checks, not physical PCI attestation. A missing accelerated provider causes failure. The implementation does not supply a hardware driver for Intel 8086:7D41.

On macOS the bounded MSL frontend emits GLSL 410 core; on Windows it keeps GLSL 330 core. Existing Metal-like top-left coordinates, fragment-position conversion, resource ownership, sequence/epoch correlation and independent readback checks are retained. Only the existing small vertex/fragment language subset is supported. CGL visible presentation, IOSurface sharing, WindowServer integration and Apple Objective-C Metal ABI registration are not implemented. The public provider rejects `--visible` on macOS.

```sh
python3 Tools/run-255u-tests.py --cxx clang++ --out build/cgl-boundaries
python3 Tools/run-render-objects.py --cxx clang++ --out build/cgl-compile
# Only on a Mac with an already accelerated OpenGL driver:
python3 Tools/run-render-objects.py --cxx clang++ --out build/cgl-render --render --frames 16
```

The runner stores the native stream and an independent Python pixel comparison. `native_macos_execution` means this public CGL program ran on macOS; it does not mean native Xe submission, a 7D41 GPU, or the Apple Metal ABI was verified. `apple_metal_abi_registered`, `windowserver_acceleration_verified` and `display_scanout_verified` remain false. The cloud CI attempt is recorded separately from compilation and can legitimately report that an accelerated context is unavailable.

Primary API references:
- https://developer.apple.com/documentation/opengl
- https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_contexts/opengl_contexts.html
- https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf

OpenGL's deprecation on macOS is a compatibility constraint, not evidence of an available 7D41 driver. This backend closes one host-provider gap, not the full GPU port.
