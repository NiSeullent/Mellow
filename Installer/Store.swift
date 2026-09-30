// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
import Foundation
import Darwin

struct StoreMarker: Codable {
    let schemaVersion: Int
    let repository: String
    let storeID: String
    let userID: UInt32
}
struct StoreState: Codable {
    let schemaVersion: Int
    let storeID: String
    let current: String?
    let previous: String?
}
struct InstallReceipt: Codable {
    let schemaVersion: Int
    let storeID: String
    let release: String
    let sourceCommit: String
    let archiveSHA256: String
    let manifestSHA256: String
    let verification: String
    let installedAt: String
}
struct ReleaseReport: Codable {
    let release: String
    let sourceCommit: String
    let path: String
    let archiveSHA256: String
    let verification: String
    let signing: [String: String]
}
struct StoreReport: Codable {
    let schemaVersion: Int
    let action: String
    let prefix: String
    let current: String?
    let previous: String?
    let releases: [ReleaseReport]
    let capabilities: Capabilities
    let scope: String
    let diagnosticKextDevice: String
    let activationPerformed: Bool
}
final class InstallStore {
    let root: SafeDirectory
    let versions: SafeDirectory
    let marker: StoreMarker
    private let lockFD: Int32
    private(set) var state: StoreState
    deinit { _ = flock(lockFD, LOCK_UN); close(lockFD) }
    init(prefix: String, create: Bool) throws {
        let localRoot = try SafeDirectory.prefix(prefix, create: create)
        let markerName = ".mellow-store.json"
        if try !localRoot.exists(markerName) {
            try require(create && (try localRoot.names()).isEmpty, "Refusing to claim an existing untracked directory")
            let initial = StoreMarker(schemaVersion: 1, repository: repository, storeID: UUID().uuidString.lowercased(), userID: geteuid())
            try localRoot.writeNew(markerName, data: jsonData(initial))
        }
        let localMarker = try JSONDecoder().decode(StoreMarker.self, from: localRoot.read(markerName, limit: 16384))
        try require(localMarker.schemaVersion == 1 && localMarker.repository == repository && localMarker.userID == geteuid() && UUID(uuidString: localMarker.storeID) != nil, "Unrecognized store ownership marker")
        let localLock = openat(localRoot.fd, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0o600)
        try require(localLock >= 0, "Cannot open store lock")
        var lockInfo = stat()
        if fstat(localLock, &lockInfo) != 0 || (lockInfo.st_mode & S_IFMT) != S_IFREG || lockInfo.st_uid != geteuid() || lockInfo.st_nlink != 1 || (lockInfo.st_mode & 0o022) != 0 {
            close(localLock); throw InstallError.invalid("Unowned or unsafe store lock")
        }
        if flock(localLock, LOCK_EX | LOCK_NB) != 0 { close(localLock); throw InstallError.invalid("Another Mellow installer is using this prefix") }
        do {
            let localVersions = try localRoot.child("versions", create: create)
            let allowed = Set([markerName, ".lock", "versions", "state.json"])
            try require(Set(try localRoot.names()).isSubset(of: allowed), "Store contains untracked root entries")
            let localState: StoreState
            if try localRoot.exists("state.json") {
                localState = try JSONDecoder().decode(StoreState.self, from: localRoot.read("state.json", limit: 16384))
                try require(localState.schemaVersion == 1 && localState.storeID == localMarker.storeID && [localState.current,localState.previous].compactMap { $0 }.allSatisfy(validTag), "Unrecognized store state")
            } else {
                try require(create && (try localVersions.names()).isEmpty, "Missing store state; existing releases require inspection")
                localState = StoreState(schemaVersion: 1, storeID: localMarker.storeID, current: nil, previous: nil)
                try localRoot.writeNew("state.json", data: jsonData(localState))
            }
            root = localRoot; marker = localMarker; lockFD = localLock; versions = localVersions; state = localState
        } catch { _ = flock(localLock, LOCK_UN); close(localLock); throw error }
    }
    func checked(_ release: String) throws -> (SafeDirectory, CheckedPackage, InstallReceipt) {
        try require(validTag(release), "Invalid release tag")
        let directory = try versions.child(release)
        let receipt = try JSONDecoder().decode(InstallReceipt.self, from: directory.read(".receipt.json", limit: 16384))
        let manifestData = try directory.read("manifest.json", limit: 4 * 1024 * 1024)
        try require(!(try directory.mode(".receipt.json")) && !(try directory.mode("manifest.json")), "Installed metadata permissions changed")
        try require(receipt.schemaVersion == 1 && receipt.storeID == marker.storeID && receipt.release == release &&
            isHex(receipt.sourceCommit, count: 40) && isHex(receipt.archiveSHA256, count: 64) && receipt.manifestSHA256 == digest(manifestData) &&
            ["github-asset-digest-and-tag","offline-user-checksum"].contains(receipt.verification), "Release is not tracked by this store")
        let manifest = try JSONDecoder().decode(PackageManifest.self, from: manifestData)
        try require(!manifest.files.isEmpty && manifest.files.count <= 4095, "Installed manifest file budget exceeded")
        var total = 0
        for file in manifest.files {
            try require(file.size >= 0 && file.size <= 128 * 1024 * 1024 && total <= archiveLimit - file.size, "Installed manifest byte budget exceeded")
            total += file.size
        }
        var entries = [TarEntry(path: "manifest.json", bytes: manifestData, directory: false, executable: false)]
        var tracked = Set(["manifest.json", ".receipt.json"])
        for file in manifest.files {
            _ = try pathParts(file.path)
            try require(tracked.insert(file.path).inserted, "Duplicate installed file")
            entries.append(TarEntry(path: file.path, bytes: try directory.read(file.path, limit: file.size), directory: false, executable: try directory.mode(file.path)))
        }
        let inventory = try directory.tree()
        try require(inventory.files == tracked, "Installed release contains untracked or missing files")
        let package = try checkEntries(entries, sha256: receipt.archiveSHA256, release: release, major: manifest.targetOSMajor, expectedCommit: receipt.sourceCommit)
        var expectedDirectories = Set<String>()
        for file in tracked {
            let parts = try pathParts(file)
            for count in 1..<parts.count { expectedDirectories.insert(parts.prefix(count).joined(separator: "/")) }
        }
        try require(inventory.directories == expectedDirectories, "Installed release contains untracked directories")
        for bundle in package.manifest.bundles { try verifySignature(bundle, at: directory.url.appendingPathComponent(bundle.binaryPath)) }
        return (directory, package, receipt)
    }
    private func replaceState(current: String?, previous: String?) throws {
        // Only our already-decoded state may be replaced. Refuse a concurrent
        // state edit rather than overwriting another program's file.
        let existing = try JSONDecoder().decode(StoreState.self, from: root.read("state.json", limit: 16384))
        try require(existing.schemaVersion == state.schemaVersion && existing.storeID == state.storeID && existing.current == state.current && existing.previous == state.previous, "Store state changed during the operation")
        let next = StoreState(schemaVersion: 1, storeID: marker.storeID, current: current, previous: previous)
        let temporary = ".state-" + UUID().uuidString.lowercased()
        do {
            try root.writeNew(temporary, data: jsonData(next))
            let publication = try root.move(temporary, to: root, as: "state.json", exclusive: false)
            state = next
            if !publication.durable { throw InstallError.durability("State rename occurred, but directory synchronization failed; both releases were preserved and crash durability is uncertain") }
        } catch { if (try? root.exists(temporary)) == true { try? root.removeFile(temporary) }; throw error }
    }
    func install(_ package: CheckedPackage, verification: String) throws {
        let release = package.manifest.release
        try require(!(try versions.exists(release)), "Release already exists; existing releases are never overwritten")
        if let current = state.current { _ = try checked(current) }
        if let previous = state.previous { _ = try checked(previous) }
        let temporary = ".install-" + UUID().uuidString.lowercased()
        guard mkdirat(versions.fd, temporary, 0o700) == 0 else { throw systemFailure("Create installation staging directory") }
        let stage = try versions.child(temporary)
        var tracked = Set<String>(), published = false
        do {
            for file in package.manifest.files {
                guard let data = package.files[file.path] else { throw InstallError.invalid("Missing validated payload") }
                tracked.insert(file.path)
                try stage.writeNew(file.path, data: data, executable: file.executable)
            }
            tracked.insert("manifest.json"); try stage.writeNew("manifest.json", data: package.manifestData)
            let receipt = InstallReceipt(schemaVersion: 1, storeID: marker.storeID, release: release, sourceCommit: package.manifest.sourceCommit,
                archiveSHA256: package.archiveSHA256, manifestSHA256: digest(package.manifestData), verification: verification,
                installedAt: ISO8601DateFormatter().string(from: Date()))
            tracked.insert(".receipt.json"); try stage.writeNew(".receipt.json", data: jsonData(receipt))
            for bundle in package.manifest.bundles { try verifySignature(bundle, at: stage.url.appendingPathComponent(bundle.binaryPath)) }
            let publication = try versions.move(temporary, to: versions, as: release)
            published = true
            if !publication.durable { throw InstallError.durability("Release rename occurred, but directory synchronization failed; the new and prior releases were preserved and current state was not changed") }
            _ = try checked(release)
            try replaceState(current: release, previous: state.current)
        } catch {
            if isDurabilityError(error) { throw error }
            // Roll back only the exact files created in this transaction. A
            // failed state commit leaves the prior current release intact.
            if published { _ = try? versions.move(release, to: versions, as: temporary) }
            if (try? versions.exists(temporary)) == true {
                do { try stage.removeExact(files: tracked); _ = unlinkat(versions.fd, temporary, AT_REMOVEDIR) }
                catch { /* Preserve any incomplete or externally changed tree for inspection. */ }
            }
            throw error
        }
    }
    func rollback(_ release: String) throws {
        _ = try checked(release)
        if let current = state.current { _ = try checked(current) }
        try require(state.current != release, "Release is already current")
        try replaceState(current: release, previous: state.current)
    }
    func uninstall(_ release: String) throws {
        let (directory, package, _) = try checked(release)
        var replacement = state.current, previous = state.previous
        if state.current == release {
            replacement = state.previous
            if let replacement = replacement { _ = try checked(replacement) }
            previous = nil
        } else if state.previous == release { previous = nil }
        let temporary = ".remove-" + UUID().uuidString.lowercased()
        let removal = try versions.move(release, to: versions, as: temporary)
        if !removal.durable {
            _ = try? versions.move(temporary, to: versions, as: release)
            throw InstallError.durability("Uninstall rename occurred, but directory synchronization failed; files were preserved and restoration durability is uncertain")
        }
        do { try replaceState(current: replacement, previous: previous) }
        catch {
            if isDurabilityError(error) { throw error }
            _ = try? versions.move(temporary, to: versions, as: release); throw error
        }
        let tracked = Set(package.manifest.files.map { $0.path }).union(["manifest.json", ".receipt.json"])
        try directory.removeExact(files: tracked)
        guard unlinkat(versions.fd, temporary, AT_REMOVEDIR) == 0 else { throw systemFailure("Remove empty release directory") }
        if !persistDirectory(versions.fd) { throw InstallError.durability("Tracked files were removed, but final directory synchronization failed; crash durability is uncertain") }
    }
    func report(_ action: String) throws -> StoreReport {
        var releases: [ReleaseReport] = []
        for name in try versions.names() {
            try require(validTag(name) && !name.hasPrefix("."), "Interrupted transaction or untracked version entry requires inspection; it was preserved")
            let (directory, package, receipt) = try checked(name)
            releases.append(ReleaseReport(release: name, sourceCommit: package.manifest.sourceCommit, path: directory.url.path,
                archiveSHA256: receipt.archiveSHA256, verification: receipt.verification,
                signing: Dictionary(uniqueKeysWithValues: package.manifest.bundles.map { ($0.kind,$0.signing) })))
        }
        let names = Set(releases.map { $0.release })
        try require([state.current,state.previous].compactMap { $0 }.allSatisfy { names.contains($0) }, "Store state references a missing release")
        return StoreReport(schemaVersion: 1, action: action, prefix: root.url.path, current: state.current, previous: state.previous,
            releases: releases, capabilities: .development, scope: "user-development; application-only framework",
            diagnosticKextDevice: "Intel 8086:7D41 diagnostic profile; GPU acceleration unverified", activationPerformed: false)
    }
}
