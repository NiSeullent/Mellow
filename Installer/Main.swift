// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
import Foundation
import Darwin

let usage = """
Mellow development binary installer (native Intel macOS 15 or 26)
Usage:
  mellow-installer install --release TAG [--prefix ABSOLUTE_PATH]
  mellow-installer install --release TAG --archive PATH --sha256 HEX [--prefix ABSOLUTE_PATH]
  mellow-installer status|list [--prefix ABSOLUTE_PATH]
  mellow-installer rollback|uninstall --release TAG [--prefix ABSOLUTE_PATH]
  mellow-installer --self-test
  mellow-installer --help

Default prefix: ~/Library/Application Support/Mellow
Installs unsigned/signed development bundles into owned version directories only.
The kext is a diagnostic Intel 8086:7D41 profile; acceleration is unverified.
The framework is for applications that explicitly use it. System Metal and
WindowServer acceleration are not registered or verified by this installer.
No kext activation, EFI/config.plist change, SIP change, reboot or sudo is used.
"""
func hostMajor() throws -> Int {
    #if !arch(x86_64)
    throw InstallError.invalid("Only native Intel x86_64 hosts are supported")
    #else
    var translated: Int32 = 0, length = MemoryLayout<Int32>.size
    let result = sysctlbyname("sysctl.proc_translated", &translated, &length, nil, 0)
    try require((result == 0 && length == MemoryLayout<Int32>.size && translated == 0) || (result == -1 && errno == ENOENT), "Rosetta or an unknown host translation state is unsupported")
    let major = ProcessInfo.processInfo.operatingSystemVersion.majorVersion
    try require([15,26].contains(major), "Only macOS 15 and 26 are supported")
    try require(geteuid() != 0 && getuid() == geteuid(), "Run as your normal user; sudo/root installation is refused")
    return major
    #endif
}
func emit<T: Encodable>(_ value: T) throws {
    FileHandle.standardOutput.write(try jsonData(value)); FileHandle.standardOutput.write(Data([10]))
}
@main struct MellowInstaller {
    static func main() {
        do {
            let arguments = Array(CommandLine.arguments.dropFirst())
            if arguments == ["--help"] || arguments == ["help"] || arguments.isEmpty { print(usage); return }
            let major = try hostMajor()
            if arguments == ["--self-test"] { try emit(selfTests(hostMajor: major)); return }
            guard let action = arguments.first, ["install","status","list","rollback","uninstall"].contains(action) else { throw InstallError.invalid("Unknown command; use --help") }
            var options: [String: String] = [:], index = 1
            while index < arguments.count {
                let key = arguments[index]
                try require(["--release","--prefix","--archive","--sha256"].contains(key) && options[key] == nil && index + 1 < arguments.count, "Unknown, repeated or incomplete option")
                options[key] = arguments[index + 1]; index += 2
            }
            let defaultPrefix = FileManager.default.homeDirectoryForCurrentUser.resolvingSymlinksInPath().appendingPathComponent("Library/Application Support/Mellow").path
            let prefix = options["--prefix"] ?? defaultPrefix
            let tag = options["--release"]
            if ["install","rollback","uninstall"].contains(action) { try require(tag.map(validTag) == true, "A valid --release TAG is required") }
            else { try require(tag == nil, "This command does not accept --release") }
            if action != "install" { try require(options["--archive"] == nil && options["--sha256"] == nil, "Offline archive options are only valid for install") }
            if action == "install" {
                let archive: Data, sha: String, expectedCommit: String?, verification: String
                if let path = options["--archive"] {
                    guard let hash = options["--sha256"] else { throw InstallError.invalid("Offline installation requires an independently obtained --sha256") }
                    archive = try snapshotFile(path); sha = hash; expectedCommit = nil; verification = "offline-user-checksum"
                } else {
                    try require(options["--sha256"] == nil, "--sha256 requires --archive")
                    let asset = try releaseAsset(tag!, major: major)
                    archive = try download(asset.url, limit: archiveLimit, asset: true)
                    sha = asset.sha256; expectedCommit = asset.sourceCommit; verification = "github-asset-digest-and-tag"
                }
                let package = try checkPackage(archive, sha256: sha, release: tag!, major: major, expectedCommit: expectedCommit)
                let store = try InstallStore(prefix: prefix, create: true)
                try store.install(package, verification: verification)
                try emit(store.report(action))
            } else {
                let store = try InstallStore(prefix: prefix, create: false)
                if action == "rollback" { try store.rollback(tag!) }
                if action == "uninstall" { try store.uninstall(tag!) }
                try emit(store.report(action))
            }
        } catch {
            let status: String
            if isDurabilityError(error) { status = "DURABILITY_UNCERTAIN" } else { status = "FAILED" }
            let object: [String: Any] = ["status":status, "reason":String(describing: error), "activationPerformed":false]
            if let bytes = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) { FileHandle.standardError.write(bytes); FileHandle.standardError.write(Data([10])) }
            exit(1)
        }
    }
}
