# Compiled development packages

The native GitHub job builds the Intel kext, the actual Objective-C++ userspace
library and clients, and a distributable `MellowAppleUserspace.framework` on
`macos-15-intel`. Compilation and packaging failures fail the job. A missing SDK,
missing source or failed Objective-C++ build never falls back to a source-only
download. A successful package is a **compiled experimental development build**.
CI does not load the kext, execute a GPU client, register a system Metal driver,
or verify any physical GPU or WindowServer acceleration.

The package requires macOS 15 or later on x86_64. This is the userspace package
minimum. The native kext target currently selects macOS 13; its actual Mach-O
minimum, if present, is recorded separately. Retained kexts without a deployment
load command report `minimum_os: null` and `sdk: null`, without guessing a version.
Building on macOS 15 does not establish successful installation or execution on
macOS 26. See [INSTALLATION.md](INSTALLATION.md) for the explicit installation
and removal controls.

## Archive and release assets

The release asset is exactly `Mellow-macos-x86_64.zip`. It contains these root
entries, with no enclosing directory, symlinks, special files or duplicate paths:

```text
manifest.json
SHA256SUMS
Mellow.kext/Contents/...
frameworks/MellowAppleUserspace.framework/
  MellowAppleUserspace
  Headers/MellowAppleUserspace.h
  Headers/MellowAppleMetal.h
  Headers/MellowAppleRenderMetal.h
  Headers/SurfacePresenter.h
  Modules/module.modulemap
  Resources/Info.plist
bin/
  libMellowAppleUserspace.dylib
  mellow-compute-acceptance
  mellow-metal-render-acceptance
  mellow-windowserver-acceptance
  metal-inventory
  native-metal-client
  native-metal-abi
  metal-probe
  tahoe-diag-client
  metadata/kext-macho-validation.json
  metadata/apple-userspace-build.json
dependencies/Lilu.kext/Contents/
  Info.plist
  MacOS/Lilu
licenses/
  LICENSE
  NOTICE
  LICENSES/...
  Drivers/PortedXe/LICENSE.MIT
  Drivers/PortedNvidia/LICENSE.MIT
  Drivers/PortedNvidiaGsp/LICENSE.MIT
```

The framework is a flat bundle. The original dylib remains next to its CLI
clients in `bin/`, preserving their recorded `@loader_path` search path and
`@rpath/libMellowAppleUserspace.dylib` dependency. The framework uses a separate
copy with install ID
`@rpath/MellowAppleUserspace.framework/MellowAppleUserspace`, public headers,
an umbrella header, a Clang module map and a bundle plist. It contains the
explicit application adapter implementation. It does not provide an installed
system accelerator plugin or full `MTLDevice` protocol support.

Lilu is necessary for the kext. Its bundled binary and plist are unchanged
copies from this repository; the manifest records their actual version and
architecture and the minimum version required by the built Mellow plist.
Lilu is **not compiled in this CI run**. Development headers in the vendored
bundle are omitted. Third-party licenses remain controlling; the root product
license is TSNPL 1.0, not MIT. The manifest's commit-specific `source_url` points
to the matching source snapshot.

Successful release publication uploads exactly these public assets:

- `Mellow-macos-x86_64.zip`
- `Mellow-macos-x86_64.zip.sha256`
- `manifest.json`, byte-identical to the internal manifest
- `install-mellow.sh`
- `uninstall-mellow.sh`

The checksum sidecar has one standard line:
`<64 lowercase hex characters>  Mellow-macos-x86_64.zip` followed by a newline.
The internal `SHA256SUMS` lists every regular payload file and `manifest.json`,
using root-relative paths; it excludes `SHA256SUMS` itself. The manifest's
`files` array lists each payload file's path, size and SHA-256, excluding both
manifest and checksum list to avoid self-reference. All archive paths use
conservative ASCII components and reject empty, dot and dotdot components.
Checksums detect corruption and changes; they are not a publisher signature.

## Manifest interpretation

[release-manifest.schema.json](../packaging/release-manifest.schema.json) defines
schema version 1. `binary_compiled: true` means that all required project
binaries exist and their actual Mach-O identities have passed packaging
validation. It does not mean that a GPU executed commands. `build.status` is
`BUILT_ONLY`; `hardware_support_verified`, `system_metal_registered` and
`windowserver_acceleration_verified` are always false in this package contract.
Each artifact records its actual architecture, Mach-O type, deployment/SDK
metadata, install names, dependencies, run paths and code-signing category.
The retained Lilu artifact has `compiled_in_this_build: false`.

`signed: false` and `developer_id_signed: false` mean that there is no publisher
or Developer ID signature. `notarized` is also false. The separate framework
copy is ad-hoc signed after its install ID changes; other binaries are measured
and recorded as `unsigned` or `ad-hoc`. `code_signing` therefore reads
`mixed-unsigned-and-ad-hoc`. Ad-hoc signing does not identify a publisher or
permit production kext activation. The packager rejects unexpected signing
identities rather than inferring trust.

The packager matches the actual kext binary against its native validation
receipt, the userspace binaries and source files against the successful native
build receipt, and the current clean tracked checkout against `source_commit`.
It validates every required executable and dylib header, the adjacent dylib
dependency and the framework install ID, then verifies the completed ZIP and
every checksum. It writes `release-build.json` outside the ZIP with packaging
tool results. Partial staging and failure reports may exist after failure;
the failed run removes the downloadable ZIP and public sidecars.

## CI and publication

`.github/workflows/build.yml` preserves the existing kext build and host tests.
It also invokes the real userspace builder without execution flags, the native
Metal makefile and the common packager. Both userspace compilation steps are
mandatory. The CI installation check extracts the ZIP to a temporary user
prefix, compares installed framework/dylib bytes, checks the client load paths,
removes managed assets while retaining an unrelated user file, and rejects
both an absent archive and a mismatched external ZIP digest. It never uses
`--install-kext` or executes a GPU client. The small packaging self-test checks
valid ZIP/checksum handling, tampering and unsafe paths; it manufactures no
Mach-O or hardware evidence.

The `Mellow-macos-x86_64` CI artifact contains the five public assets above.
The separate `Mellow-native-development` artifact retains the earlier kext
package, logs and actual build receipts, including failure reports when
available. A source archive or a logs-only artifact must not be described as
the compiled release package.

`.github/workflows/release.yml` is a manual workflow for an **existing tag**.
It resolves the tag to an exact commit, calls the same native build workflow,
verifies the resulting commit/tag/checksum metadata, then creates an
experimental GitHub prerelease. It does not replace an existing release or
move a tag. The workflow must first be present on the default branch before
GitHub exposes its dispatch entry. A maintainer may instead publish the exact
verified artifacts from a successful branch build, preserving their manifest
source commit. The existing native build's manual dispatch accepts an optional
`release_tag` to record a planned release tag in its manifest. This input creates
or moves no tag; before publishing those assets, the maintainer must bind that
tag to the manifest's exact source commit. Ordinary pushes leave `release_tag`
null. Website publication uses a separate workflow or branch.

For local packaging after explicitly approved native builds, use the same
inputs as CI:

```bash
python3 packaging/build-release.py \
  --kext-settings build/native-Release/build-settings.json \
  --kext-validation build/native-Release/macho-validation.json \
  --userspace-dir /absolute/native-userspace-output \
  --native-clients-dir /absolute/native-metal-output \
  --metal-probe build/metal-probe \
  --diagnostic-client build/tahoe-diag-client \
  --out /absolute/new-release-output \
  --source-commit EXACT_40_CHARACTER_COMMIT \
  --source-ref refs/tags/EXISTING_TAG \
  --release-tag EXISTING_TAG
```

The output must be new or empty and outside the source checkout. Packaging
copies files, creates the framework, invokes `install_name_tool` and ad-hoc
`codesign`, writes manifests/checksums/ZIPs and records tool output. It does not
download firmware or libraries, start a service, edit host configuration, or
install/load a kernel extension. This document defines the build contract;
only a successful CI run and its actual artifacts establish that compilation
and packaging completed.
