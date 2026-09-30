// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
import Foundation
import Darwin

struct RenameOutcome { let durable: Bool }
func persistDirectory(_ descriptor: Int32) -> Bool {
    while fsync(descriptor) != 0 { if errno != EINTR { return false } }
    return true
}

func systemFailure(_ operation: String) -> InstallError {
    InstallError.invalid(operation + ": " + String(cString: strerror(errno)))
}
func component(_ name: String) throws {
    try require(!name.isEmpty && name != "." && name != ".." && !name.contains("/") && !name.utf8.contains(0) && name.utf8.count <= 255, "Invalid filesystem component")
}
func snapshotFile(_ path: String, limit: Int = archiveLimit) throws -> Data {
    let descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
    guard descriptor >= 0 else { throw systemFailure("Open input") }
    defer { close(descriptor) }
    return try readDescriptor(descriptor, limit: limit, owned: false)
}
func readDescriptor(_ descriptor: Int32, limit: Int, owned: Bool) throws -> Data {
    var info = stat()
    try require(fstat(descriptor, &info) == 0 && (info.st_mode & S_IFMT) == S_IFREG && info.st_nlink == 1 && info.st_size >= 0 && info.st_size <= Int64(limit) && (!owned || info.st_uid == geteuid()), "Input must be a bounded regular file without hard links")
    var data = Data(count: Int(info.st_size)), copied = 0
    try data.withUnsafeMutableBytes { raw in
        while copied < raw.count {
            let count = Darwin.read(descriptor, raw.baseAddress!.advanced(by: copied), raw.count - copied)
            if count < 0 && errno == EINTR { continue }
            guard count > 0 else { throw systemFailure("Read regular file") }
            copied += count
        }
    }
    var extra: UInt8 = 0
    try require(Darwin.read(descriptor, &extra, 1) == 0, "Input changed size while being read")
    return data
}
// All store traversal uses directory descriptors and O_NOFOLLOW. No archive
// pathname is ever passed to an external extraction command.
final class SafeDirectory {
    let fd: Int32
    let url: URL
    init(fd: Int32, url: URL) { self.fd = fd; self.url = url }
    deinit { close(fd) }
    static func prefix(_ path: String, create: Bool) throws -> SafeDirectory {
        try require(path.hasPrefix("/") && !path.utf8.contains(0) && path.utf8.count <= 2048, "Prefix must be a bounded absolute path")
        let parts = path.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        try require(!parts.isEmpty && parts.count <= 64, "Refusing a filesystem root prefix")
        var current = SafeDirectory(fd: open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), url: URL(fileURLWithPath: "/", isDirectory: true))
        try require(current.fd >= 0, "Cannot open filesystem root")
        var stickyParent = false
        for part in parts {
            try component(part)
            var next = openat(current.fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            if next < 0 && errno == ENOENT && create {
                var parent = stat()
                try require(fstat(current.fd, &parent) == 0 && parent.st_uid == geteuid(), "New prefix parents must belong to the current user")
                guard mkdirat(current.fd, part, 0o700) == 0 else { throw systemFailure("Create prefix directory") }
                next = openat(current.fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            }
            guard next >= 0 else { throw systemFailure("Open prefix directory without symlinks") }
            var info = stat()
            guard fstat(next, &info) == 0 else { close(next); throw systemFailure("Inspect prefix ancestor") }
            let rootSticky = info.st_uid == 0 && (info.st_mode & S_ISVTX) != 0
            if !((info.st_uid == 0 || info.st_uid == geteuid()) && ((info.st_mode & 0o022) == 0 || rootSticky) && (!stickyParent || info.st_uid == geteuid())) {
                close(next); throw InstallError.invalid("Prefix ancestors must be trusted and cannot be writable by other users; a root-owned sticky parent requires an immediately user-owned child")
            }
            stickyParent = rootSticky && (info.st_mode & 0o022) != 0
            current = SafeDirectory(fd: next, url: current.url.appendingPathComponent(part, isDirectory: true))
        }
        try current.checkOwner()
        return current
    }
    func checkOwner() throws {
        var info = stat()
        try require(fstat(fd, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR && info.st_uid == geteuid() && (info.st_mode & 0o022) == 0, "Store directory must belong to the current user and must not be writable by other users")
    }
    func child(_ name: String, create: Bool = false) throws -> SafeDirectory {
        try component(name)
        if create && mkdirat(fd, name, 0o700) != 0 && errno != EEXIST { throw systemFailure("Create store directory") }
        let descriptor = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard descriptor >= 0 else { throw systemFailure("Open store directory") }
        let result = SafeDirectory(fd: descriptor, url: url.appendingPathComponent(name, isDirectory: true))
        try result.checkOwner()
        return result
    }
    func exists(_ name: String) throws -> Bool {
        try component(name)
        var info = stat()
        if fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0 { return true }
        if errno == ENOENT { return false }
        throw systemFailure("Inspect store entry")
    }
    func names() throws -> [String] {
        let copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard copy >= 0 else { throw systemFailure("Duplicate directory descriptor") }
        guard let stream = fdopendir(copy) else { close(copy); throw systemFailure("Read directory") }
        defer { closedir(stream) }
        var names: [String] = []
        errno = 0
        while let item = readdir(stream) {
            let capacity = Int(item.pointee.d_namlen) + 1
            let name = withUnsafePointer(to: &item.pointee.d_name) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: capacity) { String(cString: $0) }
            }
            if name != "." && name != ".." { try component(name); names.append(name) }
            try require(names.count <= 8192, "Store directory entry budget exceeded")
            errno = 0
        }
        if errno != 0 { throw systemFailure("Read directory entries") }
        return names.sorted()
    }
    func parent(of path: String, create: Bool = false) throws -> (SafeDirectory, String) {
        let parts = try pathParts(path)
        var directory = self
        for part in parts.dropLast() { directory = try directory.child(part, create: create) }
        return (directory, parts.last!)
    }
    func read(_ path: String, limit: Int = 128 * 1024 * 1024) throws -> Data {
        let (directory, name) = try parent(of: path)
        let descriptor = openat(directory.fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
        guard descriptor >= 0 else { throw systemFailure("Open tracked file") }
        defer { close(descriptor) }
        return try readDescriptor(descriptor, limit: limit, owned: true)
    }
    func writeNew(_ path: String, data: Data, executable: Bool = false) throws {
        let (directory, name) = try parent(of: path, create: true)
        let descriptor = openat(directory.fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, executable ? 0o700 : 0o600)
        guard descriptor >= 0 else { throw systemFailure("Create new tracked file") }
        defer { close(descriptor) }
        var offset = 0
        try data.withUnsafeBytes { raw in
            while offset < raw.count {
                let count = Darwin.write(descriptor, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw systemFailure("Write tracked file") }
                offset += count
            }
        }
        try require(fsync(descriptor) == 0 && fsync(directory.fd) == 0, "Cannot persist tracked file")
    }
    func mode(_ path: String) throws -> Bool {
        let (directory, name) = try parent(of: path)
        var info = stat()
        try require(fstatat(directory.fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0 && (info.st_mode & S_IFMT) == S_IFREG && info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 0o7022) == 0, "Tracked file ownership, type or permissions changed")
        return info.st_mode & 0o111 != 0
    }
    @discardableResult func move(_ name: String, to destination: SafeDirectory, as newName: String, exclusive: Bool = true) throws -> RenameOutcome {
        try component(name); try component(newName)
        let flags: UInt32 = exclusive ? UInt32(RENAME_EXCL) : 0
        guard renameatx_np(fd, name, destination.fd, newName, flags) == 0 else { throw systemFailure("Atomically rename owned entry") }
        let sourceDurable = persistDirectory(fd), destinationDurable = persistDirectory(destination.fd)
        return RenameOutcome(durable: sourceDurable && destinationDurable)
    }
    func removeFile(_ path: String) throws {
        let (directory, name) = try parent(of: path)
        _ = try directory.mode(name)
        guard unlinkat(directory.fd, name, 0) == 0 else { throw systemFailure("Remove tracked regular file") }
    }
    func tree(prefix: String = "", depth: Int = 0) throws -> (files: Set<String>, directories: Set<String>) {
        try require(depth <= 32, "Store tree depth exceeded")
        var files = Set<String>(), directories = Set<String>()
        for name in try names() {
            let path = prefix.isEmpty ? name : prefix + "/" + name
            _ = try pathParts(path)
            var info = stat()
            try require(fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0 && info.st_uid == geteuid(), "Unowned store entry")
            if (info.st_mode & S_IFMT) == S_IFDIR {
                let childTree = try child(name).tree(prefix: path, depth: depth + 1)
                files.formUnion(childTree.files); directories.formUnion(childTree.directories); directories.insert(path)
            } else {
                try require((info.st_mode & S_IFMT) == S_IFREG && info.st_nlink == 1, "Store links or special files are forbidden")
                files.insert(path)
            }
            try require(files.count + directories.count <= 8192, "Store tree entry budget exceeded")
        }
        return (files, directories)
    }
    // The caller supplies a previously validated, exact owned file set. Never
    // recursively delete an arbitrary user-supplied path.
    func removeExact(files: Set<String>) throws {
        let inventory = try tree()
        try require(inventory.files == files, "Refusing to delete an untracked or modified tree")
        for file in files.sorted() { try removeFile(file) }
        for path in inventory.directories.sorted(by: { $0.count > $1.count }) {
            let (directory, name) = try parent(of: path)
            guard unlinkat(directory.fd, name, AT_REMOVEDIR) == 0 else { throw systemFailure("Remove empty tracked directory") }
        }
    }
}
