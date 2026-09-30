// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
// CI-only actual-payload acceptance. No payload executable is ever run.
import Foundation
import Darwin

@main struct NativePayloadAcceptance {
    static func main() {
        do {
            let args = Array(CommandLine.arguments.dropFirst())
            try require(args.count == 5, "Expected archive, SHA256, release, target major and source commit")
            guard let major = Int(args[3]) else { throw InstallError.invalid("Invalid target") }
            try require([15,26].contains(major) && ProcessInfo.processInfo.operatingSystemVersion.majorVersion == 15 && geteuid() != 0,
                        "Acceptance is an isolated nonroot macOS 15 host test")
            let raw = try snapshotFile(args[0])
            let package = try checkPackage(raw, sha256: args[1], release: args[2], major: major, expectedCommit: args[4])
            let temporary = FileManager.default.temporaryDirectory.resolvingSymlinksInPath()
                .appendingPathComponent("mellow-real-payload-" + UUID().uuidString.lowercased(), isDirectory: true)
            try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            defer { try? FileManager.default.removeItem(at: temporary) }
            let store = try InstallStore(prefix: temporary.appendingPathComponent("store").path, create: true)
            try store.install(package, verification: "offline-user-checksum")
            let installed = try store.report("real-payload-test")
            try require(installed.current == args[2] && installed.releases.count == 1 &&
                        installed.releases[0].sourceCommit == args[4] && !installed.activationPerformed &&
                        installed.capabilities.isDevelopment, "Real installed payload differs")
            let (_, checked, _) = try store.checked(args[2])
            try require(checked.files.count == package.files.count && checked.files.allSatisfy { name, data in package.files[name] == data },
                        "Actual installed bytes differ from the original native archive")
            var duplicateRejected = false
            do { try store.install(package, verification: "offline-user-checksum") }
            catch { duplicateRejected = true }
            try require(duplicateRejected && store.state.current == args[2], "Actual release overwrite was not rejected")
            try store.uninstall(args[2])
            let removed = try store.report("real-payload-uninstall")
            try require(removed.releases.isEmpty && removed.current == nil && removed.previous == nil,
                        "Actual native package was not completely removed")
            let proof: [String: Any] = [
                "schemaVersion":1, "status":"PASSED", "sourceCommit":args[4], "release":args[2],
                "archiveSHA256":args[1], "packageTargetOSMajor":major,
                "hostOSVersion":ProcessInfo.processInfo.operatingSystemVersionString,
                "actualNativePayloadFiles":package.files.count,
                "actualPackageParsed":true, "temporaryInstallationVerified":true,
                "installedBytesVerified":true, "duplicateInstallRejected":true, "ownedUninstallVerified":true,
                "payloadBinaryExecuted":false, "systemInstallationPerformed":false,
                "macOS26RuntimeVerified":false, "physicalGPUVerified":false,
                "systemMetalRegistered":false, "windowServerAccelerationVerified":false,
                "scope":"Actual native kext/framework files parsed, installed, rehashed and removed in an isolated temporary directory on host macOS 15. No downloaded executable or driver is run."
            ]
            let result = try JSONSerialization.data(withJSONObject: proof, options: [.prettyPrinted,.sortedKeys])
            FileHandle.standardOutput.write(result); FileHandle.standardOutput.write(Data([10]))
        } catch {
            FileHandle.standardError.write(Data(("Actual-payload acceptance failed: " + String(describing: error) + "\n").utf8))
            exit(1)
        }
    }
}
