// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
import Foundation
import CryptoKit
import Security

enum InstallError: Error, CustomStringConvertible {
    case invalid(String)
    case durability(String)
    var description: String {
        switch self { case let .invalid(message), let .durability(message): return message }
    }
}
func isDurabilityError(_ error: Error) -> Bool {
    guard let failure = error as? InstallError else { return false }
    if case .durability = failure { return true }; return false
}
func require(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
    if try !condition() { throw InstallError.invalid(message) }
}
let repository = "NiSeullent/Mellow"
let archiveLimit = 512 * 1024 * 1024
func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
func isHex(_ value: String, count: Int) -> Bool {
    value.utf8.count == count && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
}
func validTag(_ tag: String) -> Bool {
    !tag.isEmpty && tag.utf8.count <= 96 && tag != "." && tag != ".." &&
        tag.utf8.first.map { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) } == true &&
        tag.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [45,46,95].contains($0) }
}
func pathParts(_ path: String) throws -> [String] {
    try require(!path.isEmpty && path.utf8.count <= 240 && !path.hasPrefix("/") && !path.hasSuffix("/"), "Invalid relative package path")
    try require(path.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [45,46,47,95].contains($0) }, "Package paths must use bounded ASCII names")
    let parts = path.components(separatedBy: "/")
    try require(parts.allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." && $0.utf8.count <= 100 }, "Package path traversal or empty component")
    return parts
}
func jsonData<T: Encodable>(_ value: T) throws -> Data {
    let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys, .prettyPrinted]
    return try encoder.encode(value)
}
struct Capabilities: Codable {
    let physicalGPUVerified: Bool
    let systemMetalRegistered: Bool
    let windowServerAccelerationVerified: Bool
    static let development = Capabilities(physicalGPUVerified: false, systemMetalRegistered: false, windowServerAccelerationVerified: false)
    var isDevelopment: Bool { !physicalGPUVerified && !systemMetalRegistered && !windowServerAccelerationVerified }
}
struct PackageFile: Codable {
    let path: String
    let size: Int
    let sha256: String
    let executable: Bool
}
struct PackageBundle: Codable {
    let kind: String
    let path: String
    let identifier: String
    let executable: String
    let binaryPath: String
    let signing: String
    let teamIdentifier: String?
}
struct PackageManifest: Codable {
    let schemaVersion: Int
    let repository: String
    let release: String
    let sourceCommit: String
    let architecture: String
    let targetOSMajor: Int
    let sdkVersion: String
    let scope: String
    let capabilities: Capabilities
    let files: [PackageFile]
    let bundles: [PackageBundle]
}
struct TarEntry {
    let path: String
    let bytes: Data
    let directory: Bool
    let executable: Bool
}
// POSIX USTAR only, without an external archive extractor. Reject links, PAX,
// GNU extensions, devices, traversal, duplicates and case-insensitive aliases
// before creating ANY files. Padding and the complete trailer must be zero.
func parseTar(_ archive: Data) throws -> [TarEntry] {
    try require(archive.count >= 1024 && archive.count <= archiveLimit && archive.count % 512 == 0, "Archive size or alignment is invalid")
    let bytes = [UInt8](archive)
    func text(_ begin: Int, _ count: Int) throws -> String {
        let field = Array(bytes[begin..<(begin + count)])
        let end = field.firstIndex(of: 0) ?? count
        try require(field[end...].allSatisfy { $0 == 0 }, "Nonzero data after USTAR string terminator")
        guard let result = String(bytes: field[..<end], encoding: .utf8) else { throw InstallError.invalid("Invalid archive name encoding") }
        return result
    }
    func octal(_ begin: Int, _ count: Int) throws -> Int {
        let field = Array(bytes[begin..<(begin + count)])
        try require(field.allSatisfy { $0 == 0 || $0 == 32 || (48...55).contains($0) }, "Unsupported USTAR numeric encoding")
        let digits = field.filter { $0 != 0 && $0 != 32 }
        var value = 0
        for digit in digits { try require(value <= (Int.max - Int(digit - 48)) / 8, "Archive integer overflow"); value = value * 8 + Int(digit - 48) }
        return value
    }
    var entries: [TarEntry] = [], seen = Set<String>(), offset = 0
    while offset + 512 <= bytes.count {
        if bytes[offset..<(offset + 512)].allSatisfy({ $0 == 0 }) {
            try require(offset + 1024 <= bytes.count && bytes[offset...].allSatisfy { $0 == 0 }, "Incomplete or nonzero USTAR trailer")
            return entries
        }
        try require(entries.count < 4096 && Array(bytes[(offset + 257)..<(offset + 263)]) == Array("ustar\0".utf8) &&
            Array(bytes[(offset + 263)..<(offset + 265)]) == Array("00".utf8), "Only bounded POSIX USTAR archives are accepted")
        let checksum = try octal(offset + 148, 8)
        var actual = 0
        for i in 0..<512 { actual += (148..<156).contains(i) ? 32 : Int(bytes[offset + i]) }
        try require(checksum == actual, "USTAR header checksum mismatch")
        let type = bytes[offset + 156]
        try require(type == 0 || type == 48 || type == 53, "Archive links, special files and extensions are forbidden")
        try require(bytes[(offset + 157)..<(offset + 257)].allSatisfy { $0 == 0 }, "Archive link targets are forbidden")
        var name = try text(offset, 100)
        let prefix = try text(offset + 345, 155)
        if !prefix.isEmpty { name = prefix + "/" + name }
        if type == 53 && name.hasSuffix("/") { name.removeLast() }
        _ = try pathParts(name)
        try require(seen.insert(name.lowercased()).inserted, "Duplicate or case-aliased archive path")
        let size = try octal(offset + 124, 12), mode = try octal(offset + 100, 8)
        try require(size <= 128 * 1024 * 1024 && (mode & 0o7000) == 0 && (type != 53 || size == 0), "Invalid archive extent or special permissions")
        let begin = offset + 512
        try require(size <= bytes.count - begin, "Truncated archive payload")
        let padded = ((size + 511) / 512) * 512
        try require(padded <= bytes.count - begin && bytes[(begin + size)..<(begin + padded)].allSatisfy { $0 == 0 }, "Truncated or nonzero archive padding")
        entries.append(TarEntry(path: name, bytes: Data(bytes[begin..<(begin + size)]), directory: type == 53, executable: mode & 0o111 != 0))
        offset = begin + padded
    }
    throw InstallError.invalid("Missing complete USTAR trailer")
}
struct CheckedPackage {
    let manifest: PackageManifest
    let manifestData: Data
    let archiveSHA256: String
    let files: [String: Data]
}
func checkPackage(_ archive: Data, sha256: String, release: String, major: Int, expectedCommit: String?) throws -> CheckedPackage {
    try require(isHex(sha256, count: 64) && digest(archive) == sha256, "Archive SHA256 mismatch")
    let entries = try parseTar(archive)
    return try checkEntries(entries, sha256: sha256, release: release, major: major, expectedCommit: expectedCommit)
}
func checkEntries(_ entries: [TarEntry], sha256: String, release: String, major: Int, expectedCommit: String?) throws -> CheckedPackage {
    guard let manifestEntry = entries.first(where: { $0.path == "manifest.json" && !$0.directory }) else { throw InstallError.invalid("Missing manifest.json") }
    try require(manifestEntry.bytes.count <= 4 * 1024 * 1024 && !manifestEntry.executable, "Invalid manifest extent or mode")
    let manifest = try JSONDecoder().decode(PackageManifest.self, from: manifestEntry.bytes)
    try require(manifest.schemaVersion == 1 && manifest.repository == repository && manifest.release == release && validTag(release) &&
        isHex(manifest.sourceCommit, count: 40) && manifest.architecture == "x86_64" && manifest.targetOSMajor == major &&
        [15,26].contains(major) && manifest.scope == "user-development" && manifest.capabilities.isDevelopment, "Package identity, target or development scope mismatch")
    if let expectedCommit = expectedCommit { try require(manifest.sourceCommit == expectedCommit, "Package source commit differs from the actual GitHub release tag") }
    try require(manifest.sdkVersion == (major == 15 ? "15.5" : "26.2"), "Unexpected package SDK version")
    try require(!manifest.files.isEmpty && manifest.files.count <= 4095, "Invalid manifest file budget")
    var files: [String: Data] = [:], fileNames = Set<String>(), folded = Set<String>(), directories = Set<String>()
    for file in manifest.files {
        let parts = try pathParts(file.path)
        try require(parts.count >= 2 && parts[0] == "payload" && fileNames.insert(file.path).inserted && folded.insert(file.path.lowercased()).inserted &&
            file.size >= 0 && file.size <= 128 * 1024 * 1024 && isHex(file.sha256, count: 64), "Invalid, duplicate or case-aliased manifest file")
        for count in 1..<parts.count { directories.insert(parts.prefix(count).joined(separator: "/")) }
        guard let entry = entries.first(where: { $0.path == file.path && !$0.directory }) else { throw InstallError.invalid("Missing declared file: " + file.path) }
        try require(entry.bytes.count == file.size && digest(entry.bytes) == file.sha256 && entry.executable == file.executable, "Payload bytes or permissions mismatch: " + file.path)
        files[file.path] = entry.bytes
    }
    for entry in entries {
        if entry.directory { try require(directories.contains(entry.path), "Untracked archive directory") }
        else { try require(entry.path == "manifest.json" || fileNames.contains(entry.path), "Untracked archive file") }
    }
    var canonicalDirectories: [String: String] = [:]
    for directory in directories {
        let key = directory.lowercased()
        try require(canonicalDirectories[key] == nil || canonicalDirectories[key] == directory, "Case-aliased directory hierarchy")
        canonicalDirectories[key] = directory
    }
    try require(fileNames.count + directories.count + 1 <= 8192, "Expanded package tree entry budget exceeded")
    try require(folded.isDisjoint(with: Set(canonicalDirectories.keys)), "File/directory hierarchy collision")
    guard let commit = files["payload/SOURCE-COMMIT.txt"], let source = String(data: commit, encoding: .utf8) else { throw InstallError.invalid("Missing hashed source commit provenance") }
    try require(source == manifest.sourceCommit + "\n", "Source commit provenance mismatch")
    try require(files["payload/LICENSE"] != nil && files["payload/NOTICE"] != nil, "Missing redistribution license or notice")
    try require(manifest.bundles.count == 2, "Both actual development bundles are required")
    var kinds = Set<String>()
    for bundle in manifest.bundles {
        try require(kinds.insert(bundle.kind).inserted, "Duplicate bundle identity")
        let kext = bundle.kind == "kext"
        try require(kext || bundle.kind == "framework", "Unknown bundle kind")
        let path = kext ? "payload/Mellow.kext" : "payload/MellowAppleUserspace.framework"
        let executable = kext ? "Mellow" : "MellowAppleUserspace"
        let binaryPath = kext ? path + "/Contents/MacOS/Mellow" : path + "/Versions/A/MellowAppleUserspace"
        try require(bundle.path == path && bundle.executable == executable && bundle.binaryPath == binaryPath &&
            bundle.identifier == (kext ? "com.NiSeullent.Mellow" : "com.NiSeullent.Mellow.AppleUserspace"), "Unexpected public bundle identity")
        let plistPath = kext ? path + "/Contents/Info.plist" : path + "/Versions/A/Resources/Info.plist"
        guard let plist = files[plistPath], let binary = files[binaryPath] else { throw InstallError.invalid("Missing actual bundle plist or binary") }
        guard let dictionary = try PropertyListSerialization.propertyList(from: plist, options: [], format: nil) as? [String: Any] else { throw InstallError.invalid("Invalid bundle property list") }
        try require(dictionary["CFBundleIdentifier"] as? String == bundle.identifier && dictionary["CFBundleExecutable"] as? String == executable &&
            dictionary["CFBundlePackageType"] as? String == (kext ? "KEXT" : "FMWK") && !(dictionary["CFBundleVersion"] as? String ?? "").isEmpty, "Bundle plist identity mismatch")
        let hasSignature = try checkMachO(binary, kext: kext, major: major, sdk: manifest.sdkVersion)
        try require(["unsigned-development","ad-hoc-development","developer-id"].contains(bundle.signing), "Unknown signing declaration")
        try require(hasSignature == (bundle.signing != "unsigned-development"), "Mach-O code signature differs from package declaration")
        try require(manifest.files.first(where: { $0.path == binaryPath })?.executable == true, "Bundle binary lacks executable mode")
    }
    return CheckedPackage(manifest: manifest, manifestData: manifestEntry.bytes, archiveSHA256: sha256, files: files)
}
func checkMachO(_ data: Data, kext: Bool, major: Int, sdk: String) throws -> Bool {
    let bytes = [UInt8](data)
    func u32(_ offset: Int) throws -> UInt32 {
        try require(offset >= 0 && offset <= bytes.count - 4, "Truncated Mach-O")
        return UInt32(bytes[offset]) | UInt32(bytes[offset + 1]) << 8 | UInt32(bytes[offset + 2]) << 16 | UInt32(bytes[offset + 3]) << 24
    }
    try require(bytes.count >= 32 && (try u32(0)) == 0xfeedfacf && (try u32(4)) == 0x01000007 &&
        (try u32(12)) == (kext ? 11 : 6), "Bundle must contain a real thin x86_64 linked Mach-O of the expected kind")
    let count = Int(try u32(16)), length = Int(try u32(20))
    try require(count > 0 && count <= 256 && length <= bytes.count - 32, "Invalid Mach-O command bounds")
    var offset = 32, signatures = 0, versions = 0, installNames: [String] = []
    for _ in 0..<count {
        let command = try u32(offset), size = Int(try u32(offset + 4))
        try require(size >= 8 && size % 8 == 0 && size <= 32 + length - offset, "Invalid Mach-O command extent")
        if command == 0x1d {
            try require(size == 16, "Invalid code signature command")
            let begin = Int(try u32(offset + 8)), count = Int(try u32(offset + 12))
            try require(begin <= bytes.count && count > 0 && count <= bytes.count - begin, "Invalid signature blob bounds")
            signatures += 1
        }
        if command == 0x32 {
            try require(size >= 24 && (try u32(offset + 8)) == 1, "Non-macOS Mach-O target")
            let minimum = try u32(offset + 12), actualSDK = try u32(offset + 16)
            try require(minimum >> 16 <= UInt32(major), "Binary needs a newer macOS")
            if !kext {
                let pieces = sdk.split(separator: ".").compactMap { UInt32($0) }
                try require(pieces.count == 2 && minimum == UInt32(major) << 16 && actualSDK == (pieces[0] << 16 | pieces[1] << 8), "Framework deployment or SDK differs from manifest")
            }
            versions += 1
        }
        if command == 0xd {
            try require(size >= 24, "Invalid dylib identity command")
            let begin = Int(try u32(offset + 8))
            try require(begin >= 24 && begin < size, "Invalid dylib name extent")
            guard let end = bytes[(offset + begin)..<(offset + size)].firstIndex(of: 0),
                  let name = String(bytes: bytes[(offset + begin)..<end], encoding: .utf8) else { throw InstallError.invalid("Unterminated dylib identity") }
            installNames.append(name)
        }
        offset += size
    }
    // The actual Xcode-linked kext has no LC_BUILD_VERSION. Its target is a
    // package/CI build declaration, not a claim derived from nonexistent bytes.
    // Frameworks must carry one exact native deployment and SDK command.
    try require(offset == 32 + length && signatures <= 1 && (kext ? versions <= 1 : versions == 1), "Missing or duplicate Mach-O target metadata")
    if !kext { try require(installNames == ["@rpath/MellowAppleUserspace.framework/Versions/A/MellowAppleUserspace"], "Framework install-name mismatch") }
    return signatures == 1
}
// Static signature verification does not execute installed code or bypass
// Gatekeeper. Unsigned development code is identified honestly as unsigned.
func verifySignature(_ bundle: PackageBundle, at binary: URL) throws {
    if bundle.signing == "unsigned-development" { return }
    var code: SecStaticCode?
    try require(SecStaticCodeCreateWithPath(binary as CFURL, SecCSFlags(rawValue: 0), &code) == errSecSuccess, "Unable to inspect binary signature")
    guard let code = code else { throw InstallError.invalid("Missing static signature object") }
    var information: CFDictionary?
    try require(SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation), &information) == errSecSuccess, "Unable to read signing information")
    let dictionary = information as NSDictionary?
    let flags = (dictionary?[kSecCodeInfoFlags] as? NSNumber)?.uint32Value ?? 0
    let adHoc = flags & 2 != 0
    if bundle.signing == "ad-hoc-development" { try require(adHoc, "Signature is not declared ad-hoc code") }
    else {
        try require(!adHoc && bundle.teamIdentifier?.isEmpty == false &&
            dictionary?[kSecCodeInfoTeamIdentifier] as? String == bundle.teamIdentifier, "Developer ID team mismatch")
        var requirement: SecRequirement?
        let text = "anchor apple generic and (certificate leaf[field.1.2.840.113635.100.6.1.13] exists or certificate leaf[field.1.2.840.113635.100.6.1.18] exists)"
        try require(SecRequirementCreateWithString(text as CFString, SecCSFlags(rawValue: 0), &requirement) == errSecSuccess &&
            SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), requirement) == errSecSuccess, "Developer ID static validation failed")
    }
    try require(SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), nil) == errSecSuccess, "Static binary signature is invalid")
}
