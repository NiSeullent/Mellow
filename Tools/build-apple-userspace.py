#!/usr/bin/env python3
"""Build native x86_64 Mellow userspace; run GPU/window clients only with explicit flags."""
import argparse
import ctypes
from datetime import datetime, timezone
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP_SOURCES = (
    "Runtime/PlatformRuntime.cpp", "Runtime/OpenCLProvider.cpp",
    "Runtime/ShaderJit.cpp", "Runtime/AirDecoder.cpp", "Runtime/MetalObjects.cpp",
    "Runtime/OpenGLProvider.cpp", "Runtime/RenderShaderJit.cpp", "Runtime/RenderObjects.cpp",
    "Userspace/WindowServer/SurfaceSnapshot.cpp",
)
OBJC_SOURCES = (
    "Userspace/AppleMetal/MellowAppleMetal.mm",
    "Userspace/AppleMetal/MellowAppleRenderMetal.mm",
    "Userspace/WindowServer/SurfacePresenter.mm",
)
CLIENTS = {
    "compute": ("Userspace/AppleMetal/compute-acceptance.mm", "mellow-compute-acceptance"),
    "metal-render": ("Userspace/AppleMetal/render-acceptance.mm", "mellow-metal-render-acceptance"),
    "render": ("Userspace/WindowServer/Acceptance.mm", "mellow-windowserver-acceptance"),
}
INVENTORY_SOURCE = "Userspace/Diagnostics/MetalInventory.mm"
FRAMEWORKS = (
    "Foundation", "Metal", "OpenGL", "IOSurface", "CoreFoundation",
    "QuartzCore", "CoreGraphics", "AppKit", "IOKit",
)
MAX_REPORT_BYTES = 1024 * 1024


class NotAvailable(RuntimeError):
    """Native tools or a requested execution environment are absent."""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def strict_json(value):
    def pairs(items):
        result = {}
        for key, item in items:
            if key in result:
                raise ValueError("Duplicate JSON key: " + key)
            result[key] = item
        return result

    def invalid(value):
        raise ValueError("Non-finite JSON number: " + value)

    return json.loads(value, object_pairs_hook=pairs, parse_constant=invalid)


def version_tuple(value):
    pieces = value.strip().split(".")
    if not pieces or not all(piece.isdecimal() for piece in pieces):
        raise ValueError("Invalid macOS/SDK version: " + value)
    return tuple(int(piece) for piece in (pieces + ["0", "0"])[:3])


def command(report, label, arguments, timeout, env):
    item = {"name": label, "command": [str(value) for value in arguments]}
    report["commands"].append(item)
    try:
        result = subprocess.run(item["command"], cwd=ROOT, env=env,
                                capture_output=True, text=True, timeout=timeout)
        item.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
        return result
    except subprocess.TimeoutExpired as failure:
        item.update(exit_code=None, timed_out=True)
        for key in ("stdout", "stderr"):
            value = getattr(failure, key, None)
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            item[key] = value or ""
        raise RuntimeError(label + " exceeded its timeout") from failure


def checked(report, label, arguments, env):
    result = command(report, label, arguments, 120, env)
    if result.returncode:
        raise RuntimeError(label + " failed; inspect the recorded command diagnostics")
    return result.stdout.strip()


def native_tools(report, env, deployment):
    if sys.platform != "darwin":
        raise NotAvailable("Native macOS with an existing SDK is required; no SDK download or cross-build is attempted")
    xcrun = shutil.which("xcrun")
    if not xcrun:
        raise NotAvailable("xcrun is unavailable; no developer tools installation is attempted")
    try:
        sdk = Path(checked(report, "discover SDK", [xcrun, "--sdk", "macosx", "--show-sdk-path"], env))
        sdk_version = checked(report, "discover SDK version", [xcrun, "--sdk", "macosx", "--show-sdk-version"], env)
        compiler = Path(checked(report, "discover compiler", [xcrun, "--sdk", "macosx", "--find", "clang++"], env))
    except RuntimeError as failure:
        raise NotAvailable("Existing native developer tools could not provide a macOS SDK: " + str(failure)) from failure
    if not sdk.is_dir() or not compiler.is_file():
        raise NotAvailable("xcrun returned a missing SDK or compiler")
    if version_tuple(sdk_version) < version_tuple(deployment):
        raise NotAvailable("The selected SDK is older than the requested deployment target")
    missing = [name for name in FRAMEWORKS if not (sdk / "System/Library/Frameworks" / (name + ".framework")).is_dir()]
    if missing:
        raise NotAvailable("The selected SDK lacks required frameworks: " + ", ".join(missing))
    report["sdk"] = {"path": str(sdk.resolve()), "version": sdk_version}
    report["compiler"] = {"path": str(compiler.resolve()),
                          "version": checked(report, "compiler version", [compiler, "--version"], env)}
    return compiler, sdk


def object_name(source):
    return source.replace("/", "_").replace(".", "_") + ".o"


def binary_identity(path, deployment, sdk_version, role):
    """Read linked Mach-O metadata rather than treating command flags as evidence."""
    # Public layout: Apple's cctools include/mach-o/loader.h and
    # xnu osfmk/mach/machine.h. Only the requested thin little-endian image is
    # accepted; no architecture slice or build version is inferred from the host.
    with path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32:
            raise ValueError("Truncated Mach-O header: " + path.name)
        magic, cpu, subtype, kind, count, size, flags, _ = struct.unpack("<8I", header)
        if magic != 0xfeedfacf or cpu != 0x01000007 or kind != (6 if role == "library" else 2):
            raise ValueError("Unexpected linked Mach-O architecture/type: " + path.name)
        if not 0 < count <= 4096 or not count * 8 <= size <= 2 * 1024 * 1024:
            raise ValueError("Invalid Mach-O load-command bounds: " + path.name)
        data = stream.read(size)
        if len(data) != size:
            raise ValueError("Truncated Mach-O load commands: " + path.name)
    versions, install_names, dependencies, rpaths = [], [], [], []
    at = 0
    for _ in range(count):
        if at + 8 > size:
            raise ValueError("Missing Mach-O load command: " + path.name)
        command_kind, command_size = struct.unpack_from("<2I", data, at)
        if command_size < 8 or command_size % 8 or at + command_size > size:
            raise ValueError("Invalid Mach-O load-command size: " + path.name)
        command_data = data[at:at + command_size]
        if command_kind == 0x32:  # LC_BUILD_VERSION
            if command_size < 24:
                raise ValueError("Short Mach-O build-version command")
            _, _, platform_id, minimum, sdk, tools = struct.unpack_from("<6I", command_data)
            if platform_id != 1 or command_size != 24 + tools * 8:
                raise ValueError("Invalid/non-macOS Mach-O build version")
            versions.append((minimum, sdk))
        elif command_kind == 0x24:  # LC_VERSION_MIN_MACOSX
            if command_size != 16:
                raise ValueError("Invalid Mach-O minimum-version command")
            versions.append(struct.unpack_from("<2I", command_data, 8))
        elif command_kind in (0xc, 0xd, 0x80000018, 0x8000001f, 0x80000023, 0x8000001c):
            minimum_size = 12 if command_kind == 0x8000001c else 24
            if command_size < minimum_size:
                raise ValueError("Short Mach-O library/path command")
            string_at, = struct.unpack_from("<I", command_data, 8)
            string_end = command_data.find(b"\0", string_at)
            if not minimum_size <= string_at < command_size or string_end < 0:
                raise ValueError("Invalid Mach-O library/path string")
            value = command_data[string_at:string_end].decode("utf-8")
            (rpaths if command_kind == 0x8000001c else
             install_names if command_kind == 0xd else dependencies).append(value)
        at += command_size
    if at != size or len(versions) != 1:
        raise ValueError("Missing/ambiguous Mach-O macOS version: " + path.name)

    def unpack_version(value):
        return (value >> 16, (value >> 8) & 255, value & 255)

    minimum, sdk = [unpack_version(value) for value in versions[0]]
    if minimum != version_tuple(deployment) or sdk != version_tuple(sdk_version):
        raise ValueError("Linked Mach-O deployment/SDK differs from selected native tools: " + path.name)
    library_name = "@rpath/libMellowAppleUserspace.dylib"
    if role == "library" and install_names != [library_name]:
        raise ValueError("Unexpected userspace dylib install name")
    if role == "client" and (library_name not in dependencies or "@loader_path" not in rpaths):
        raise ValueError("Client does not load the adjacent Mellow userspace dylib")
    if role == "inventory" and library_name in dependencies:
        raise ValueError("Read-only inventory unexpectedly depends on the Mellow adapter")
    return {"architecture": "x86_64", "cpu_subtype": subtype, "platform": "macOS",
            "file_type": "MH_DYLIB" if kind == 6 else "MH_EXECUTE", "flags": flags,
            "minimum_os": ".".join(map(str, minimum)), "sdk": ".".join(map(str, sdk)),
            "install_names": install_names, "dependencies": dependencies, "rpaths": rpaths}


def build(report, out, compiler, sdk, env, deployment):
    headers = sorted(path.relative_to(ROOT).as_posix() for path in (ROOT / "Runtime").glob("*.hpp"))
    headers += ["Userspace/AppleMetal/MellowAppleMetal.h", "Userspace/AppleMetal/MellowAppleRenderMetal.h",
                "Userspace/AppleMetal/RenderPassLimits.hpp",
                "Userspace/WindowServer/SurfacePresenter.h",
                "Userspace/WindowServer/SurfaceSnapshot.hpp", "Userspace/WindowServer/RenderFixtureOracle.hpp", "tests/render_fixture.hpp",
                "tests/opencl_runtime_sha256.hpp", "Tools/build-apple-userspace.py"]
    sources = [*CPP_SOURCES, *OBJC_SOURCES, *[entry[0] for entry in CLIENTS.values()], INVENTORY_SOURCE]
    for name in sources + headers:
        if not (ROOT / name).is_file():
            raise RuntimeError("Required real source is absent: " + name)
    report["source_sha256"] = {name: digest(ROOT / name) for name in sources + headers}
    base = [str(compiler), "-std=c++17", "-O2", "-arch", "x86_64", "-isysroot", str(sdk),
            "-mmacosx-version-min=" + deployment, "-Wall", "-Wextra", "-Werror=return-type",
            "-pthread", "-I", str(ROOT)]
    objects = out / "objects"
    objects.mkdir()

    def record_binary(path, role):
        report.setdefault("binary_identity", {})[path.name] = binary_identity(
            path, deployment, report["sdk"]["version"], role)
        report["artifacts"][path.name] = digest(path)

    def compile_source(source):
        target = objects / object_name(source)
        arguments = list(base)
        # ARC and Objective-C blocks are applied only to actual Objective-C++
        # translation units; production C++ sources retain their own semantics.
        if source.endswith(".mm"):
            arguments += ["-fobjc-arc", "-fblocks"]
        arguments += ["-c", str(ROOT / source), "-o", str(target)]
        checked(report, "compile " + source, arguments, env)
        report["artifacts"][target.relative_to(out).as_posix()] = digest(target)
        return target

    library_objects = [compile_source(source) for source in (*CPP_SOURCES, *OBJC_SOURCES)]
    framework_flags = [flag for name in FRAMEWORKS for flag in ("-framework", name)]
    library = out / "libMellowAppleUserspace.dylib"
    checked(report, "link userspace library", [*base, "-dynamiclib", *library_objects,
            "-Wl,-install_name,@rpath/" + library.name, *framework_flags, "-o", library], env)
    record_binary(library, "library")
    for name, (source, filename) in CLIENTS.items():
        client_object = compile_source(source)
        target = out / filename
        checked(report, "link " + name + " acceptance", [*base, client_object,
                "-L", out, "-lMellowAppleUserspace", "-Wl,-rpath,@loader_path",
                *framework_flags, "-o", target], env)
        record_binary(target, "client")
    inventory_object = compile_source(INVENTORY_SOURCE)
    inventory = out / "metal-inventory"
    inventory_frameworks = [flag for name in ("Foundation", "Metal", "IOKit", "CoreGraphics")
                            for flag in ("-framework", name)]
    checked(report, "link native Metal inventory", [*base, inventory_object, *inventory_frameworks,
            "-o", inventory], env)
    record_binary(inventory, "inventory")
    report["native_compilation"] = "PASSED"


def read_client_report(path):
    if not path.is_file() or path.stat().st_size > MAX_REPORT_BYTES:
        raise ValueError("Acceptance client did not produce a bounded current JSON report")
    result = strict_json(path.read_text(encoding="utf-8"))
    if type(result) is not dict:
        raise ValueError("Acceptance report must be a JSON object")
    return result


def execution_host(report):
    if sys.platform != "darwin" or platform.machine() != "x86_64" or version_tuple(platform.mac_ver()[0])[0] not in (15, 26):
        raise NotAvailable("Requested GPU/window execution needs a native x86_64 macOS 15 or 26 process")
    if version_tuple(platform.mac_ver()[0]) < version_tuple(report["target"]["deployment_target"]):
        raise NotAvailable("The current macOS is older than the linked acceptance binaries' minimum deployment target")
    try:
        # Query this Python process. A separately launched universal sysctl tool
        # could run its ARM slice and miss translation of the current process.
        sysctl = ctypes.CDLL(None, use_errno=True).sysctlbyname
    except (AttributeError, OSError) as failure:
        raise NotAvailable("Cannot determine native execution/translation context") from failure
    sysctl.argtypes = (ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                       ctypes.c_void_p, ctypes.c_size_t)
    sysctl.restype = ctypes.c_int
    values = {}
    for name in ("sysctl.proc_translated", "hw.optional.arm64"):
        value = ctypes.c_int(0)
        length = ctypes.c_size_t(ctypes.sizeof(value))
        ctypes.set_errno(0)
        status = sysctl(name.encode("ascii"), ctypes.byref(value), ctypes.byref(length), None, 0)
        native_errno = ctypes.get_errno() if status else 0
        values[name] = {"status": status, "errno": native_errno,
                        "value": value.value if not status else None}
        if status and native_errno != errno.ENOENT:
            report["execution_host"] = values
            raise NotAvailable("Native architecture query failed: " + name)
        if not status and (length.value != ctypes.sizeof(value) or value.value < 0):
            report["execution_host"] = values
            raise NotAvailable("Invalid native architecture query: " + name)
    # Apple documents ENOENT as a native-process result for proc_translated.
    translated = values["sysctl.proc_translated"]["value"] or 0
    arm_capable = values["hw.optional.arm64"]["value"] or 0
    values.update(native_x86_64_process=not translated and not arm_capable,
                  physical_host_identity_verified=False)
    report["execution_host"] = values
    if translated or arm_capable:
        raise NotAvailable("Rosetta/Apple Silicon cannot provide physical x86_64 acceptance")


def validate_receipt(name, native, args):
    errors = []

    def need(condition, message):
        if not condition:
            errors.append(message)

    if name == "compute":
        need(native.get("status") == "PASSED", "Compute status must be PASSED")
        for key in ("independentReadbackVerified", "gpuEventOwnershipVerified", "gpuProfilingVerified",
                    "gpuResourcesReleased", "retainedResourceLifetimeVerified",
                    "completionCallbackAfterReadbackVerified", "sameWorkerWaitRejected"):
            need(native.get(key) is True, key + " must be true")
        need(type(native.get("openCLGPUIndex")) is int and native["openCLGPUIndex"] == args.opencl_gpu_index,
             "Compute GPU selection mismatch")
        need(native.get("providerInitializationStatus") == "ready" and native.get("bootstrapSubmissionAttempted") is True,
             "Validated native compute bootstrap receipt missing")
        for key, value in (("dispatchesVerified", 2), ("completionHandlersVerified", 1)):
            need(type(native.get(key)) is int and native[key] == value, key + " mismatch")
        executions = native.get("executions")
        need(type(executions) is list and len(executions) == 2 and all(type(item) is dict for item in executions),
             "Two actual production execution receipts are required")
        previous_sequence = previous_end = 0
        epoch = None
        if type(executions) is list:
            for event in executions:
                if type(event) is not dict:
                    continue
                for key in ("submitted", "submissionAttempted", "executionCompleted", "runtimePlanned",
                            "eventOwnershipVerified", "profilingVerified", "resourcesReleased"):
                    need(event.get(key) is True, "Execution " + key + " must be true")
                for key in ("resultsVerified", "runtimeCompletionAccepted"):
                    need(event.get(key) is False, "Runtime cannot substitute for independent readback validation")
                sequence, start, end, current_epoch = [event.get(key) for key in ("sequence", "gpuStart", "gpuEnd", "epoch")]
                valid = all(type(value) is int for value in (sequence, start, end, current_epoch))
                need(valid and sequence > previous_sequence and start >= previous_end and end > start and
                     current_epoch > 0 and (epoch is None or current_epoch == epoch), "Invalid/uncorrelated GPU execution interval")
                if valid:
                    previous_sequence, previous_end, epoch = sequence, end, current_epoch
        initial, expected, actual = [native.get(key) for key in ("input", "independentExpected", "actualReadback")]
        valid_vectors = all(type(vector) is list and len(vector) == 256 and
                            all(type(word) is int and 0 <= word <= 0xffffffff for word in vector)
                            for vector in (initial, expected, actual))
        need(valid_vectors, "Complete uint32 challenge/readback vectors are required")
        if valid_vectors:
            oracle = [((((word * 7 + 3) & 0xffffffff) * 7 + 3) & 0xffffffff) for word in initial]
            need(actual == expected == oracle and actual != initial, "Independent two-dispatch uint32 result mismatch")
        for key in ("systemMetalRegistered", "windowServerIntegrated", "fullMetalProtocolConformance",
                    "physicalPCIIdentityVerified"):
            need(native.get(key) is False, key + " must remain false for this explicit compute subset")
    elif name == "metal-render":
        need(native.get("status") == "PASSED" and native.get("acceptance") == "apple-opt-in-render",
             "Metal-selector render status/scope mismatch")
        need(native.get("providerInitializationStatus") == "ready", "Render provider must be ready")
        need(native.get("bootstrapSubmissionAttempted") is False and native.get("renderSubmissionAttempted") is True,
             "Actual render submission attempt receipt missing")
        for key in ("independentReadbackVerified", "gpuFenceVerified", "gpuResourcesReleased",
                    "ioSurfaceWritten", "retainedResourceLifetimeVerified"):
            need(native.get(key) is True, key + " must be true")
        for key, value in (("framesVerified", 2), ("completionHandlersVerified", 2), ("verifiedPixels", 2 * 64 * 48)):
            need(type(native.get(key)) is int and native[key] == value, key + " mismatch")
        for key in ("systemMetalRegistered", "windowServerIntegrated", "displayScanoutVerified",
                    "fullMetalProtocolConformance", "physicalPCIIdentityVerified", "presentRequested",
                    "applicationSnapshotQueued"):
            need(native.get(key) is False, key + " must remain false for offscreen selector acceptance")
        need(native.get("snapshotAlphaMode") == "straight", "Render snapshot alpha mismatch")
        capabilities = native.get("adapterCapabilities")
        need(type(capabilities) is dict, "Actual render adapter capabilities missing")
        if type(capabilities) is dict:
            need(capabilities.get("provider") == "existing-host-cgl-gpu", "Unexpected render provider")
            for key in ("acceleratedPixelFormat", "softwareRendererRejected", "coreProfile"):
                need(capabilities.get(key) is True, "Render provider " + key + " must be true")
            for key in ("computeInterop", "fullMetalProtocolConformance", "metalFamilyAdvertised",
                        "systemMetalRegistered", "windowServerIntegrated", "displayScanoutVerified",
                        "physicalPCIIdentityVerified"):
                need(capabilities.get(key) is False, "Render provider " + key + " must remain false")
        executions = native.get("executions")
        need(type(executions) is list and len(executions) == 2, "Two actual GPU render frame receipts required")
        previous_sequence = 0
        epoch = None
        if type(executions) is list:
            for frame in executions:
                if type(frame) is not dict:
                    errors.append("Invalid actual render frame receipt")
                    continue
                for key in ("renderSubmitted", "fenceSignaled", "readbackCompleted", "resourcesReleased", "ioSurfaceWritten"):
                    need(frame.get(key) is True, "Render frame " + key + " must be true")
                for key in ("swapCompleted", "displayScanoutVerified"):
                    need(frame.get(key) is False, "Offscreen frame " + key + " must remain false")
                for key, value in (("width", 64), ("height", 48), ("verifiedPixels", 64 * 48)):
                    need(type(frame.get(key)) is int and frame[key] == value, "Render frame " + key + " mismatch")
                need(type(frame.get("foregroundPixels")) is int and 500 <= frame["foregroundPixels"] <= 64 * 48,
                     "Independent foreground coverage missing")
                sequence, current_epoch, current_surface = [frame.get(key) for key in ("sequence", "epoch", "ioSurfaceID")]
                valid = all(type(value) is int and value > 0 for value in (sequence, current_epoch, current_surface))
                need(valid and sequence > previous_sequence and (epoch is None or current_epoch == epoch),
                     "Uncorrelated render frame sequence/session or invalid surface identity")
                if valid:
                    previous_sequence, epoch = sequence, current_epoch
    elif name == "render":
        need(native.get("status") == "PASS" and native.get("passed") is True, "Render status must be PASS")
        need(native.get("schema_version") == 1 and native.get("scope") == "application-surface-presentation",
             "Render schema/scope mismatch")
        for key in ("gpu_render_submitted", "gpu_fence_signaled", "gpu_readback_completed",
                    "gpu_resources_released", "iosurface_gpu_written", "surface_id_correlated",
                    "snapshot_rgba_matches_gpu_readback", "calayer_transaction_commit_called", "gpu_output_values_verified"):
            need(native.get(key) is True, key + " must be true")
        for key, value in (("requested_frames", args.frames), ("frames_completed", args.frames),
                           ("interval_ms", args.interval_ms), ("snapshot_bytes_compared", args.frames * 64 * 48 * 4)):
            need(type(native.get(key)) is int and native[key] == value, key + " mismatch")
        need(type(native.get("independent_pixels_verified")) is int and
             native["independent_pixels_verified"] == args.frames * 64 * 48,
             "Independent GPU pixel validation count mismatch")
        need(native.get("native_macos_execution") is True, "Native macOS execution receipt missing")
        need(native.get("snapshot_alpha_mode") == "straight", "Presentation snapshot alpha mismatch")
        for key in ("apple_metal_abi_registered", "windowserver_acceleration_verified", "display_scanout_verified",
                    "physical_pci_identity_verified", "cpu_render_fallback"):
            need(native.get(key) is False, key + " must remain false")
        samples = native.get("samples")
        need(type(samples) is list and len(samples) == 2, "First/last render sample receipts are required")
        if type(samples) is list and len(samples) == 2:
            for sample, frame_index in zip(samples, (0, args.frames - 1)):
                if type(sample) is not dict:
                    errors.append("Invalid render sample")
                    continue
                need(type(sample.get("frame")) is int and sample["frame"] == frame_index, "Sample frame mismatch")
                sha = sample.get("gpu_rgba_sha256")
                need(type(sha) is str and len(sha) == 64 and all(c in "0123456789abcdef" for c in sha),
                     "Invalid actual GPU readback digest")
                receipt = sample.get("receipt")
                need(type(receipt) is dict and receipt.get("snapshotCreated") is True and
                     receipt.get("transactionCommitCalled") is True, "Snapshot/layer receipt missing")
    else:
        errors.append("Unknown acceptance client")
    return errors


def run_clients(args, report, out, env):
    # x86_64 compile success on an Apple Silicon host is not physical x86_64
    # acceptance. The clients must run on the requested native target platform.
    execution_host(report)
    for name, requested in (("compute", args.run_compute), ("metal-render", args.run_metal_render), ("render", args.run_render)):
        if not requested:
            continue
        executable = out / CLIENTS[name][1]
        if digest(executable) != report["artifacts"][executable.name] or digest(out / "libMellowAppleUserspace.dylib") != report["artifacts"]["libMellowAppleUserspace.dylib"]:
            raise RuntimeError("Acceptance binary/library changed after native compilation")
        result_path = out / (name + "-acceptance.json")
        if name == "render":
            arguments = [executable, "--present", "--report", result_path,
                         "--frames", str(args.frames), "--interval-ms", str(args.interval_ms)]
        else:
            arguments = [executable, str(args.opencl_gpu_index)] if name == "compute" else [executable]
        report["acceptance"][name] = {"status": "STARTED", "gpu_execution": "UNKNOWN"}
        result = command(report, "run " + name + " acceptance", arguments, args.timeout, env)
        if name != "render":
            if len(result.stdout.encode("utf-8")) > MAX_REPORT_BYTES:
                raise ValueError(name + " acceptance stdout exceeds its report bound")
            result_path.write_text(result.stdout, encoding="utf-8")
        native = read_client_report(result_path)
        item = {"status": "FAILED", "exit_code": result.returncode, "native_report": native,
                "native_report_sha256": digest(result_path), "gpu_execution": "UNKNOWN"}
        report["acceptance"][name] = item
        if result.returncode == 77 and native.get("status") == "NOT_RUN":
            unattempted = native.get("gpu_execution_attempted") is False if name == "render" else (
                native.get("bootstrapSubmissionAttempted") is False and
                native.get("providerInitializationStatus") == "unavailable" and
                (name == "compute" or native.get("renderSubmissionAttempted") is False))
            if not unattempted:
                raise RuntimeError(name + " NOT_RUN receipt lacks explicit unattempted GPU evidence")
            item.update(status="NOT_RUN", gpu_execution="NOT_RUN")
            raise NotAvailable(name + " client reports no usable native GPU/window environment")
        # Native clients own their independent result checks. Accept only an
        # explicit successful receipt; compilation or exit zero alone is not one.
        item["validation_errors"] = validate_receipt(name, native, args)
        if result.returncode or item["validation_errors"]:
            raise RuntimeError(name + " acceptance receipt failed independent packaging checks")
        item["status"] = "PASSED_LIMITED_SCOPE"
        item["gpu_execution"] = "CLIENT_REPORTED"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="New/empty output directory outside the source tree")
    parser.add_argument("--deployment-target", choices=("15.0", "26.0"), default="15.0")
    parser.add_argument("--run-compute", action="store_true", help="Explicitly execute the real native compute client")
    parser.add_argument("--run-metal-render", action="store_true", help="Explicitly execute the native Metal-selector offscreen render client")
    parser.add_argument("--run-render", action="store_true", help="Explicitly create the render client's AppKit window")
    parser.add_argument("--opencl-gpu-index", type=int, default=0)
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--interval-ms", type=int, default=33)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    if not 0 <= args.opencl_gpu_index <= 65535 or not 2 <= args.frames <= 600:
        parser.error("GPU index must be 0..65535 and frames 2..600")
    if not 1 <= args.interval_ms <= 1000 or not 1 <= args.timeout <= 180:
        parser.error("Interval must be 1..1000 ms and timeout 1..180 seconds")
    if args.run_render and args.frames * args.interval_ms > 30000:
        parser.error("The native render client's scheduled display time may not exceed 30 seconds")
    if args.run_render and args.frames * args.interval_ms >= args.timeout * 1000:
        parser.error("Render scheduling duration must be below the external worker timeout")
    out = args.out.resolve()
    if args.out.is_symlink() or out.is_relative_to(ROOT) or ROOT.is_relative_to(out):
        parser.error("Output may not be a symlink or overlap the source tree")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error("Output directory must be new or empty")
    out.mkdir(parents=True, exist_ok=True)
    report = {
        "schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
        "status": "NOT_RUN", "native_compilation": "NOT_RUN",
        "host": {"system": platform.system(), "release": platform.release(),
                 "machine": platform.machine(), "macos_version": platform.mac_ver()[0]},
        "target": {"architecture": "x86_64", "deployment_target": args.deployment_target,
                   "requested_os_majors": [15, 26]},
        "requested_execution": {"compute": args.run_compute, "metal-render": args.run_metal_render, "render": args.run_render},
        "acceptance": {name: {"status": "NOT_RUN", "gpu_execution": "NOT_RUN"} for name in CLIENTS},
        "system_metal_registered": False, "system_windowserver_acceleration_verified": False,
        "all_gpu_models_verified": False, "physical_scanout_verified": False,
        "scope": "Explicit Mellow compute/render selector subsets and app IOSurface/CoreAnimation presentation; no system driver registration",
        "commands": [], "artifacts": {},
    }
    result_code = 1
    env = os.environ.copy()
    env["MACOSX_DEPLOYMENT_TARGET"] = args.deployment_target
    try:
        compiler, sdk = native_tools(report, env, args.deployment_target)
        build(report, out, compiler, sdk, env, args.deployment_target)
        report["status"] = "BUILT_ONLY"
        if args.run_compute or args.run_metal_render or args.run_render:
            run_clients(args, report, out, env)
            report["status"] = "PASSED_LIMITED_APP_SCOPE"
        changed = [name for name, sha in report["source_sha256"].items() if not (ROOT / name).is_file() or digest(ROOT / name) != sha]
        report["source_changed_during_build_or_run"] = changed
        if changed:
            raise RuntimeError("Source changed during build/execution; artifacts cannot be attributed to the initial snapshot")
        changed_artifacts = [name for name, sha in report["artifacts"].items()
                             if not (out / name).is_file() or digest(out / name) != sha]
        report["artifact_changed_during_build_or_run"] = changed_artifacts
        if changed_artifacts:
            raise RuntimeError("Compiled artifacts changed during build/execution; receipts cannot be attributed to recorded binaries")
        result_code = 0
    except NotAvailable as failure:
        partial = any(item.get("status") == "PASSED_LIMITED_SCOPE" for item in report["acceptance"].values())
        report.update(status="PARTIAL_UNAVAILABLE" if partial else "NOT_RUN", reason=str(failure))
        result_code = 77
    except (OSError, ValueError, RuntimeError) as failure:
        report.update(status="FAILED", reason=str(failure))
        result_code = 1
    finally:
        report_path = out / "apple-userspace-build.json"
        report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(json.dumps({"status": report["status"], "native_compilation": report["native_compilation"],
                          "report": str(report_path), "system_metal_registered": False,
                          "system_windowserver_acceleration_verified": False}))
    return result_code


if __name__ == "__main__":
    raise SystemExit(main())
