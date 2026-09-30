// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
import Foundation

struct ReleaseAsset {
    let release: String
    let url: String
    let sha256: String
    let sourceCommit: String
}
// macOS supplies curl. No package manager, interpreter installation or shell
// interpolation is used. URLs originate only from this fixed public repository.
func download(_ address: String, limit: Int, asset: Bool) throws -> Data {
    guard let initial = URLComponents(string: address) else { throw InstallError.invalid("Invalid download URL") }
    try require(initial.scheme == "https" && initial.user == nil && initial.password == nil && initial.port == nil &&
        initial.host == (asset ? "github.com" : "api.github.com"), "Unexpected download origin")
    let temporary = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent("mellow-download-" + UUID().uuidString.lowercased(), isDirectory: true)
    try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    defer { try? FileManager.default.removeItem(at: temporary) }
    let output = temporary.appendingPathComponent("download")
    let process = Process(), pipe = Pipe()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/curl")
    process.arguments = ["--fail","--silent","--show-error","--location","--proto","=https","--proto-redir","=https","--tlsv1.2",
        "--max-redirs","4","--connect-timeout","20","--max-time","300","--max-filesize",String(limit),
        "--header","Accept: application/vnd.github+json","--header","X-GitHub-Api-Version: 2022-11-28",
        "--output",output.path,"--write-out","%{url_effective}",address]
    process.standardOutput = pipe
    process.standardError = FileHandle.standardError
    try process.run()
    let finalData = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    try require(process.terminationReason == .exit && process.terminationStatus == 0 && finalData.count <= 16384, "GitHub download failed")
    guard let finalText = String(data: finalData, encoding: .utf8), let final = URLComponents(string: finalText) else { throw InstallError.invalid("Invalid final download URL") }
    let allowed = asset ? ["github.com","release-assets.githubusercontent.com","objects.githubusercontent.com"] : ["api.github.com"]
    try require(final.scheme == "https" && final.user == nil && final.password == nil && final.port == nil && allowed.contains(final.host ?? ""), "Unexpected GitHub download redirect")
    return try snapshotFile(output.path, limit: limit)
}
func releaseAsset(_ tag: String, major: Int) throws -> ReleaseAsset {
    try require(validTag(tag) && [15,26].contains(major), "Invalid release selection")
    let prefix = "https://api.github.com/repos/" + repository
    let metadata = try download(prefix + "/releases/tags/" + tag, limit: 1024 * 1024, asset: false)
    guard let object = try JSONSerialization.jsonObject(with: metadata) as? [String: Any],
          object["tag_name"] as? String == tag, object["draft"] as? Bool == false,
          let assets = object["assets"] as? [[String: Any]] else { throw InstallError.invalid("Unexpected GitHub release metadata") }
    let name = "mellow-development-macos" + String(major) + "-x86_64.tar"
    let matches = assets.filter { $0["name"] as? String == name }
    try require(matches.count == 1, "Requested binary package is absent or duplicated in the release")
    let candidate = matches[0]
    let expectedURL = "https://github.com/" + repository + "/releases/download/" + tag + "/" + name
    guard candidate["state"] as? String == "uploaded", candidate["browser_download_url"] as? String == expectedURL,
          let rawDigest = candidate["digest"] as? String, rawDigest.hasPrefix("sha256:"),
          let size = candidate["size"] as? NSNumber else { throw InstallError.invalid("Release asset identity or GitHub SHA256 digest is missing") }
    let sha = String(rawDigest.dropFirst(7))
    try require(isHex(sha, count: 64) && size.int64Value >= 1024 && size.int64Value <= Int64(archiveLimit), "Invalid release asset hash or size")
    let commitData = try download(prefix + "/commits/" + tag, limit: 1024 * 1024, asset: false)
    guard let commit = try JSONSerialization.jsonObject(with: commitData) as? [String: Any],
          let shaCommit = commit["sha"] as? String, isHex(shaCommit, count: 40) else { throw InstallError.invalid("Cannot resolve the release tag to a source commit") }
    return ReleaseAsset(release: tag, url: expectedURL, sha256: sha, sourceCommit: shaCommit)
}
