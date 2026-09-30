// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
import Foundation
import Darwin

struct SelfTestReport: Codable {
    let status: String
    let scope: String
    let hostOSMajor: Int
    let checks: Int
    let networkUsed: Bool
    let systemInstallationPerformed: Bool
    let nativeBinaryExecutionVerified: Bool
    let physicalGPUVerified: Bool
    let tests: [String]
}
private func fixtureMachO(kext: Bool, major: Int) -> Data {
    // Header-only test bytes, not a functioning executable or GPU proof.
    func word(_ value: UInt32) -> [UInt8] { (0..<4).map { UInt8(truncatingIfNeeded: value >> ($0 * 8)) } }
    var commands = word(0x32) + word(24) + word(1) + word(UInt32(major) << 16) + word(UInt32(major) << 16 | UInt32(major == 15 ? 5 : 2) << 8) + word(0)
    if !kext {
        var name = Array("@rpath/MellowAppleUserspace.framework/Versions/A/MellowAppleUserspace\0".utf8)
        while (24 + name.count) % 8 != 0 { name.append(0) }
        commands += word(0xd) + word(UInt32(24 + name.count)) + word(24) + word(0) + word(0) + word(0) + name
    }
    let header = word(0xfeedfacf) + word(0x01000007) + word(3) + word(kext ? 11 : 6) + word(kext ? 1 : 2) + word(UInt32(commands.count)) + word(0) + word(0)
    return Data(header + commands)
}
private func fixtureEntries(release: String, major: Int) throws -> [TarEntry] {
    let source = String(repeating: "a", count: 40)
    var payload: [String: Data] = ["payload/SOURCE-COMMIT.txt":Data((source + "\n").utf8),"payload/LICENSE":Data("fixture license\n".utf8),"payload/NOTICE":Data("fixture notice\n".utf8)]
    var bundles: [PackageBundle] = [], executablePaths = Set<String>()
    for kext in [true,false] {
        let path = kext ? "payload/Mellow.kext" : "payload/MellowAppleUserspace.framework"
        let name = kext ? "Mellow" : "MellowAppleUserspace"
        let identifier = kext ? "com.NiSeullent.Mellow" : "com.NiSeullent.Mellow.AppleUserspace"
        let binary = kext ? path + "/Contents/MacOS/Mellow" : path + "/Versions/A/MellowAppleUserspace"
        let plist = kext ? path + "/Contents/Info.plist" : path + "/Versions/A/Resources/Info.plist"
        payload[binary] = fixtureMachO(kext: kext, major: major); executablePaths.insert(binary)
        payload[plist] = try PropertyListSerialization.data(fromPropertyList: ["CFBundleIdentifier":identifier,"CFBundleExecutable":name,"CFBundlePackageType":kext ? "KEXT" : "FMWK","CFBundleVersion":"1"], format: .xml, options: 0)
        bundles.append(PackageBundle(kind: kext ? "kext" : "framework", path: path, identifier: identifier, executable: name, binaryPath: binary, signing: "unsigned-development", teamIdentifier: nil))
    }
    let files = payload.keys.sorted().map { PackageFile(path: $0, size: payload[$0]!.count, sha256: digest(payload[$0]!), executable: executablePaths.contains($0)) }
    let manifest = PackageManifest(schemaVersion: 1, repository: repository, release: release, sourceCommit: source, architecture: "x86_64", targetOSMajor: major, sdkVersion: major == 15 ? "15.5" : "26.2", scope: "user-development", capabilities: .development, files: files, bundles: bundles)
    return [TarEntry(path: "manifest.json", bytes: try jsonData(manifest), directory: false, executable: false)] + files.map { TarEntry(path: $0.path, bytes: payload[$0.path]!, directory: false, executable: $0.executable) }
}
private func fixtureTar(_ entries: [TarEntry], firstType: UInt8? = nil) -> Data {
    var result = Data()
    for (index, entry) in entries.enumerated() {
        var header = [UInt8](repeating: 0, count: 512)
        func put(_ offset: Int, _ value: [UInt8]) { for (i, byte) in value.enumerated() { header[offset + i] = byte } }
        func number(_ offset: Int, _ length: Int, _ value: Int) { put(offset, Array(String(format: "%0*o", length - 1, value).utf8) + [0]) }
        put(0, Array(entry.path.utf8)); number(100, 8, entry.executable ? 0o700 : 0o600)
        number(108, 8, 0); number(116, 8, 0); number(124, 12, entry.bytes.count); number(136, 12, 0)
        put(148, [UInt8](repeating: 32, count: 8)); header[156] = index == 0 ? (firstType ?? 48) : 48
        put(257, Array("ustar\0".utf8)); put(263, Array("00".utf8))
        let checksum = header.reduce(0) { $0 + Int($1) }
        put(148, Array(String(format: "%06o", checksum).utf8) + [0,32])
        result.append(contentsOf: header); result.append(entry.bytes)
        result.append(Data(repeating: 0, count: (512 - entry.bytes.count % 512) % 512))
    }
    result.append(Data(repeating: 0, count: 1024)); return result
}
func selfTests(hostMajor: Int) throws -> SelfTestReport {
    var checks = 0, tests: [String] = []
    func check(_ value: Bool, _ name: String) throws { checks += 1; try require(value, "Self-test failed: " + name); tests.append(name) }
    func rejects(_ name: String, _ body: () throws -> Void) throws {
        var rejected = false; do { try body() } catch { rejected = true }
        try check(rejected, name)
    }
    let entries = try fixtureEntries(release: "fixture-a", major: hostMajor), archive = fixtureTar(entries)
    let package = try checkPackage(archive, sha256: digest(archive), release: "fixture-a", major: hostMajor, expectedCommit: String(repeating: "a", count: 40))
    try check(package.manifest.release == "fixture-a", "bounded USTAR and header-only fixture acceptance")
    try rejects("checksum mismatch") { _ = try checkPackage(archive, sha256: String(repeating: "0", count: 64), release: "fixture-a", major: hostMajor, expectedCommit: nil) }
    var damaged = archive; damaged[0] ^= 1
    try rejects("USTAR header corruption") { _ = try parseTar(damaged) }
    try rejects("archive link type") { _ = try parseTar(fixtureTar(entries, firstType: 50)) }
    try rejects("PAX extension type") { _ = try parseTar(fixtureTar(entries, firstType: 120)) }
    try rejects("truncated trailer") { _ = try parseTar(archive.dropLast(512)) }
    try rejects("archive traversal") { _ = try parseTar(fixtureTar([TarEntry(path: "../outside", bytes: Data(), directory: false, executable: false)])) }
    try rejects("case alias") { _ = try parseTar(fixtureTar([TarEntry(path: "payload/A", bytes: Data(), directory: false, executable: false),TarEntry(path: "payload/a", bytes: Data(), directory: false, executable: false)])) }
    try rejects("undeclared archive file") { _ = try checkEntries(entries + [TarEntry(path: "payload/unknown", bytes: Data(), directory: false, executable: false)], sha256: digest(archive), release: "fixture-a", major: hostMajor, expectedCommit: nil) }
    try rejects("source tag mismatch") { _ = try checkPackage(archive, sha256: digest(archive), release: "fixture-a", major: hostMajor, expectedCommit: String(repeating: "b", count: 40)) }
    try rejects("wrong native binary kind") { _ = try checkMachO(fixtureMachO(kext: true, major: hostMajor), kext: false, major: hostMajor, sdk: hostMajor == 15 ? "15.5" : "26.2") }
    try rejects("truncated Mach-O commands") { _ = try checkMachO(fixtureMachO(kext: false, major: hostMajor).dropLast(), kext: false, major: hostMajor, sdk: hostMajor == 15 ? "15.5" : "26.2") }
    let temporary = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent("mellow-installer-tests-" + UUID().uuidString.lowercased(), isDirectory: true)
    try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    defer { try? FileManager.default.removeItem(at: temporary) }
    let prefix = temporary.appendingPathComponent("store").path
    let store = try InstallStore(prefix: prefix, create: true)
    try store.install(package, verification: "offline-user-checksum")
    try check(store.state.current == "fixture-a" && (try store.report("test")).releases.count == 1, "transactional temporary-prefix installation")
    try rejects("no overwrite of existing release") { try store.install(package, verification: "offline-user-checksum") }
    try check(store.state.current == "fixture-a", "duplicate install preserves current release")
    let archiveB = fixtureTar(try fixtureEntries(release: "fixture-b", major: hostMajor))
    let packageB = try checkPackage(archiveB, sha256: digest(archiveB), release: "fixture-b", major: hostMajor, expectedCommit: nil)
    try store.install(packageB, verification: "offline-user-checksum")
    try check(store.state.current == "fixture-b" && store.state.previous == "fixture-a", "previous release preservation")
    try store.rollback("fixture-a")
    try check(store.state.current == "fixture-a" && store.state.previous == "fixture-b", "tracked rollback")
    try rejects("arbitrary rollback refused") { try store.rollback("missing") }
    try rejects("arbitrary deletion refused") { try store.uninstall("../outside") }
    let releaseA = try store.versions.child("fixture-a")
    try releaseA.writeNew("payload/untracked", data: Data("preserve me".utf8))
    try rejects("untracked file stops uninstall") { try store.uninstall("fixture-a") }
    try check((try releaseA.read("payload/untracked")) == Data("preserve me".utf8), "untracked file was preserved")
    try releaseA.removeFile("payload/untracked")
    let originalState = try store.root.read("state.json")
    try store.root.removeFile("state.json")
    try store.root.writeNew("state.json", data: Data("foreign state".utf8))
    let archiveC = fixtureTar(try fixtureEntries(release: "fixture-c", major: hostMajor))
    let packageC = try checkPackage(archiveC, sha256: digest(archiveC), release: "fixture-c", major: hostMajor, expectedCommit: nil)
    try rejects("failed state commit rolls back new version") { try store.install(packageC, verification: "offline-user-checksum") }
    try check(!(try store.versions.exists("fixture-c")) && (try store.root.read("state.json")) == Data("foreign state".utf8), "foreign state and prior releases preserved")
    try store.root.removeFile("state.json"); try store.root.writeNew("state.json", data: originalState)
    try store.uninstall("fixture-a")
    try check(store.state.current == "fixture-b" && !(try store.versions.exists("fixture-a")), "tracked uninstall selects preserved previous release")
    let link = temporary.appendingPathComponent("linked-store").path
    try require(symlink(prefix, link) == 0, "Cannot create self-test symlink")
    try rejects("symlink prefix refused") { _ = try InstallStore(prefix: link, create: true) }
    let releaseB = try store.versions.child("fixture-b")
    let existing = releaseB.url.appendingPathComponent("payload/LICENSE").path
    let hard = temporary.appendingPathComponent("hard-link").path
    try require(Darwin.link(existing, hard) == 0, "Cannot create self-test hard link")
    try rejects("hard-linked payload refused") { _ = try store.checked("fixture-b") }
    try require(unlink(hard) == 0, "Cannot remove self-test hard link")
    try store.uninstall("fixture-b")
    try check((try store.report("test")).releases.isEmpty && store.state.current == nil, "only owned releases removed")
    return SelfTestReport(status: "PASSED", scope: "CPU archive validation and isolated filesystem transactions; header-only binary fixtures", hostOSMajor: hostMajor, checks: checks, networkUsed: false, systemInstallationPerformed: false, nativeBinaryExecutionVerified: false, physicalGPUVerified: false, tests: tests)
}
