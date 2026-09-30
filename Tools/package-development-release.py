#!/usr/bin/env python3
"""Assemble real same-commit CI binaries; never execute downloaded payloads.

CI-only Python/GitHub CLI tooling. End users need only the native macOS CLI.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import plistlib
import re
import stat
import struct
import subprocess
import tarfile
import zipfile

REPO = "NiSeullent/Mellow"
MAX_ZIP = 64 * 1024 * 1024
CLAIMS = dict(physicalGPUVerified=False, systemMetalRegistered=False,
              windowServerAccelerationVerified=False)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode()


def path_ok(name):
    parts = name.split("/")
    return (0 < len(name.encode()) <= 240 and
            re.fullmatch(r"[A-Za-z0-9._/-]+", name) is not None and
            all(part not in ("", ".", "..") and len(part) <= 100 for part in parts))


def read_zip(raw):
    require(len(raw) <= MAX_ZIP, "Oversized ZIP container")
    result, seen, total = {}, set(), 0
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        entries = archive.infolist()
        require(len(entries) <= 4096, "ZIP entry budget exceeded")
        for entry in entries:
            name = entry.filename.rstrip("/") if entry.is_dir() else entry.filename
            require(path_ok(name) and name.lower() not in seen, "Invalid or aliased ZIP path")
            seen.add(name.lower())
            mode = entry.external_attr >> 16
            require(stat.S_IFMT(mode) in (0, stat.S_IFREG, stat.S_IFDIR), "ZIP special file")
            require(not entry.flag_bits & 1 and entry.compress_type in
                    (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), "Unsupported ZIP encoding")
            require(entry.file_size <= MAX_ZIP and total <= MAX_ZIP - entry.file_size,
                    "ZIP expansion budget exceeded")
            total += entry.file_size
            if entry.is_dir():
                require(entry.file_size == 0, "Nonempty ZIP directory")
                continue
            data = archive.read(entry)  # Also checks the actual CRC.
            require(len(data) == entry.file_size, "ZIP extent differs")
            result[name] = data
    return result


def api(route):
    require(route.startswith(f"repos/{REPO}/"), "Unexpected GitHub API repository")
    return json.loads(subprocess.check_output(["gh", "api", route]))


def fetch_artifact(run_id, name, commit, workflow, folder):
    require(re.fullmatch(r"[1-9][0-9]{0,19}", str(run_id)), "Invalid run ID")
    run = api(f"repos/{REPO}/actions/runs/{run_id}")
    require(run["head_sha"] == commit and run["status"] == "completed" and
            run["conclusion"] == "success" and run["path"].split("@")[0] == workflow,
            "Artifact run is not a successful exact-source build")
    listing = api(f"repos/{REPO}/actions/runs/{run_id}/artifacts?per_page=100")
    require(listing["total_count"] <= 100, "Unexpected artifact count")
    matches = [item for item in listing["artifacts"] if item["name"] == name]
    require(len(matches) == 1, "Missing or duplicate exact artifact")
    item = matches[0]
    require(not item["expired"] and 0 < item["size_in_bytes"] <= MAX_ZIP and
            re.fullmatch(r"sha256:[0-9a-f]{64}", item.get("digest", "")),
            "Artifact SHA256 or extent unavailable")
    target = folder / f"{item['id']}.zip"
    with target.open("xb") as output:
        # gh handles authentication and strips credentials on cross-origin
        # redirects. Neither the URL nor the token enters a shell expression.
        subprocess.run(["gh", "api", f"repos/{REPO}/actions/artifacts/{item['id']}/zip"],
                       stdout=output, check=True)
    require(target.stat().st_size <= MAX_ZIP, "Downloaded artifact exceeded bound")
    raw = target.read_bytes()
    require(len(raw) == item["size_in_bytes"] and "sha256:" + sha(raw) == item["digest"],
            "GitHub artifact digest or size differs")
    return read_zip(raw), dict(runID=int(run_id), artifactID=item["id"], name=name,
                               sha256=sha(raw), bytes=len(raw), runURL=run["html_url"])


def macho(raw, kind, target, sdk, install_name=None, allow_no_version=False, minimum=None):
    require(len(raw) >= 32, "Truncated linked Mach-O")
    magic, cpu, subtype, filetype, count, length, flags, reserved = struct.unpack_from("<8I", raw)
    require(magic == 0xFEEDFACF and cpu == 0x01000007 and filetype == kind and
            0 < count <= 256 and length <= len(raw) - 32, "Unexpected linked Mach-O identity")
    offset, versions, names, signatures = 32, [], [], 0
    for _ in range(count):
        require(offset + 8 <= 32 + length, "Truncated Mach-O command")
        command, size = struct.unpack_from("<II", raw, offset)
        require(size >= 8 and size % 8 == 0 and size <= 32 + length - offset,
                "Invalid Mach-O command extent")
        if command == 0x32:
            require(size >= 24, "Short build-version command")
            versions.append(struct.unpack_from("<III", raw, offset + 8))
        if command == 0x1D:
            require(size == 16, "Short signature command")
            begin, extent = struct.unpack_from("<II", raw, offset + 8)
            require(extent > 0 and begin <= len(raw) and extent <= len(raw) - begin,
                    "Signature extent differs")
            signatures += 1
        if command == 0xD:
            require(size >= 24, "Short dylib identity")
            begin = struct.unpack_from("<I", raw, offset + 8)[0]
            require(24 <= begin < size, "Invalid dylib name")
            text = raw[offset + begin:offset + size]
            require(b"\0" in text, "Unterminated dylib name")
            names.append(text.split(b"\0", 1)[0].decode("utf-8"))
        offset += size
    sdk_major, sdk_minor = map(int, sdk.split("."))
    expected = (1, (target if minimum is None else minimum) << 16, sdk_major << 16 | sdk_minor << 8)
    require(offset == 32 + length and signatures == 0, "Expected unsigned development binary")
    require(versions == [expected] or (allow_no_version and versions == []),
            "Binary deployment/SDK differs from actual CI target")
    if install_name is not None:
        require(names == [install_name], "Native framework install-name differs")
    return dict(fileType=filetype, architecture="x86_64", buildVersionCommands=versions,
                signing="unsigned-development", installNames=names)


def assemble(args):
    root = pathlib.Path.cwd()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    require(re.fullmatch(r"[a-f0-9]{40}", commit), "Invalid checkout commit")
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}", args.release), "Invalid release tag")
    major, sdk = args.target, "15.5" if args.target == 15 else "26.2"
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    kernel, kp = fetch_artifact(args.kernel_run, f"Mellow-native-development-{major}.0",
                               commit, ".github/workflows/build.yml", out)
    settings = json.loads(kernel["native-Release/build-settings.json"])
    bs = next(item["buildSettings"] for item in settings if item.get("target") == "Mellow")
    require(all(bs[key] == value for key, value in dict(ARCHS="x86_64", SDK_VERSION=sdk,
            MACOSX_DEPLOYMENT_TARGET=f"{major}.0", CODE_SIGNING_ALLOWED="NO").items()),
            "Actual Xcode kext build settings differ")
    kext = read_zip(kernel["Mellow-native-development.zip"])
    apps, ap = fetch_artifact(args.userspace_run, f"Mellow-Apple-userspace-{major}.0",
                             commit, ".github/workflows/apple-userspace.yml", out)
    # Both CI packagers copy an independently resolved exact checkout commit.
    for contents in (kext, apps):
        require(contents["SOURCE-COMMIT.txt"] == (commit + "\n").encode(),
                "Native bundle source provenance differs")
        for name in ("LICENSE", "NOTICE"):
            require(contents[name] == (root / name).read_bytes(), "Redistribution notice differs")
    report = json.loads(apps["apple-userspace-build.json"])
    require(report["native_compilation"] == "PASSED" and report["status"] == "BUILT_ONLY" and
            report["target"]["architecture"] == "x86_64" and
            report["target"]["deployment_target"] == f"{major}.0" and
            report["sdk"]["version"] == sdk and not report["source_changed_during_build_or_run"] and
            not report["artifact_changed_during_build_or_run"], "Userspace build evidence differs")
    for key in ("system_metal_registered", "system_windowserver_acceleration_verified",
                "all_gpu_models_verified", "physical_scanout_verified"):
        require(report[key] is False, "Unexpected hardware/system success claim")
    require(all(value is False for value in report["requested_execution"].values()),
            "Unexpected GPU execution request")
    for name, expected in report["source_sha256"].items():
        require(path_ok(name) and sha((root / name).read_bytes()) == expected,
                "Compiled source hash differs from exact checkout")
    for name, expected in report["artifacts"].items():
        require(name in apps and sha(apps[name]) == expected, "Linked artifact hash differs")
    payload, executables = {}, set()
    def add(name, data, executable=False):
        path = "payload/" + name
        require(path_ok(path) and len(data) <= 128 * 1024 * 1024, "Invalid payload path/extent")
        if path in payload:
            require(payload[path] == data, "Conflicting redistribution file")
        payload[path] = data
        if executable:
            executables.add(path)
    for name, data in kext.items():
        if name.startswith("Mellow.kext/") or name.startswith(("LICENSES/", "Drivers/")) or name in ("LICENSE", "NOTICE", "SOURCE-COMMIT.txt", "SOURCE-URL.txt", "VALIDATION-SCOPE.txt"):
            add(name, data, name == "Mellow.kext/Contents/MacOS/Mellow")
    framework = "MellowAppleUserspace.framework/"
    for name, data in apps.items():
        if name.startswith(framework):
            add(name, data, name.endswith("/MellowAppleUserspace"))
            canonical = name.replace("/Versions/Current/", "/Versions/A/")
            if "/Versions/" not in name:
                canonical = framework + "Versions/A/" + name[len(framework):]
            require(canonical in apps and apps[canonical] == data, "Framework materialized alias differs")
    for name in ("libMellowAppleUserspace.dylib", "mellow-compute-acceptance",
                 "mellow-metal-render-acceptance", "mellow-windowserver-acceptance", "metal-inventory"):
        add("bin/" + name, apps[name], True)
    # Its @loader_path is the containing directory beside the real framework.
    add("mellow-framework-link-check", apps["mellow-framework-link-check"], True)
    for name in ("metal-probe", "tahoe-diag-client"):
        add("bin/" + name, kext[name], True)
    identities = dict(kext=macho(kext["Mellow.kext/Contents/MacOS/Mellow"], 11, major, sdk,
                                allow_no_version=True),
                      framework=macho(apps[framework + "Versions/A/MellowAppleUserspace"], 6, major, sdk,
                                      "@rpath/MellowAppleUserspace.framework/Versions/A/MellowAppleUserspace"))
    for name in ("libMellowAppleUserspace.dylib", "mellow-compute-acceptance", "mellow-metal-render-acceptance",
                 "mellow-windowserver-acceptance", "metal-inventory", "mellow-framework-link-check"):
        identities[name] = macho(apps[name], 6 if name.endswith(".dylib") else 2, major, sdk)
    # These existing diagnostic build commands deliberately have older minimum
    # OS targets. Record their actual bytes; never assign the package target to
    # a diagnostic whose load commands say otherwise.
    identities["metal-probe"] = macho(kext["metal-probe"], 2, major, sdk, minimum=13)
    identities["tahoe-diag-client"] = macho(kext["tahoe-diag-client"], 2, major, sdk, minimum=15)
    for name, identifier, executable, kind in (
        ("Mellow.kext/Contents/Info.plist", "com.NiSeullent.Mellow", "Mellow", "KEXT"),
        (framework + "Versions/A/Resources/Info.plist", "com.NiSeullent.Mellow.AppleUserspace", "MellowAppleUserspace", "FMWK")):
        data = plistlib.loads(payload["payload/" + name])
        require(data["CFBundleIdentifier"] == identifier and data["CFBundleExecutable"] == executable and
                data["CFBundlePackageType"] == kind and data["CFBundleVersion"], "Bundle identity differs")
    diagnostic = plistlib.loads(kext["Mellow.kext/Contents/Info.plist"])["IOKitPersonalities"]["MellowTahoeDiagnostic"]
    require(diagnostic["IOPCIPrimaryMatch"] == "0x7D418086", "Unexpected diagnostic PCI match")
    evidence = dict(schemaVersion=1, repository=REPO, release=args.release, sourceCommit=commit,
                    targetOSMajor=major, sdkVersion=sdk, kernel=kp, userspace=ap,
                    actualKextBuildSettings={key: bs[key] for key in ("ARCHS", "SDK_VERSION", "MACOSX_DEPLOYMENT_TARGET", "CODE_SIGNING_ALLOWED")},
                    binaryIdentity=identities, capabilities=CLAIMS,
                    scope="Native SDK build and checked real payload bytes; no payload execution, firmware, GPU or system registration",
                    kextDependencies=plistlib.loads(kext["Mellow.kext/Contents/Info.plist"])["OSBundleLibraries"])
    add("BUILD-PROVENANCE.json", json_bytes(evidence))
    add("validation/apple-userspace-build.json", apps["apple-userspace-build.json"])
    add("validation/kernel-build-settings.json", kernel["native-Release/build-settings.json"])
    add("validation/kernel-macho.json", kernel["native-Release/macho-validation.json"])
    manifest = dict(schemaVersion=1, repository=REPO, release=args.release, sourceCommit=commit,
                    architecture="x86_64", targetOSMajor=major, sdkVersion=sdk, scope="user-development",
                    capabilities=CLAIMS, files=[dict(path=name, size=len(data), sha256=sha(data), executable=name in executables)
                                               for name, data in sorted(payload.items())],
                    bundles=[dict(kind="kext", path="payload/Mellow.kext", identifier="com.NiSeullent.Mellow", executable="Mellow",
                                  binaryPath="payload/Mellow.kext/Contents/MacOS/Mellow", signing="unsigned-development"),
                             dict(kind="framework", path="payload/MellowAppleUserspace.framework", identifier="com.NiSeullent.Mellow.AppleUserspace",
                                  executable="MellowAppleUserspace", binaryPath="payload/MellowAppleUserspace.framework/Versions/A/MellowAppleUserspace", signing="unsigned-development")])
    members = {"manifest.json": json_bytes(manifest), **payload}
    require(len(members) < 4096 and len({name.lower() for name in members}) == len(members), "Payload count/case alias")
    archive = out / f"mellow-development-macos{major}-x86_64.tar"
    with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data in sorted(members.items()):
            entry = tarfile.TarInfo(name); entry.size = len(data)
            entry.mode = 0o700 if name in executables else 0o600
            entry.uid = entry.gid = entry.mtime = 0; entry.uname = entry.gname = ""
            tar.addfile(entry, io.BytesIO(data))
    require(archive.stat().st_size <= 512 * 1024 * 1024, "Archive exceeds installer budget")
    digest = sha(archive.read_bytes())
    (out / "package-proof.json").write_bytes(json_bytes({**evidence, "archive":archive.name,
        "archiveSHA256":digest, "archiveBytes":archive.stat().st_size, "payloadFiles":len(payload)}))
    (out / "SHA256SUMS.txt").write_text(f"{digest}  {archive.name}\n")
    print(json.dumps(dict(archive=str(archive), sha256=digest, sourceCommit=commit, targetOSMajor=major)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-run", required=True)
    parser.add_argument("--userspace-run", required=True)
    parser.add_argument("--release", required=True)
    parser.add_argument("--target", required=True, type=int, choices=(15, 26))
    parser.add_argument("--out", required=True)
    assemble(parser.parse_args())
