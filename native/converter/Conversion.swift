import Foundation
import CryptoKit
import Darwin

// The two converter apps share the same engine and window. The build target
// selects the game; the Portal 2 app keeps its existing layouts and identity.
#if L4D2_CONVERTER
typealias ConverterSource = L4D2Source
enum ConverterGame {
    static let name = "Left 4 Dead 2"
    static let stem = "L4D2"
    static let executable = "hl2_osx"
    static let sourceDetail = "A complete Mac app or Steam game folder.\nFor Steam, choose Left 4 Dead 2 inside steamapps/common."
    static let pickerMessage = "Choose the complete Left 4 Dead 2 Mac game folder inside steamapps/common, or a Mac app containing it. Separate depots must be assembled first."
}
#else
typealias ConverterSource = Portal2Source
enum ConverterGame {
    static let name = "Portal 2"
    static let stem = "Portal2"
    static let executable = "portal2_osx"
    static let sourceDetail = "A Mac app, Steam game folder, or downloaded depots.\nFor Steam, choose the Portal 2 folder inside steamapps/common."
    static let pickerMessage = "For Steam, choose the Portal 2 folder inside steamapps/common.\nDefault location: ~/Library/Application Support/Steam/steamapps/common/Portal 2\nYou can also choose Portal 2.app or a folder of downloaded depots."
}
#endif

enum ConversionError: LocalizedError {
    case message(String)
    case cancelled

    var errorDescription: String? {
        switch self {
        case .message(let text): return text
        case .cancelled: return "Conversion cancelled."
        }
    }
}

final class Cancellation {
    private let lock = NSLock()
    private var requested = false

    func cancel() {
        lock.lock()
        requested = true
        lock.unlock()
    }

    func check() throws {
        lock.lock()
        let value = requested
        lock.unlock()
        if value { throw ConversionError.cancelled }
    }
}

// Thin the two downloaded libraries without requiring Xcode's lipo on the
// user's Mac. Fat headers are big endian; the i386 Mach-O slice is little endian.
func i386Slice(_ data: Data) throws -> Data {
    func word(_ offset: Int, little: Bool = false) throws -> UInt32 {
        guard offset >= 0, offset <= data.count - 4 else {
            throw ConversionError.message("The downloaded runtime has a truncated Mach-O header.")
        }
        let bytes = Array(data[offset..<(offset + 4)])
        return (little ? Array(bytes.reversed()) : bytes).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    }
    let magic = try word(0)
    if magic == 0xcefaedfe, data.count >= 28, try word(4, little: true) == 7 { return data }
    guard magic == 0xcafebabe else {
        throw ConversionError.message("The downloaded runtime is not an Intel universal library.")
    }
    let count = Int(try word(4))
    guard count <= (data.count - 8) / 20 else {
        throw ConversionError.message("The downloaded runtime has a truncated architecture table.")
    }
    for index in 0..<count {
        let base = 8 + index * 20
        if try word(base) != 7 { continue }
        let offset = Int(try word(base + 8))
        let size = Int(try word(base + 12))
        guard offset >= 8 + count * 20, size >= 28,
              offset <= data.count, size <= data.count - offset else {
            throw ConversionError.message("The downloaded runtime has an invalid Intel slice.")
        }
        let slice = data.subdata(in: offset..<(offset + size))
        guard slice.prefix(4) == Data([0xce, 0xfa, 0xed, 0xfe]),
              slice[4..<8] == Data([7, 0, 0, 0]) else {
            throw ConversionError.message("The downloaded runtime's Intel slice is not i386.")
        }
        return slice
    }
    throw ConversionError.message("The downloaded runtime is missing its 32-bit Intel library.")
}

func isInside(_ child: URL, _ parent: URL) -> Bool {
    let a = child.resolvingSymlinksInPath().standardizedFileURL.pathComponents
    let b = parent.resolvingSymlinksInPath().standardizedFileURL.pathComponents
    return a.starts(with: b)
}

// RENAME_EXCL makes publishing atomic and never replaces a previous game (and
// its saves), even if another conversion finishes at the same moment.
func publish(_ staged: URL, in directory: URL) throws -> URL {
    for suffix in 1...10000 {
        let name = suffix == 1 ? "\(ConverterGame.stem)-Compat.app" : "\(ConverterGame.stem)-Compat \(suffix).app"
        let output = directory.appendingPathComponent(name)
        if renamex_np(staged.path, output.path, UInt32(RENAME_EXCL)) == 0 { return output }
        let code = errno
        if code != EEXIST {
            throw ConversionError.message("Could not save the converted app: \(String(cString: strerror(code)))")
        }
    }
    throw ConversionError.message("This folder already contains too many converted copies. Choose another folder.")
}

final class Converter {
    static let downloadURL = "https://updates.cdn-apple.com/2021/macos/041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg"
    static let installerHash = "db0b2300de719fa3e4ee132b55afd4e689211ad5332760fe5fe7a30351c9e75c"
    static let libraries: [(name: String, member: String, hash: String)] = [
        ("libstdc++.6.dylib", "libstdc++.6.0.9.dylib", "0e68f2c931e50ecf461059a52480610dbb4c8b02a2f28c1f76efee854976fa00"),
        ("libc++abi.dylib", "libc++abi.dylib", "f2c1ebc33f979e7d48a223babcfa679f54f3040390dc022f2beb732ac4acc9af")
    ]
    static var defaultCache: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("org.32bitgoofy.\(ConverterGame.stem)Converter")
    }

    let cancellation = Cancellation()
    let resources: URL
    let cache: URL
    let progress: (String, String) -> Void
    private let fm = FileManager.default

    init(resources: URL, cache: URL = Converter.defaultCache,
         progress: @escaping (String, String) -> Void) {
        self.resources = resources
        self.cache = cache
        self.progress = progress
    }

    func sha256(_ url: URL) throws -> String {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        var digest = SHA256()
        while true {
            try cancellation.check()
            let chunk = try file.read(upToCount: 1024 * 1024) ?? Data()
            if chunk.isEmpty { break }
            digest.update(data: chunk)
        }
        return digest.finalize().map { String(format: "%02x", $0) }.joined()
    }

    @discardableResult
    func run(_ executable: String, _ arguments: [String], cwd: URL? = nil,
             cancellable: Bool = true, tick: (() -> Void)? = nil) throws -> String {
        if cancellable { try cancellation.check() }
        let log = fm.temporaryDirectory.appendingPathComponent("portal2-command-\(UUID().uuidString).log")
        fm.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600])
        defer { try? fm.removeItem(at: log) }
        let output = try FileHandle(forWritingTo: log)
        defer { try? output.close() }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.currentDirectoryURL = cwd
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = output
        process.standardError = output
        try process.run()
        var lastTick = Date.distantPast
        while process.isRunning {
            if cancellable {
                do { try cancellation.check() } catch {
                    process.terminate()
                    let deadline = Date().addingTimeInterval(3)
                    while process.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
                    if process.isRunning { kill(process.processIdentifier, SIGKILL) }
                    process.waitUntilExit()
                    throw error
                }
            }
            if Date().timeIntervalSince(lastTick) > 0.5 {
                tick?()
                lastTick = Date()
            }
            Thread.sleep(forTimeInterval: 0.1)
        }
        process.waitUntilExit()
        if cancellable { try cancellation.check() }
        let reader = try FileHandle(forReadingFrom: log)
        defer { try? reader.close() }
        let size = try reader.seekToEnd()
        try reader.seek(toOffset: size > 4096 ? size - 4096 : 0)
        let text = String(decoding: try reader.readToEnd() ?? Data(), as: UTF8.self)
        guard process.terminationStatus == 0 else {
            throw ConversionError.message("\(URL(fileURLWithPath: executable).lastPathComponent) could not finish.\n\n\(text.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
        return text
    }

    private func mounted(_ image: URL, at mount: URL, body: () throws -> Void) throws {
        // Even a cancelled attach may already have mounted the image. Always
        // detach our private mount point before removing the extraction folder.
        defer {
            if (try? run("/usr/bin/hdiutil", ["detach", mount.path], cancellable: false)) == nil {
                _ = try? run("/usr/bin/hdiutil", ["detach", "-force", mount.path], cancellable: false)
            }
        }
        try run("/usr/bin/hdiutil", ["attach", "-readonly", "-nobrowse", "-mountpoint", mount.path, image.path])
        try body()
    }

    func prepareRuntime() throws -> URL {
        try fm.createDirectory(at: cache, withIntermediateDirectories: true)
        let lock = open(cache.appendingPathComponent("runtime.lock").path, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
        guard lock >= 0 else { throw ConversionError.message("Could not open the compatibility download cache.") }
        defer { flock(lock, LOCK_UN); close(lock) }
        while flock(lock, LOCK_EX | LOCK_NB) != 0 {
            guard errno == EWOULDBLOCK else { throw ConversionError.message("Could not lock the compatibility download cache.") }
            try cancellation.check()
            progress("Waiting for another conversion…", "Compatibility files are being prepared by another copy of this app.")
            Thread.sleep(forTimeInterval: 0.2)
        }
        try cancellation.check()
        let runtime = cache.appendingPathComponent("runtime")
        if try Self.libraries.allSatisfy({ library in
            let url = runtime.appendingPathComponent(library.name)
            guard fm.fileExists(atPath: url.path) else { return false }
            return try sha256(url) == library.hash
        }) { return runtime }

        let installer = cache.appendingPathComponent("InstallMacOSX-Lion.dmg")
        progress("Preparing compatibility files…", "The first conversion downloads 4.72 GB from Apple. Later conversions reuse it.")
        if fm.fileExists(atPath: installer.path), try sha256(installer) != Self.installerHash {
            try fm.removeItem(at: installer)
        }
        if !fm.fileExists(atPath: installer.path) {
            let partial = cache.appendingPathComponent("download-\(UUID().uuidString).dmg")
            defer { try? fm.removeItem(at: partial) }
            try run("/usr/bin/curl", ["--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                                     "--connect-timeout", "30", "--speed-limit", "1024", "--speed-time", "120",
                                     "--retry", "3", "--output", partial.path, Self.downloadURL], tick: {
                let size = (try? self.fm.attributesOfItem(atPath: partial.path)[.size] as? NSNumber)?.int64Value ?? 0
                let downloaded = ByteCountFormatter.string(fromByteCount: size, countStyle: .file)
                self.progress("Downloading compatibility files…", "\(downloaded) of 4.72 GB · from Apple · first conversion only")
            })
            progress("Preparing compatibility files…", "Finishing the download. This can take a moment.")
            guard try sha256(partial) == Self.installerHash else {
                throw ConversionError.message("The Apple download was incomplete or changed. Please try again.")
            }
            try fm.moveItem(at: partial, to: installer)
        }

        progress("Extracting compatibility files…", "Preparing two libraries from the download. Nothing is installed on your Mac.")
        let temp = cache.appendingPathComponent("extract-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let packageImage = temp.appendingPathComponent("InstallMacOSX.pkg/InstallESD.dmg")
        try mounted(installer, at: temp.appendingPathComponent("installer")) {
            try run("/usr/bin/xar", ["-xf", temp.appendingPathComponent("installer/InstallMacOSX.pkg").path,
                                    "InstallMacOSX.pkg/InstallESD.dmg"], cwd: temp)
        }
        try mounted(packageImage, at: temp.appendingPathComponent("esd")) {
            try run("/usr/bin/xar", ["-xf", temp.appendingPathComponent("esd/Packages/BaseSystemBinaries.pkg").path,
                                    "Payload"], cwd: temp)
        }
        try run("/usr/bin/tar", ["-xf", temp.appendingPathComponent("Payload").path] +
                Self.libraries.map { "./usr/lib/" + $0.member }, cwd: temp)
        let staged = temp.appendingPathComponent("runtime")
        try fm.createDirectory(at: staged, withIntermediateDirectories: false)
        for library in Self.libraries {
            try cancellation.check()
            let data = try Data(contentsOf: temp.appendingPathComponent("usr/lib/" + library.member))
            let thin = staged.appendingPathComponent(library.name)
            try i386Slice(data).write(to: thin)
            guard try sha256(thin) == library.hash else {
                throw ConversionError.message("The downloaded compatibility library \(library.name) did not match the expected version.")
            }
        }
        try "Extracted locally from Apple's Lion archive.\n\(Self.downloadURL)\nArchive SHA-256: \(Self.installerHash)\n"
            .write(to: staged.appendingPathComponent("SOURCE.txt"), atomically: true, encoding: .utf8)
        if fm.fileExists(atPath: runtime.path) { try fm.removeItem(at: runtime) }
        try fm.moveItem(at: staged, to: runtime)
        return runtime
    }

    func convert(source: URL, destination: URL) throws -> URL {
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let destination = destination.resolvingSymlinksInPath().standardizedFileURL
        let converterApp = resources.deletingLastPathComponent().deletingLastPathComponent()
        guard !isInside(destination, source),
              !(converterApp.pathExtension == "app" && isInside(destination, converterApp)) else {
            throw ConversionError.message("Choose an output folder outside the original game and the converter app.")
        }
        let layout = try ConverterSource.discover(source)
        try layout.checkDestination(destination)
        let image = layout.image
        try cancellation.check()
        progress("Preparing compatibility files…", "Your original game files and existing converted copies stay untouched.")
        let runtime = try prepareRuntime()
        let temp = destination.appendingPathComponent(".\(ConverterGame.stem.lowercased())-converting-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let app = temp.appendingPathComponent("\(ConverterGame.stem)-Compat.app")
        let output = app.appendingPathComponent("Contents")
        for directory in ["MacOS", "Resources", "SharedSupport"] {
            try fm.createDirectory(at: output.appendingPathComponent(directory), withIntermediateDirectories: true)
        }
        progress("Copying \(ConverterGame.name)…", "Copying the game files and settings into your new app.")
        try layout.copyGame(to: output.appendingPathComponent("SharedSupport/\(ConverterGame.stem)"), check: cancellation.check)
        #if L4D2_CONVERTER
        if let icon = layout.icon {
            try Data(contentsOf: icon).write(to: output.appendingPathComponent("Resources/game.icns"))
        }
        #else
        if let resources = layout.resources {
            try run("/usr/bin/ditto", ["--noextattr", "--noqtn", resources.path,
                                      output.appendingPathComponent("Resources").path])
        } else if let icon = layout.icon {
            try fm.copyItem(at: icon, to: output.appendingPathComponent("Resources/game.icns"))
        }
        #endif
        try cancellation.check()
        #if L4D2_CONVERTER
        try Data(contentsOf: image).write(to: output.appendingPathComponent("SharedSupport/\(ConverterGame.stem).image"))
        #else
        try fm.copyItem(at: image, to: output.appendingPathComponent("SharedSupport/\(ConverterGame.stem).image"))
        #endif
        try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: output.appendingPathComponent("SharedSupport/\(ConverterGame.stem).image").path)
        let runtimeOutput = output.appendingPathComponent("SharedSupport/\(ConverterGame.stem)/compat-runtime")
        // Remove only the new copy's old entry (including a symlink), before
        // copying private libraries. Never follow an input link when writing.
        if (try? fm.attributesOfItem(atPath: runtimeOutput.path)) != nil { try fm.removeItem(at: runtimeOutput) }
        try run("/usr/bin/ditto", ["--noextattr", "--noqtn", runtime.path, runtimeOutput.path])
        try fm.copyItem(at: resources.appendingPathComponent("game_loader"), to: output.appendingPathComponent("MacOS/\(ConverterGame.stem)Compat"))
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: output.appendingPathComponent("MacOS/\(ConverterGame.stem)Compat").path)
        try fm.copyItem(at: resources.appendingPathComponent("Info-\(ConverterGame.stem).plist"), to: output.appendingPathComponent("Info.plist"))
        progress("Finishing your app…", "Preparing \(ConverterGame.name) to open from Finder.")
        try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
        try cancellation.check()
        return try publish(app, in: destination)
    }
}
