# Explicit Apple Metal render selector adapter

`MellowCreateRenderDevice(&error)` creates a separate application-selected device
over the production `RenderObjects` and accelerated macOS CGL provider. It does
not infer OpenCL sharing or expose a system Metal device. The bounded selectors
support source libraries, vertex/fragment functions, an actual compiled render
pipeline, IOSurface textures, queues, asynchronous command completion, and
render encoders. These objects do not advertise full Apple protocol conformance,
GPU families, IOAccelerator registration, or WindowServer acceleration.

Textures require BGRA8Unorm, 2D, one mip/slice/sample, 1–2048 dimensions, shared
default resource/cache options, render-target-only usage, and identity swizzle.
The supported pass has one color attachment at index zero, mip/slice/depth zero,
`MTLLoadActionClear` and `MTLStoreActionStore`, finite clear components in 0..1,
and no depth/stencil, resolve, query, counter sample buffers, rate map, or custom sample positions.
Nonzero render-target dimensions must match the texture. Pipelines have sample
count one, no blend/depth/stencil/vertex layout, full color write mask, default
topology, no indirect commands, and no vertex amplification.

The production typed MSL subset compiles one `vertex float4` and one
`fragment float4` function. Source creation validates both functions before
returning the library; pipeline creation compiles the GLSL program on the real
GPU. Encoders accept matching finite float4 values at buffer zero in both stages
via `setVertexBytes:length:atIndex:` and `setFragmentBytes:length:atIndex:`. An
independent value in either stage is rejected instead of silently merged.
`drawPrimitives:vertexStart:vertexCount:` accepts only triangle, start zero,
count three, one draw per clearing pass, up to 16 passes per command. The vertex
shader supplies the actual triangle positions; fullscreen coverage is not
inferred. Unsupported selectors raise the named adapter exception and mark an
encoding command as failed. Unsupported compute/blit return nil and fail the
command. A real render error remains an error, not a completed empty command.

Command encoding is single-threaded. The device worker serializes submissions,
retains the command/queue/device and Objective-C texture resources, and keeps
the actual C++ shaders/pipelines/textures alive until GPU completion. Completion
handlers run after the production fence, IOSurface write, and readback checks.
`waitUntilCompleted` includes callback completion, rejects waiting on unfinished
work from the same device worker, and never substitutes an artificial fence.

`getBytes:bytesPerRow:fromRegion:mipmapLevel:` copies the completed full mip-zero
image as top-left BGRA8. The destination stride and address interval are checked
for overflow; the caller owns a buffer large enough for the requested rows.
CPU texture replacement, views, sampling, blit, and arbitrary regions are
unsupported. BGRA uses **straight alpha**; neither this adapter nor the existing
surface presenter silently premultiplies it.

`MellowGetCompletedRenderTextureIOSurface(texture)` is an explicit borrowed
getter. It returns null before completed GPU content or after a failed write
invalidates it. Keep the texture alive and exclude further writes while copying
the surface. IOSurface rows use bottom-left origin. Call the existing
`MellowSurfacePresenter` on the main thread with `sourceBottomLeft:YES`; it
copies an immutable image and commits an application layer transaction. This
does not implement `CAMetalDrawable` or prove WindowServer driver adoption or
display scanout. Frame telemetry records actual CGL fence/readback/surface IDs,
and driver strings without fabricated physical PCI IDs.

`render-acceptance.mm` runs two actual 64×48 GPU passes with varying triangle and
gradient parameters. It verifies all 6144 pixels with the existing independent
CPU oracle, checks copied IOSurface channel/orientation/straight-alpha layout,
actual frame completion and cleanup, asynchronous callbacks, negative encoding,
and texture lifetime after the caller releases its references. The default run
is offscreen. Explicit `--present` also creates a visible app window, checks the
presenter receipt, and holds the last completed frame for two seconds.

The native executable is `mellow-metal-render-acceptance [--present]`. JSON
`PASSED`/exit 0 means these checks passed on its recorded accelerated provider;
known absent CGL capability or an out-of-scope macOS host yields `NOT_RUN`/exit
77 before render submission. API/internal/compilation/submission failures yield
`FAILED`/exit 1. All receipts preserve false system Metal, WindowServer, physical
PCI, and scanout claims. `renderSubmissionAttempted` distinguishes an attempted
valid render commit from a provider that was never usable.

The coordinated native builder links `MellowAppleRenderMetal.mm` alongside
`MellowAppleMetal.mm` (shared error symbols), production runtime sources, and
the existing surface presenter. The render client additionally requires AppKit,
QuartzCore, CoreGraphics, CoreFoundation, IOSurface, OpenGL, Foundation, and
Metal frameworks. Full macOS 15/26 userspace SDKs and native runtime tests are
required; source inspection or a kernel SDK is not a native build or GPU test.
The counter attachment loop uses the SDK's named `MTLMaxRenderPassSampleBuffers`
declaration with no guessed numeric fallback. Availability and declaration of
that symbol, along with the entire Objective-C adapter, remain pending actual
macOS 15/26 userspace SDK compilation.

API semantics were checked against Apple's documentation for
[texture readback](https://developer.apple.com/documentation/metal/mtltexture),
[clear/store actions](https://developer.apple.com/documentation/metal/setting-load-and-store-actions),
[render target width](https://developer.apple.com/documentation/metal/mtlrenderpassdescriptor/rendertargetwidth?language=objc),
[sample positions](https://developer.apple.com/documentation/metal/mtlrenderpassdescriptor/getsamplepositions%3Acount%3A),
[texture swizzle](https://developer.apple.com/documentation/metal/mtltexturedescriptor/swizzle),
and [render pipeline descriptors](https://developer.apple.com/documentation/metal/mtlrenderpipelinedescriptor).
