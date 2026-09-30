#!/usr/bin/env python3
"""Package real macOS build products; never execute a GPU client or load a kext.

This file is independently authored MIT-licensed packaging code. The packaged
product and third-party files retain their own licenses; see licenses/NOTICE.
"""
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mellow contributors.
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import tempfile
from datetime import datetime, timezone
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ASSET = "Mellow-macos-x86_64.zip"
LIBRARY_ID = "@rpath/libMellowAppleUserspace.dylib"
FRAMEWORK_ID = "@rpath/MellowAppleUserspace.framework/MellowAppleUserspace"
APPLE_BINARIES = ("libMellowAppleUserspace.dylib", "mellow-compute-acceptance",
                  "mellow-metal-render-acceptance", "mellow-windowserver-acceptance", "metal-inventory")
ROOTS = {"manifest.json", "SHA256SUMS", "Mellow.kext", "frameworks", "bin", "dependencies", "licenses"}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def valid_path(name):
    require(isinstance(name, str) and bool(re.fullmatch(r"[A-Za-z0-9_.@+/-]+", name)), "Unsafe package path")
    parts = name.split("/")
    require(all(part not in ("", ".", "..") for part in parts) and parts[0] in ROOTS,
            "Package path escapes permitted roots: " + name)
    return name


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def checked(arguments, commands):
    result = subprocess.run([str(arg) for arg in arguments], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    commands.append({"argv": [str(arg) for arg in arguments], "exit_code": result.returncode,
                     "stdout": result.stdout, "stderr": result.stderr})
    require(result.returncode == 0, "Required packaging tool failed: " + str(arguments[0]) + "\n" + result.stderr)
    return result.stdout.strip()


def copy_regular(source, destination):
    require(source.is_file() and not source.is_symlink(), "Missing/nonregular input: " + str(source))
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_bundle(source, destination):
    """Dereference only links contained in this bundle; reject cycles/special files."""
    source = source.resolve(strict=True)
    require(source.is_dir(), "Bundle is absent: " + str(source))

    def walk(current, target, ancestors):
        resolved = current.resolve(strict=True)
        require(resolved.is_relative_to(source), "Bundle symlink points outside input bundle")
        info = resolved.stat()
        if stat.S_ISDIR(info.st_mode):
            require(resolved not in ancestors, "Bundle contains a symlink cycle")
            target.mkdir(parents=True, exist_ok=False)
            for child in sorted(resolved.iterdir()):
                walk(child, target / child.name, ancestors | {resolved})
        else:
            require(stat.S_ISREG(info.st_mode), "Bundle contains a special file")
            shutil.copy2(resolved, target)

    destination.parent.mkdir(parents=True, exist_ok=True)
    walk(source, destination, set())


def macho(path, expected_kind):
    """Validate actual thin x86_64 Mach-O headers and bounded load commands."""
    with path.open("rb") as stream:
        header = stream.read(32)
        require(len(header) == 32, "Truncated Mach-O: " + str(path))
        magic, cpu, subtype, kind, count, size, flags, _ = struct.unpack("<8I", header)
        require(magic == 0xfeedfacf and cpu == 0x01000007 and kind == expected_kind,
                "Expected a thin x86_64 Mach-O of type " + str(expected_kind) + ": " + str(path))
        require(0 < count <= 4096 and count * 8 <= size <= 2 * 1024 * 1024, "Invalid Mach-O command bounds")
        data = stream.read(size)
        require(len(data) == size, "Truncated Mach-O load commands")
    at, versions, ids, dependencies, rpaths = 0, [], [], [], []
    for _ in range(count):
        require(at + 8 <= size, "Missing Mach-O command")
        command, length = struct.unpack_from("<2I", data, at)
        require(length >= 8 and length % 8 == 0 and at + length <= size, "Invalid Mach-O command length")
        item = data[at:at + length]
        if command == 0x32:
            require(length >= 24, "Short build-version command")
            _, _, target, minimum, sdk, tools = struct.unpack_from("<6I", item)
            require(target == 1 and length == 24 + tools * 8, "Invalid/non-macOS build-version command")
            versions.append((minimum, sdk))
        elif command == 0x24:
            require(length == 16, "Invalid macOS minimum-version command")
            versions.append(struct.unpack_from("<2I", item, 8))
        elif command in (0xc, 0xd, 0x80000018, 0x8000001f, 0x80000023, 0x8000001c):
            minimum_size = 12 if command == 0x8000001c else 24
            require(length >= minimum_size, "Short dylib/path command")
            offset, = struct.unpack_from("<I", item, 8)
            require(minimum_size <= offset < length, "Invalid dylib/path offset")
            end = item.find(b"\0", offset)
            require(end >= 0, "Unterminated dylib/path string")
            value = item[offset:end].decode("utf-8")
            (rpaths if command == 0x8000001c else ids if command == 0xd else dependencies).append(value)
        at += length
    require(at == size and (len(versions) == 1 or expected_kind == 11 and not versions),
            "Missing/ambiguous macOS build version")
    version = lambda word: ".".join(str(n) for n in (word >> 16, (word >> 8) & 255, word & 255))
    return {"architecture": "x86_64", "cpu_subtype": subtype, "platform": "macOS",
            "file_type": {2: "MH_EXECUTE", 6: "MH_DYLIB", 11: "MH_KEXT_BUNDLE"}[kind], "flags": flags,
            "minimum_os": version(versions[0][0]) if versions else None,
            "sdk": version(versions[0][1]) if versions else None,
            "install_names": ids, "dependencies": dependencies, "rpaths": rpaths}


def signing(path):
    result = subprocess.run(["/usr/bin/codesign", "-d", "--verbose=4", str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    text = result.stdout + result.stderr
    if result.returncode != 0 and "code object is not signed at all" in text:
        return "unsigned"
    require(result.returncode == 0 and "Signature=adhoc" in text and "Authority=" not in text,
            "Unexpected or unreadable signing identity: " + str(path) + "\n" + text)
    return "ad-hoc"


def verify_archive(archive, stage):
    expected = {p.relative_to(stage).as_posix(): p for p in stage.rglob("*") if p.is_file()}
    with zipfile.ZipFile(archive) as reader:
        infos = reader.infolist()
        require(len(infos) == len(expected) and len({item.filename for item in infos}) == len(infos),
                "ZIP has missing/duplicate/unexpected entries")
        for item in infos:
            valid_path(item.filename)
            require(item.filename in expected and stat.S_ISREG(item.external_attr >> 16), "Nonregular ZIP payload")
            require(hashlib.sha256(reader.read(item)).hexdigest() == digest(expected[item.filename]), "ZIP payload changed")
    checksums = (stage / "SHA256SUMS").read_text(encoding="ascii").splitlines()
    checksum_paths = set()
    for line in checksums:
        require(bool(re.fullmatch(r"[0-9a-f]{64}  [A-Za-z0-9_.@+/-]+", line)), "Invalid checksum line")
        sha, name = line.split("  ", 1)
        valid_path(name)
        require(name not in checksum_paths and name != "SHA256SUMS" and name in expected,
                "Unexpected/duplicate checksum path")
        require(digest(expected[name]) == sha, "Checksum differs from payload")
        checksum_paths.add(name)
    require(checksum_paths == set(expected) - {"SHA256SUMS"}, "Incomplete checksum list")


def self_test():
    """Exercise checksum/ZIP rejection without synthesizing a binary receipt."""
    with tempfile.TemporaryDirectory(prefix="mellow-packaging-test-") as temporary:
        stage = Path(temporary) / "stage"
        (stage / "bin").mkdir(parents=True)
        (stage / "bin/check.txt").write_text("packaging-only fixture\n", encoding="ascii")
        (stage / "manifest.json").write_text("{}\n", encoding="ascii")
        hashes = [f"{digest(p)}  {p.relative_to(stage).as_posix()}" for p in sorted(stage.rglob("*")) if p.is_file()]
        (stage / "SHA256SUMS").write_text("\n".join(hashes) + "\n", encoding="ascii")
        archive = Path(temporary) / "fixture.zip"
        write_archive(archive, stage)
        verify_archive(archive, stage)
        (stage / "bin/check.txt").write_text("tampered\n", encoding="ascii")
        try:
            verify_archive(archive, stage)
        except ValueError:
            pass
        else:
            raise ValueError("Tampered payload was accepted")
        for name in ("/bin/a", "bin/../a", "bin//a", "bin/a b", "unexpected/a"):
            try:
                valid_path(name)
            except ValueError:
                pass
            else:
                raise ValueError("Unsafe archive path was accepted")
    print("Packaging checksum and archive safety checks passed; no binary or GPU claim.")


def write_archive(archive, stage):
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as writer:
        for path in sorted(stage.rglob("*")):
            require(not path.is_symlink(), "Staging contains a symlink")
            if path.is_dir():
                continue
            require(path.is_file(), "Staging contains a special file")
            name = valid_path(path.relative_to(stage).as_posix())
            writer.write(path, name)


def build(args, out, report):
    require(platform.system() == "Darwin" and platform.machine() == "x86_64", "Actual macOS Intel build host is required")
    commit = checked(["git", "-C", ROOT, "rev-parse", "HEAD"], report["commands"])
    require(bool(re.fullmatch(r"[0-9a-f]{40}", args.source_commit)) and commit == args.source_commit,
            "Source commit does not identify current checkout")
    checked(["git", "-C", ROOT, "diff", "--quiet", "HEAD", "--"], report["commands"])
    settings = json.loads(args.kext_settings.read_text(encoding="utf-8"))
    targets = [item["buildSettings"] for item in settings if item.get("target") == "Mellow"]
    require(len(targets) == 1, "Expected exactly one actual Mellow Xcode target")
    bundle = Path(targets[0]["TARGET_BUILD_DIR"]) / targets[0]["FULL_PRODUCT_NAME"]
    require(bundle.name == "Mellow.kext", "Unexpected actual kext product")
    with (bundle / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    require(info.get("CFBundleIdentifier") == "com.NiSeullent.Mellow" and info.get("CFBundleExecutable") == "Mellow",
            "Unexpected kext identity")
    require(isinstance(info.get("CFBundleVersion"), str), "Missing actual kext version")
    validation = json.loads(args.kext_validation.read_text(encoding="utf-8"))
    require(validation.get("structural_validation_passed") is True and validation.get("errors") == [] and
            validation.get("sha256") == digest(bundle / "Contents/MacOS/Mellow") and
            validation.get("bundle_version") == info["CFBundleVersion"], "Kext validation receipt does not match actual binary")
    userspace = json.loads((args.userspace_dir / "apple-userspace-build.json").read_text(encoding="utf-8"))
    require(userspace.get("status") == "BUILT_ONLY" and userspace.get("native_compilation") == "PASSED" and
            userspace.get("target", {}).get("architecture") == "x86_64" and
            userspace.get("target", {}).get("deployment_target") == "15.0", "Actual successful userspace compilation receipt required")
    require(userspace.get("source_changed_during_build_or_run") == [] and
            userspace.get("artifact_changed_during_build_or_run") == [] and
            all(value is False for value in userspace.get("requested_execution", {}).values()),
            "Changed/executed userspace build is not the requested build-only snapshot")
    source_hashes = userspace.get("source_sha256", {})
    require(source_hashes, "Userspace receipt has no source snapshot")
    for name, sha in source_hashes.items():
        require(PurePosixPath(name).as_posix() == name and not PurePosixPath(name).is_absolute() and
                all(part not in ("", ".", "..") for part in name.split("/")), "Unsafe source receipt path")
        require(digest(ROOT / name) == sha, "Userspace source differs from compile receipt: " + name)
    for name in APPLE_BINARIES:
        require(userspace.get("artifacts", {}).get(name) == digest(args.userspace_dir / name), "Userspace binary differs from receipt: " + name)
    stage = out / "payload"
    stage.mkdir()
    copy_bundle(bundle, stage / "Mellow.kext")
    for name in APPLE_BINARIES:
        copy_regular(args.userspace_dir / name, stage / "bin" / name)
    for name in ("native-metal-client", "native-metal-abi"):
        copy_regular(args.native_clients_dir / name, stage / "bin" / name)
    copy_regular(args.metal_probe, stage / "bin/metal-probe")
    copy_regular(args.diagnostic_client, stage / "bin/tahoe-diag-client")
    copy_regular(args.kext_validation, stage / "bin/metadata/kext-macho-validation.json")
    copy_regular(args.userspace_dir / "apple-userspace-build.json", stage / "bin/metadata/apple-userspace-build.json")

    framework = stage / "frameworks/MellowAppleUserspace.framework"
    copy_regular(args.userspace_dir / APPLE_BINARIES[0], framework / "MellowAppleUserspace")
    for source in ("Userspace/AppleMetal/MellowAppleMetal.h", "Userspace/AppleMetal/MellowAppleRenderMetal.h",
                   "Userspace/WindowServer/SurfacePresenter.h"):
        copy_regular(ROOT / source, framework / "Headers" / Path(source).name)
    (framework / "Headers/MellowAppleUserspace.h").write_text(
        '#import <MellowAppleUserspace/MellowAppleMetal.h>\n#import <MellowAppleUserspace/MellowAppleRenderMetal.h>\n'
        '#import <MellowAppleUserspace/SurfacePresenter.h>\n', encoding="ascii")
    (framework / "Modules").mkdir()
    (framework / "Modules/module.modulemap").write_text(
        'framework module MellowAppleUserspace {\n  umbrella header "MellowAppleUserspace.h"\n'
        '  export *\n  module * { export * }\n}\n', encoding="ascii")
    (framework / "Resources").mkdir()
    with (framework / "Resources/Info.plist").open("wb") as stream:
        plistlib.dump({"CFBundleIdentifier": "com.NiSeullent.MellowAppleUserspace", "CFBundleName": "MellowAppleUserspace",
                       "CFBundleExecutable": "MellowAppleUserspace", "CFBundlePackageType": "FMWK",
                       "CFBundleVersion": info["CFBundleVersion"], "CFBundleShortVersionString": info["CFBundleVersion"],
                       "CFBundleInfoDictionaryVersion": "6.0", "LSMinimumSystemVersion": "15.0"}, stream)
    checked(["/usr/bin/install_name_tool", "-id", FRAMEWORK_ID, framework / "MellowAppleUserspace"], report["commands"])
    checked(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", framework], report["commands"])
    checked(["/usr/bin/codesign", "--verify", "--strict", framework], report["commands"])

    # Lilu is a retained upstream binary, not a newly compiled project product.
    lilu = ROOT / "Lilu.kext"
    with (lilu / "Contents/Info.plist").open("rb") as stream:
        lilu_info = plistlib.load(stream)
    minimum_lilu = info.get("OSBundleLibraries", {}).get("as.vit9696.Lilu")
    parse_version = lambda value: tuple(int(part) for part in value.split("."))
    require(lilu_info.get("CFBundleIdentifier") == "as.vit9696.Lilu" and lilu_info.get("CFBundleExecutable") == "Lilu" and
            isinstance(minimum_lilu, str) and parse_version(lilu_info["CFBundleVersion"]) >= parse_version(minimum_lilu),
            "Missing/incompatible retained Lilu dependency")
    copy_regular(lilu / "Contents/Info.plist", stage / "dependencies/Lilu.kext/Contents/Info.plist")
    copy_regular(lilu / "Contents/MacOS/Lilu", stage / "dependencies/Lilu.kext/Contents/MacOS/Lilu")
    for source in ("LICENSE", "NOTICE", "Drivers/PortedXe/LICENSE.MIT", "Drivers/PortedNvidia/LICENSE.MIT",
                   "Drivers/PortedNvidiaGsp/LICENSE.MIT"):
        copy_regular(ROOT / source, stage / "licenses" / source)
    copy_bundle(ROOT / "LICENSES", stage / "licenses/LICENSES")

    artifacts = []
    def artifact(path, kind, role):
        identity = macho(stage / path, kind)
        status = signing(stage / path)
        artifacts.append({"path": path, "binary_path": path, "type": role, "identity": identity,
                          "code_signing": status, "compiled_in_this_build": role != "retained-dependency"})
        return identity

    artifact("Mellow.kext/Contents/MacOS/Mellow", 11, "kext")
    dependency_identity = artifact("dependencies/Lilu.kext/Contents/MacOS/Lilu", 11, "retained-dependency")
    library = artifact("bin/libMellowAppleUserspace.dylib", 6, "dylib")
    require(library["install_names"] == [LIBRARY_ID], "Original adjacent dylib ID changed")
    framework_identity = artifact("frameworks/MellowAppleUserspace.framework/MellowAppleUserspace", 6, "framework")
    require(framework_identity["install_names"] == [FRAMEWORK_ID], "Framework copy has incorrect install ID")
    for name in APPLE_BINARIES[1:] + ("native-metal-client", "native-metal-abi", "metal-probe", "tahoe-diag-client"):
        identity = artifact("bin/" + name, 2, "executable")
        require(parse_version(identity["minimum_os"]) <= (15, 0, 0), "CLI minimum OS exceeds package minimum: " + name)
        if name in APPLE_BINARIES[1:4]:
            require(LIBRARY_ID in identity["dependencies"] and "@loader_path" in identity["rpaths"], "Client cannot load its adjacent original dylib")
    files = [{"path": valid_path(p.relative_to(stage).as_posix()), "sha256": digest(p), "size": p.stat().st_size}
             for p in sorted(stage.rglob("*")) if p.is_file()]
    manifest = {"schema_version": 1, "package_name": "Mellow", "asset_name": ASSET, "architecture": "x86_64",
                "minimum_macos": "15.0", "repository": args.repository, "source_commit": commit,
                "source_ref": args.source_ref, "release_tag": args.release_tag, "version": info["CFBundleVersion"],
                "source_url": "https://github.com/" + args.repository + "/tree/" + commit,
                "experimental": True, "binary_compiled": True, "signed": False, "developer_id_signed": False,
                "notarized": False, "code_signing": "mixed-unsigned-and-ad-hoc", "hardware_support_verified": False,
                "system_metal_registered": False, "windowserver_acceleration_verified": False,
                "build": {"status": "BUILT_ONLY", "created_utc": datetime.now(timezone.utc).isoformat(),
                          "host": {"system": platform.system(), "machine": platform.machine(), "macos_version": platform.mac_ver()[0]},
                          "sdk": userspace["sdk"], "compiler": userspace["compiler"],
                          "workflow": os.environ.get("GITHUB_WORKFLOW"), "run_id": os.environ.get("GITHUB_RUN_ID"),
                          "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"), "gpu_clients_executed": False,
                          "kext_loaded": False, "native_client_build": "Tools/NativeMetalClient.mk"},
                "dependencies": [{"identifier": "as.vit9696.Lilu", "minimum_version": minimum_lilu,
                                  "version": lilu_info["CFBundleVersion"], "path": "dependencies/Lilu.kext",
                                  "provenance": "retained repository binary; not compiled in this CI build",
                                  "identity": dependency_identity}], "artifacts": artifacts, "files": files}
    write_json(stage / "manifest.json", manifest)
    hashes = [f"{digest(p)}  {p.relative_to(stage).as_posix()}" for p in sorted(stage.rglob("*")) if p.is_file()]
    (stage / "SHA256SUMS").write_text("\n".join(hashes) + "\n", encoding="ascii")
    archive = out / ASSET
    write_archive(archive, stage)
    verify_archive(archive, stage)
    copy_regular(stage / "manifest.json", out / "manifest.json")
    (out / (ASSET + ".sha256")).write_text(digest(archive) + "  " + ASSET + "\n", encoding="ascii")
    for name in ("install-mellow.sh", "uninstall-mellow.sh"):
        copy_regular(ROOT / "packaging" / name, out / name)
    report.update(status="PACKAGED_BUILD_ONLY", asset_name=ASSET, archive_sha256=digest(archive),
                  binary_count=len(artifacts), hardware_support_verified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    for name in ("kext-settings", "kext-validation", "userspace-dir", "native-clients-dir", "metal-probe", "diagnostic-client", "out"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--source-commit")
    parser.add_argument("--source-ref")
    parser.add_argument("--release-tag")
    parser.add_argument("--repository", default="NiSeullent/Mellow")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    for name in ("kext_settings", "kext_validation", "userspace_dir", "native_clients_dir", "metal_probe", "diagnostic_client", "out", "source_commit", "source_ref"):
        if getattr(args, name) is None:
            parser.error("Missing --" + name.replace("_", "-"))
    require(bool(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository)), "Invalid repository name")
    out = args.out.resolve()
    require(not args.out.is_symlink() and not out.is_relative_to(ROOT) and not ROOT.is_relative_to(out),
            "Release output must be outside the source tree")
    require(not out.exists() or out.is_dir() and not any(out.iterdir()), "Release output must be new or empty")
    out.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "status": "FAILED", "commands": []}
    code = 1
    try:
        build(args, out, report)
        code = 0
    except (OSError, ValueError, KeyError, TypeError, struct.error) as failure:
        report["reason"] = str(failure)
        # An unsuccessful packaging run never leaves a downloadable ZIP.
        for name in (ASSET, ASSET + ".sha256", "manifest.json", "install-mellow.sh", "uninstall-mellow.sh"):
            (out / name).unlink(missing_ok=True)
    finally:
        write_json(out / "release-build.json", report)
        print(json.dumps({"status": report["status"], "report": str(out / "release-build.json")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
