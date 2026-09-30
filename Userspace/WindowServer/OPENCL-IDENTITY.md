# OpenCL device identity and initialization outcome

`Runtime/OpenCLProvider` can select a GPU without inventing a PCI product ID.
This is a host OpenCL execution path through the installed implementation. It
does not register an Apple Metal device or a WindowServer accelerator.

## Identity sources

The core query `CL_DEVICE_VENDOR_ID` returns a 32-bit identifier. PCI vendors
use their 16-bit vendor identifier with the remaining bits zero; Khronos vendor
identifiers start at `0x10000`. `CL_DEVICE_TYPE` is an implementation-reported
single device type, not physical hardware attestation. The adapter checks for
exactly the GPU type and retains the full vendor value in `apiVendorId` when
using API object identity. [Khronos clGetDeviceInfo reference](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clGetDeviceInfo.html).

`CL_DEVICE_ID_INTEL` (`0x4251`) belongs to the optional
`cl_intel_device_attribute_query` extension. Its value is a PCI product ID when
the implementation is primarily driven by such a PCI device; other platforms
may use a platform-defined identifier. The adapter queries it only when the
selected device advertises the extension. An advertised query that fails or
returns an unusable identifier fails initialization; it does not silently
downgrade identity. [Intel extension specification](https://registry.khronos.org/OpenCL/extensions/intel/cl_intel_device_attribute_query.html).

Without that extension, the adapter leaves `reportedDeviceId` and the
descriptor's `deviceId` zero. `IdentityScope::OpenClDeviceObject` explicitly
identifies the selected live `cl_device_id` handle. `apiVendorId` holds the exact
32-bit core query result, and the legacy 16-bit `vendorId` is zero when that
value cannot be represented. Names, device counts and renderer-name matching
never manufacture a PCI identity.

The existing `IdentityScope::ReportedPci` default is preserved for existing
provider descriptors and positional initializers. Its driver-reported values
are not proof of correspondence to an independently enumerated PCI function.
`apiObjectIdentityVerified` means API object ownership and the bootstrap below
passed, including on that existing path. It never means physical PCI identity
was verified.

## Admission and lifetime

Before publishing a provider validation record, the adapter checks GPU type,
vendor query, device/compiler availability, context and queue ownership,
ordered queue and profiling properties. It builds and dispatches the actual
OpenCL C witness `x[i] = x[i] * 7u + 3u`, waits for its event, checks event
ownership and GPU timestamps, reads its output and compares every result.
Object cleanup must also succeed. Merely obtaining a device handle is
insufficient.

The policy layer accepts API object identity only for
`Host` + `OpenCL` + `Hardware` with current adapter-owned validation evidence.
It rejects this identity scope for `Native`, `Mellow`, OpenGL and reference
providers. A handle identifies a live object in this process and session; reset
invalidation clears verified evidence and increments the epoch. API object
identity is rejected by the persistent cache identity validator.

Matching handles do not establish interoperation. Different provider queues
still need an explicit resource transfer contract with compatibility, ordering
and preserved-content evidence. No CL/GL sharing is inferred here.

## Other correlation APIs

`cl_khr_pci_bus_info` is optional per device and must not be assumed available
on a platform. A future PCI correlation path would need to check advertisement
and independently match the returned PCI address; this adapter does not query
it. [Khronos PCI bus extension reference](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/cl_khr_pci_bus_info.html).

Apple's archived TN2335 documents `clGetGLContextInfoAPPLE` with
`CL_CGL_DEVICE_FOR_CURRENT_VIRTUAL_SCREEN_APPLE` for a CL context made from a
CGL sharegroup. It returns the CL device for that CGL virtual screen. The note
also documents an older renderer-ID conversion. Neither path is implemented
here, and the 2014 note does not verify their availability or ABI on macOS 15
or 26. Even a working device correlation would not establish resource sharing
without separate runtime checks. [Apple TN2335](https://developer.apple.com/library/archive/technotes/tn2335/_index.html).

## Structured initialization result

`initializeDetailed(index)` returns `OpenCLInitialization` containing
`status`, `bootstrapSubmissionAttempted` and `error`; the original boolean API
wraps this result.

| Status | Meaning |
| --- | --- |
| `Ready` | Discovery, ownership and completed bootstrap checks succeeded. |
| `Unavailable` | Loader or platforms absent, requested GPU absent, or the selected device/compiler reports unavailable; no bootstrap submission was attempted. |
| `Failure` | API/query/ownership/allocation/build/cleanup error, invalid identity or type, duplicate initialization, or any failed attempted bootstrap submission. |

The attempted flag is set before the enqueue call, so an enqueue error with
uncertain execution remains `Failure`, even when no event is returned. A
bootstrap compiler error is also `Failure` before enqueue. Initialization
failure preserves the attempt evidence while invalidating provider admission.
A duplicate initialization reports `Failure` for the new attempt while
preserving the existing ready session.

`tests/opencl_runtime_regression.cpp` uses a synthetic ICD to verify these
state and error boundaries. Its calculations run on the CPU and provide no
physical GPU, native Metal or WindowServer evidence.
