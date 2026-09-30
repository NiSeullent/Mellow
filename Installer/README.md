# Mellow native macOS development installer

The installer downloads actual development binaries from
[NiSeullent/Mellow releases](https://github.com/NiSeullent/Mellow/releases).
It requires a native Intel Mac running macOS 15 or 26. Rosetta and root/sudo
execution are refused. End users need no Python, Homebrew or Xcode.

This installs files into your user directory. The diagnostic `Mellow.kext`
contains the Intel `8086:7D41` development profile; it does not establish working
GPU acceleration. `MellowAppleUserspace.framework` works only in applications
that explicitly use its adapter. System Metal registration, WindowServer
acceleration and physical GPU success remain unverified. Installation does not
load a kext, edit EFI/OpenCore/config.plist, change SIP, reboot, or alter global
configuration. Unsigned development code remains unsigned; this tool does not
remove quarantine or bypass macOS approval mechanisms.

## Download, review and install

Select a release tag that actually contains the assets below. Download
`mellow-install.sh` from that release and review it first. For example, replace
`TAG` with the selected tag:

```sh
curl --fail --location --proto '=https' --proto-redir '=https' \
  --output mellow-install.sh \
  https://github.com/NiSeullent/Mellow/releases/download/TAG/mellow-install.sh
less mellow-install.sh
bash ./mellow-install.sh --help
bash ./mellow-install.sh --release TAG
```

The bootstrap downloads the host's native CLI and checks the SHA256 digest
reported by the [GitHub release-asset API](https://docs.github.com/en/rest/releases/assets)
before executing it. The CLI independently verifies the payload's GitHub digest,
resolves the tag's actual source commit, and checks the manifest and bundle
identities. This trusts the selected public repository and GitHub release
metadata. A SHA256 checksum supplied manually for an offline archive verifies
those bytes; it is not independent publisher authentication.

The default prefix is `~/Library/Application Support/Mellow`. A different
absolute, user-owned directory can be selected with `--prefix`. Its ancestors
must be real directories; symlink prefixes and roots are refused. A new prefix
may be claimed only when it is empty. Neither installer requires `sudo`.

The standalone CLI supports:

```sh
./mellow-installer-macos15-x86_64 install --release TAG
./mellow-installer-macos15-x86_64 install --release TAG \
  --archive mellow-development-macos15-x86_64.tar --sha256 EXPECTED_HEX
./mellow-installer-macos15-x86_64 status
./mellow-installer-macos15-x86_64 list
./mellow-installer-macos15-x86_64 rollback --release PREVIOUS_TAG
./mellow-installer-macos15-x86_64 uninstall --release TAG
```

Use the `macos26` CLI and payload on macOS 26. All commands accept `--prefix`.
Commands return JSON, with nonzero exit on validation or transaction failure.
Installed files live under `PREFIX/versions/TAG/payload/`. `state.json` records
the current and previous versions; there is no automatically followed `current`
symlink. Release directories are never overwritten. Rollback selects an existing
validated release. Uninstall deletes only the exact regular files recorded in a
validated receipt/manifest; changed, untracked, linked or foreign files stop it.

The installer holds a store lock, stages on the same filesystem, then publishes
using exclusive atomic rename. A failed state update removes only its own
complete staging tree and preserves the previous selection. Interrupted or
externally changed staging trees are preserved for inspection rather than swept
away. If an atomic rename succeeds but directory synchronization fails, the CLI
returns `DURABILITY_UNCERTAIN` and preserves the relevant resources. It does not
report a durable success or delete a release referenced by a newly published
state. After such a storage error, inspect the store before further operations.
The owned store is not a security boundary against another process running
as the same user; do not modify its files concurrently outside this CLI.

## Release asset and manifest contract

For each target, publish `mellow-installer-macos15-x86_64` or
`mellow-installer-macos26-x86_64`, `mellow-install.sh`, and the matching
`mellow-development-macos15-x86_64.tar` or
`mellow-development-macos26-x86_64.tar`.

Payload archives are uncompressed POSIX USTAR containing only regular files and
directories. Links, PAX/GNU extensions, devices, traversal, path aliases,
duplicate names, untracked files, special permissions, nonzero padding and
incomplete trailers are rejected before files are written. Limit: 512 MiB total,
128 MiB per file, 4,096 archive entries, ASCII relative paths of at most 240 bytes.
Framework symlink aliases must be materialized as exact regular file copies by
the release packager. The installed framework keeps its canonical `Versions/A`
install-name. Exact file hashes are listed in root `manifest.json`:

```json
{
  "schemaVersion": 1,
  "repository": "NiSeullent/Mellow",
  "release": "TAG",
  "sourceCommit": "40 lowercase hexadecimal characters",
  "architecture": "x86_64",
  "targetOSMajor": 15,
  "sdkVersion": "15.5",
  "scope": "user-development",
  "capabilities": {
    "physicalGPUVerified": false,
    "systemMetalRegistered": false,
    "windowServerAccelerationVerified": false
  },
  "files": [
    {"path": "payload/LICENSE", "size": 0, "sha256": "64 lowercase hexadecimal characters", "executable": false}
  ],
  "bundles": [
    {"kind": "kext", "path": "payload/Mellow.kext", "identifier": "com.NiSeullent.Mellow", "executable": "Mellow", "binaryPath": "payload/Mellow.kext/Contents/MacOS/Mellow", "signing": "unsigned-development"},
    {"kind": "framework", "path": "payload/MellowAppleUserspace.framework", "identifier": "com.NiSeullent.Mellow.AppleUserspace", "executable": "MellowAppleUserspace", "binaryPath": "payload/MellowAppleUserspace.framework/Versions/A/MellowAppleUserspace", "signing": "unsigned-development"}
  ]
}
```

The example is a schema illustration, not an installable manifest. Every payload
file, including `payload/SOURCE-COMMIT.txt`, `payload/LICENSE` and `payload/NOTICE`,
must be declared with its actual size, SHA256 and executable mode. The commit text
must equal the manifest commit followed by one newline. Target 26 uses SDK `26.2`.
Both exact bundle identities and plists are mandatory. The validator checks thin
x86_64 Mach-O kind, bounded load commands and framework deployment/SDK/install-name
metadata. The actual kext has no `LC_BUILD_VERSION`; its target is a manifest/CI
build declaration, not metadata inferred from its binary. Checksums and structural
validation do not prove that an unsigned binary will run successfully.

Current artifacts declare `unsigned-development` based on their actual absence
of `LC_CODE_SIGNATURE`. Future `ad-hoc-development` or `developer-id` declarations
require a real signature command and native Security static verification. Developer
ID also requires a matching `teamIdentifier` and the actual certificate chain.

## Native build and isolated verification

`.github/workflows/installer.yml` builds x86_64 CLIs with actual SDKs 15.5 and 26.2,
and publishes hashes, exact sources/licenses and load-command evidence. The
macOS 15 Intel runner cannot execute the minimum-26 release binary. Each SDK
therefore also compiles a separate minimum-15 test executable and runs it only
on the macOS 15 host.

`--self-test` uses no network, system installation or GPU. It checks bounded tar
parsing, corrupt/linked/traversal/case-aliased inputs, source and binary-header
identity failures, temporary-prefix installation, no-overwrite, rollback,
uninstall, hard-link/symlink rejection, and preservation after a failed state
commit. Its synthetic Mach-O bytes are expressly header-only fixtures. Passing
these CPU/filesystem tests does not verify executing a native payload, physical
hardware, system Metal or WindowServer. The Linux development environment has
no Swift/macOS SDK; native compile/test status comes from the actual CI results.
