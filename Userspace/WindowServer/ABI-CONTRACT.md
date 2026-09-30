# Measured Metal registration boundary and public application presentation

The implemented path in this directory is an application window: a completed
GPU-written IOSurface is snapshotted through public APIs and displayed in a
dedicated CoreAnimation sublayer. It does not register an Apple MTLDevice or
replace the accelerator used by the system WindowServer. A client PASS has this
application scope only. Native compilation and execution require a Mac and its
installed SDK; they have not been performed in this Linux workspace.

## Exact target evidence

The existing `abi-evidence/tahoe-graphics-inventory.json` inventories actual
x86_64 macOS 26.6.2 / build 25G83 Recovery artifacts. IOAcceleratorFamily2 is
487.4.3, compatible version 2.0.0, UUID
`b77a42ec6b0c3a8d89a2b8f4cdbac8c9`. The Metal image UUID is
`5d64fa8029ce32aabab64e5034132c0b`; IOAccelerator.framework UUID is
`0f4e28cd8a033d84bb553b8afbf5626c`; CoreDisplay UUID is
`8bfeff75c8c83b5bafa061385199a1bb`.

`registration-evidence.json` records the hashes of these existing reports and
the observed symbols below. No macOS 15 private ABI has been measured here.
The version-specific inventory remains `private_abi_verified=false`.

| Boundary | Actual 25G83 evidence | Implementation consequence |
| --- | --- | --- |
| App enumeration | `_MTLCopyAllDevices`, `_MTLCreateSystemDefaultDevice`, `-[MTLIOAccelDevice registryID]` | These obtain existing system devices. No provider-install API was established. |
| Metal service/device objects | `-[MTLIOAccelDevice initWithAcceleratorPort:]`, `-[MTLIOAccelService initWithAcceleratorPort:deviceClass:]`, `-[MTLIOAccelServiceGlobalContext registerService:deviceClass:]` | Selector spelling and owner are confirmed. Argument encodings, return ABI, dispatch order, subclass duties and lifetime rules are unverified. They are not callable registration declarations in this port. |
| Intel bundle identity | `abi-evidence/intel-umd-partial.json`: principal class `MTLIGAccelDevice`, executable `AppleIntelICLGraphicsMTLDriver` | The main executable was not acquired. The inspected `libigdmd.dylib` helper is not that plugin. Its export list cannot define the plugin factory or initialization contract. |
| IOAccelerator user-space device creation | `_IOAccelDeviceCreate`, `_IOAccelDeviceCreateWithAPIProperty` | The functions exist, but no matching target prototype, API-property semantics or object ownership contract was established. |
| Kernel accelerator entry | `IOGraphicsAccelerator2::newUserClient(task*, void*, unsigned int, IOUserClient**)` | Its symbol is present. The client type mapping and concrete user-client dispatch tables are not recovered. |
| Shared GPU objects | `IOAccelSharedUserClient2::new_resource(IOAccelNewResourceArgs*, IOAccelNewResourceReturnData*, unsigned long long, unsigned int*)`, `create_shmem(unsigned int, IOAccelDeviceShmemData*)`, `externalMethod(...)` | Named structures exist. Their field offsets, sizes, selector numbers, mapped memory types, validation and ownership rules are unverified. |
| Command submission | `IOAccelCommandQueue::submit_command_buffers(IOAccelCommandQueueSubmitArgs const*)`, `s_submit_command_buffers(..., IOExternalMethodArguments*)` | A submit structure and dispatch adapter exist; the structure and command stream format remain unknown. |
| Kernel surface import | `IOAccelResource2::newResourceWithIOSurface(IOGraphicsAccelerator2*, IOAccelShared2*, IOSurface*, unsigned int, unsigned int)` | GPU resource import exists. The two flags and resource/fence semantics are not inferred from this name. |
| System display presentation | `_CoreDisplay_Display_PresentIOAccelSurfaceWithIOAccelSurfaceInfoAndBufferIndex` | The export exists. Its private prototype, surface selection and WindowServer call flow are unverified. A function name is not a presentation implementation. |

The Objective-C names above come from definitions as well as method-name strings;
the report does not contain verified method type encodings or the plugin body.
C++ mangling identifies argument type names, but does not encode structure layout
or ordinary function return types. None of these rows permits an invented
success implementation, guessed selector table, C++ object layout or factory.

## Public-source cross checks

[Apple's WebKit sandbox](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/WebProcess/com.apple.WebProcess.sb.in)
recognizes `MetalPluginName`, `MetalPluginClassName`, `IOAccelTypes`,
`IOAccelRevision`, `IOAccelIndex` and `IOAccelDisplayPipeCapabilities` as readable
IOKit properties. This confirms property vocabulary used by Apple's stack; it
does not specify their values, types or sufficient registration conditions.
The [original WebKit change](https://commits.webkit.org/231438@main) separately
records the two Metal plugin keys. This port does not publish them on an
unimplemented accelerator.

Apple's open IOGraphics
[IOAccelClientConnect.h](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOKit/graphics/IOAccelClientConnect.h)
names the older `IOAccelerator` service and surface client type. Its
[IOAccelSurfaceConnect.h](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOKit/graphics/IOAccelSurfaceConnect.h)
and [IOAccelTypes.h](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOKit/graphics/IOAccelTypes.h)
describe that surface interface and structures. They do not define the observed
`IOAccelSharedUserClient2` or `IOAccelCommandQueueSubmitArgs` contract. Legacy
surface selector values cannot be substituted for the modern GPU user client.

[XNU IOUserClient.h](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOUserClient.h)
defines the general `IOExternalMethodArguments`/dispatch envelope, including
scalar and structure counts. It does not assign accelerator-specific selectors
or define their payloads. [Apple's public Metal enumeration API](https://developer.apple.com/documentation/metal/mtlcopyalldevices())
and [registryID](https://developer.apple.com/documentation/metal/mtldevice/registryid)
provide an actual device correlation test once a real provider is registered.

The unresolved native registration contract is consequently exact: the target
plugin loading/factory path; complete Objective-C initialization and capability
contract; accelerator client types and external-method layouts; GPU object,
IOSurface, command, residency, fence/reset and lifetime semantics; and the
WindowServer/CoreDisplay provider selection and presentation contract. Neither
the 25G83 symbol inventory nor the inspected public sources establish this
contract for macOS 15 or 26. App presentation cannot fill those missing fields.

## Implemented public surface contract

`SurfacePresenter.h/.mm` calls
[IOSurface read-lock, layout and base-address APIs](https://developer.apple.com/documentation/iosurface/iosurface-functions?language=objc),
[CGDataProviderCreateWithCFData / CGImageCreate](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/drawingwithquartz2d/dq_data_mgr/dq_data_mgr.html),
and [CALayer.contents](https://developer.apple.com/documentation/QuartzCore/CALayer/contents?language=occ)
inside a main-thread CATransaction. It uses no private CoreAnimation surface
function. Apple documents CGImage layer contents and cautions that a view can
overwrite its own backing-layer contents; the acceptance app therefore uses a
dedicated sublayer.

The producer must finish GPU writes and retain exclusive surface ownership
before the snapshot call. In the CGL route, the acceptance app requires real
`renderSubmitted`, `fenceSignaled`, `readbackCompleted`, `resourcesReleased` and
`ioSurfaceWritten`, plus matching IOSurface ID and texture/command sequence.
`IOSurfaceLock(ReadOnly)` allows required cache synchronization; it is not a
replacement for the GPU fence. [AvoidSync](https://developer.apple.com/documentation/iosurface/iosurfacelockoptions/avoidsync)
is deliberately not requested because readback may be necessary.

Accepted pixels are nonplanar BGRA8, four bytes per element, straight alpha in
sRGB. `CGImageAlphaFirst` plus little-endian 32-bit ordering describes these
bytes without pretending that the unblended GL producer premultiplied RGB.
Source stride and allocation are checked. Snapshot storage is independent and
tightly packed; OpenGL bottom-first rows are reversed without changing channels.
Dimensions are limited to 16384 and snapshot storage to 256 MiB as adapter
resource policy, not GPU capabilities. The layer retains the independent image,
so successful return allows immediate producer reuse of the original surface.
No GPU fence or source buffer is retained by the compositor through this path.

`Acceptance.mm` renders the real `tests/render_fixture.hpp` MSL subset through
the native CGL provider into an IOSurface-backed texture. It compares every
snapshot byte, including BGRA/RGBA conversion and row orientation, against actual
GL readback before displaying several frames in an AppKit window. An independent
CPU oracle then checks pixel-center triangle coverage, a bounded subpixel edge
strip, the gradient RGB values with one-unit tolerance, exact alpha and at least
500 foreground pixels. Matching empty/black readbacks cannot pass. Expected
pixels are never supplied to the runtime. It supplies no CPU-rendered image as
fallback. The source is intended for x86_64 macOS 15/26;
actual compatibility still requires native compile and execution.

The JSON receipt records `snapshotCreated` and `transactionCommitCalled`.
Commit returning is not a scanout acknowledgement. Acceptance `PASS` means
the GPU/surface/snapshot/application-layer checks completed; it keeps
`apple_metal_abi_registered`, `windowserver_acceleration_verified`,
`display_scanout_verified` and `physical_pci_identity_verified` false.
Exit 77 with `NOT_RUN` means there was no explicit presentation request or the
OS/accelerated provider was unavailable. Invalid arguments, compilation errors
and render failures return 1 with `FAIL`. A successful
application presentation returns 0. These outcomes do not certify a previously
unsupported GPU's native driver or system WindowServer acceleration.

## Build and explicit execution

The native `.mm` files require ARC and Foundation, AppKit, QuartzCore,
CoreGraphics, IOSurface and the render runtime's OpenGL/CoreFoundation libraries.
The native packager includes this client; merely building it does not run it.
An example executable invocation is:

```sh
./mellow-windowserver-acceptance --present --report /tmp/mellow-app-presentation.json --frames 90 --interval-ms 33
```

It creates a window, performs GPU work and writes only the named report. Omitting
the explicit presentation/report arguments returns `NOT_RUN`. The default frame
count keeps the window alive for approximately three seconds; display time is
bounded to 30 seconds. Native framework calls can still block inside a driver,
so the user or supervisor must enforce an external process deadline.

Portable snapshot checks can be built separately from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Wconversion -Werror -fsanitize=address,undefined -fno-omit-frame-pointer Userspace/WindowServer/SurfaceSnapshot.cpp Userspace/WindowServer/SurfaceSnapshotTests.cpp -o build/surface-snapshot-test
build/surface-snapshot-test
```

They check actual row padding/orientation, source/destination bounds, overlap,
overflow, resource limits and unchanged output on rejected layouts. They do not
test macOS framework calls or GPU execution.

`RenderFixtureOracleTests.cpp` separately checks the independent verifier with
synthetic barycentric pixels and black/row-flip/RGB/alpha/size/parameter corruption
controls. Those synthetic pixels are CPU test inputs only and are never recorded
as GPU results. It can be built with the same host flags using that single source.
