import Foundation
import Darwin

final class ConversionCancellation {
    private let lock = NSLock()
    private var requested = false
    func cancel() { lock.lock(); requested = true; lock.unlock() }
    func check() throws {
        lock.lock(); let value = requested; lock.unlock()
        if value { throw ConverterError.cancelled }
    }
}

struct ConversionResult {
    let app: URL
    let game: LEGOGame
    let log: URL
    var hasSeparateData: Bool { game == .sagaSteam || game == .sagaRetail }
}

final class LEGOConverter {
    let resources: URL
    let cancellation = ConversionCancellation()
    let log: URL
    let progress: (Double, String, String) -> Void
    private let fm = FileManager.default
    private var lastCopyUpdate = Date.distantPast
    private var logHandle: FileHandle?

    init(resources: URL, logDirectory: URL? = nil,
         progress: @escaping (Double, String, String) -> Void) throws {
        self.resources = resources
        self.progress = progress
        let directory = logDirectory ?? FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/LEGOConverter")
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        log = directory.appendingPathComponent("conversion-\(UUID().uuidString).log")
        fm.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600])
        logHandle = try FileHandle(forWritingTo: log)
        record("LEGOConverter 1.0\nmacOS \(ProcessInfo.processInfo.operatingSystemVersionString)\n")
    }
    deinit { try? logHandle?.close() }
    private func record(_ line: String) {
        try? logHandle?.write(contentsOf: Data((line + "\n").utf8))
    }
    func recordFailure(_ error: Error) { record("Conversion failed: " + error.localizedDescription) }

    @discardableResult
    func run(_ executable: URL, _ arguments: [String], cwd: URL? = nil) throws -> String {
        try cancellation.check()
        record("$ " + ([executable.path] + arguments).map { String(reflecting: $0) }.joined(separator: " "))
        let capture = fm.temporaryDirectory.appendingPathComponent("lego-converter-command-\(UUID().uuidString).log")
        fm.createFile(atPath: capture.path, contents: nil, attributes: [.posixPermissions: 0o600])
        defer { try? fm.removeItem(at: capture) }
        let output = try FileHandle(forWritingTo: capture)
        defer { try? output.close() }
        let process = Process()
        process.executableURL = resources.appendingPathComponent("Tools/RunTool")
        process.arguments = [executable.path] + arguments
        process.currentDirectoryURL = cwd
        var environment = ProcessInfo.processInfo.environment
        for key in ["DEVELOPER_DIR", "TOOLCHAINS", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES"] {
            environment.removeValue(forKey: key)
        }
        environment["PATH"] = resources.appendingPathComponent("Toolchain/bin").path + ":/usr/bin:/bin:/usr/sbin:/sbin"
        environment["SDKROOT"] = resources.appendingPathComponent("Toolchain/SDK").path
        process.environment = environment
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = output; process.standardError = output
        try process.run()
        var cancelled = false
        while process.isRunning {
            do { try cancellation.check() } catch { cancelled = true; break }
            Thread.sleep(forTimeInterval: 0.05)
        }
        if cancelled {
            if getpgid(process.processIdentifier) == process.processIdentifier {
                kill(-process.processIdentifier, SIGTERM)
            } else { process.terminate() }
            let deadline = Date().addingTimeInterval(2)
            while process.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
            if getpgid(process.processIdentifier) == process.processIdentifier { kill(-process.processIdentifier, SIGKILL) }
            else if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        }
        process.waitUntilExit()
        let reader = try FileHandle(forReadingFrom: capture)
        defer { try? reader.close() }
        let size = try reader.seekToEnd()
        try reader.seek(toOffset: size > 65536 ? size - 65536 : 0)
        let text = String(decoding: try reader.readToEnd() ?? Data(), as: UTF8.self)
        record(text)
        if cancelled { throw ConverterError.cancelled }
        try cancellation.check()
        guard process.terminationReason == .exit, process.terminationStatus == 0 else {
            throw ConverterError.message("\(executable.lastPathComponent) could not finish.\n" +
                String(text.suffix(2500)).trimmingCharacters(in: .whitespacesAndNewlines))
        }
        return text
    }

    private func copy(_ source: URL, _ destination: URL) throws {
        let root = source.resolvingSymlinksInPath()
        try copyNode(root, destination, root: root)
    }

    private func copyNode(_ source: URL, _ destination: URL, root: URL) throws {
        try cancellation.check()
        let attributes = try fm.attributesOfItem(atPath: source.path)
        let type = attributes[.type] as? FileAttributeType
        if type == .typeSymbolicLink {
            let target = source.resolvingSymlinksInPath()
            let link = try fm.destinationOfSymbolicLink(atPath: source.path)
            guard pathIsInside(target, root), !link.hasPrefix("/") else {
                throw ConverterError.message("The game contains a link outside its installation: \(source.lastPathComponent). Choose a complete local copy.")
            }
            try fm.createSymbolicLink(atPath: destination.path, withDestinationPath: link)
        } else if type == .typeDirectory {
            try fm.createDirectory(at: destination, withIntermediateDirectories: true,
                                   attributes: [.posixPermissions: 0o755])
            for child in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
                try copyNode(child, destination.appendingPathComponent(child.lastPathComponent), root: root)
            }
        } else if type == .typeRegular {
            if clonefile(source.path, destination.path, 0) != 0 {
                if (attributes[.size] as? NSNumber)?.intValue ?? 0 > 16 * 1024 * 1024 {
                    try run(URL(fileURLWithPath: "/usr/bin/ditto"), ["--noqtn", source.path, destination.path])
                } else { try fm.copyItem(at: source, to: destination) }
            }
            let mode = (attributes[.posixPermissions] as? NSNumber)?.intValue ?? 0o644
            try fm.setAttributes([.posixPermissions: mode | 0o600], ofItemAtPath: destination.path)
            if Date().timeIntervalSince(lastCopyUpdate) > 0.15 {
                lastCopyUpdate = Date()
                progress(0.5, "Copying your game", source.lastPathComponent)
            }
        } else {
            throw ConverterError.message("The installation contains an unsupported file: \(source.lastPathComponent).")
        }
    }

    private func write(_ data: Data, to url: URL) throws {
        try data.write(to: url, options: .atomic)
        try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: url.path)
    }

    private func repairFramework(_ framework: URL) throws {
        let version = framework.appendingPathComponent("Versions/A")
        guard fm.fileExists(atPath: version.path) else { return }
        let name = framework.deletingPathExtension().lastPathComponent
        for (link, target) in [("Versions/Current", "A"), (name, "Versions/Current/" + name),
                               ("Resources", "Versions/Current/Resources")] {
            let path = framework.appendingPathComponent(link)
            if (try? fm.attributesOfItem(atPath: path.path)) == nil &&
                (link != "Resources" || fm.fileExists(atPath: version.appendingPathComponent("Resources").path)) {
                try fm.createSymbolicLink(atPath: path.path, withDestinationPath: target)
            }
        }
    }

    private func flattenResources(_ app: URL) throws {
        let keys: [URLResourceKey] = [.isSymbolicLinkKey, .fileSizeKey]
        guard let items = fm.enumerator(at: app, includingPropertiesForKeys: keys) else { return }
        for case let item as URL in items {
            try cancellation.check()
            let values = try item.resourceValues(forKeys: Set(keys))
            if values.isSymbolicLink == true { continue }
            let size = getxattr(item.path, "com.apple.ResourceFork", nil, 0, 0, XATTR_NOFOLLOW)
            guard size > 0 else { continue }
            var bytes = Data(count: size)
            let read = bytes.withUnsafeMutableBytes {
                getxattr(item.path, "com.apple.ResourceFork", $0.baseAddress, size, 0, XATTR_NOFOLLOW)
            }
            guard read == size else { throw ConverterError.message("Could not preserve a classic Mac resource fork.") }
            let target = values.fileSize == 0 ? item : URL(fileURLWithPath: item.path + ".lp32-rsrc")
            if target != item, fm.fileExists(atPath: target.path), try Data(contentsOf: target) != bytes {
                throw ConverterError.message("The game contains conflicting resource data for \(item.lastPathComponent).")
            }
            try write(bytes, to: target)
        }
    }

    private func installControllerProfiles(_ resources: URL) throws {
        let folder = resources.appendingPathComponent("InputDevices/AnalogTriggers")
        guard let bytes = try? Data(contentsOf: folder.appendingPathComponent("PS4Dualshock.plist")),
              let original = try PropertyListSerialization.propertyList(from: bytes, format: nil) as? [String: Any],
              original["VendorID"] as? Int == 1356, original["ProductID"] as? Int == 1476 else { return }
        for (name, vendor, product, title) in [("PS4DualshockV2.plist", 1356, 2508, "DualShock 4 (v2)"),
            ("LP32StandardGamepad.plist", 0x7f32, 0x7f32, "Standard Gamepad")] {
            let url = folder.appendingPathComponent(name)
            if fm.fileExists(atPath: url.path) { continue }
            var info = original
            info["VendorID"] = vendor; info["ProductID"] = product
            info["CGPDisplayNameOvr"] = title; info["ButtonBack"] = "9:9"
            if vendor == 0x7f32 { info["CGPDeviceType"] = "Xbox" }
            try write(PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0), to: url)
        }
    }

    func rebuildRuntime(in destination: URL) throws -> URL {
        let toolchain = resources.appendingPathComponent("Toolchain")
        guard let manifest = try JSONSerialization.jsonObject(with:
            Data(contentsOf: toolchain.appendingPathComponent("Build.json"))) as? [String: Any],
              let sources = manifest["sources"] as? [String], let frameworks = manifest["frameworks"] as? [String],
              let minimum = manifest["minimumCompilerOS"] as? Int,
              let resourceVersion = manifest["resourceVersion"] as? String else {
            throw ConverterError.message("The bundled compiler is incomplete. Obtain a complete copy of LEGOConverter.")
        }
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= minimum else {
            throw ConverterError.message("Runtime rebuilding needs macOS \(minimum) or later with this bundled compiler. Normal conversion uses the included runtime and works on macOS 11 or later.")
        }
        try fm.createDirectory(at: destination, withIntermediateDirectories: true)
        let clang = toolchain.appendingPathComponent("bin/clang")
        let sdk = toolchain.appendingPathComponent("SDK")
        let source = toolchain.appendingPathComponent("Sources")
        let common = ["-arch", "x86_64", "-isysroot", sdk.path, "-mmacosx-version-min=11.0",
                      "-O2", "-g0", "-Wall", "-Wextra", "-Werror", "-resource-dir",
                      toolchain.appendingPathComponent("lib/clang/" + resourceVersion).path]
        progress(0.08, "Building the compatibility runtime", "Using the compiler included with LEGOConverter")
        let object = destination.appendingPathComponent("libcpp_stream_bridge.o")
        try run(clang, common + ["-std=c++11", "-isystem", sdk.appendingPathComponent("usr/include/c++/v1").path,
                                "-c", source.appendingPathComponent("libcpp_stream_bridge.cpp").path, "-o", object.path])
        let output = destination.appendingPathComponent("game_loader")
        var args = common + ["--ld-path=" + toolchain.appendingPathComponent("bin/ld").path,
            "-Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000,-segaddr,__LEGACY,0x1000,-segprot,__LEGACY,rwx,rwx"]
        args += sources.map { source.appendingPathComponent($0).path }
        args += [object.path, "-lc++", "-lffi", "-lcurl", "-lz"]
        for framework in frameworks { args += ["-framework", framework] }
        args += ["-o", output.path]
        try run(clang, args)
        try run(URL(fileURLWithPath: "/usr/bin/codesign"), ["--force", "--sign", "-", output.path])
        return output
    }

    func convert(source: URL, destination: URL, game: LEGOGame?, dataFolder: URL? = nil,
                 continueWhenInactive: Bool = false, rebuild: Bool = false) throws -> ConversionResult {
        try cancellation.check()
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let destination = destination.resolvingSymlinksInPath().standardizedFileURL
        let converterApp = resources.deletingLastPathComponent().deletingLastPathComponent()
        guard !pathIsInside(destination, source),
              !(converterApp.pathExtension == "app" && pathIsInside(destination, converterApp)),
              dataFolder.map({ !pathIsInside(destination, $0) }) ?? true else {
            throw ConverterError.message("Choose an output folder outside the original game, its data folder, and LEGOConverter.")
        }
        record("Source: \(source.path)\nDestination: \(destination.path)")
        progress(0.02, "Checking your game", "Reading the original Mac executable")
        let inspection = try SourceInspection.read(source)
        guard let selected = game ?? inspection.detected else {
            throw ConverterError.message("This game could not be identified. Choose its title in the Game menu.")
        }
        let layout = try inspection.layout(for: selected, dataOverride: dataFolder)
        record("Game: \(selected.rawValue)\nExecutable: \(layout.image.path)")
        if let data = layout.data, pathIsInside(destination, data) {
            throw ConverterError.message("Choose an output folder outside the original game's data folder.")
        }
        try fm.createDirectory(at: destination, withIntermediateDirectories: true)
        let staging = destination.appendingPathComponent(".lego-converter-\(UUID().uuidString)")
        try fm.createDirectory(at: staging, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: staging) }
        let loader = rebuild ? try rebuildRuntime(in: staging.appendingPathComponent("runtime")) :
            resources.appendingPathComponent("Runtime/game_loader")
        guard fm.isExecutableFile(atPath: loader.path) else {
            throw ConverterError.message("LEGOConverter is missing its compatibility runtime. Obtain a complete copy of the converter.")
        }
        try run(URL(fileURLWithPath: "/usr/bin/codesign"), ["--verify", "--strict", loader.path])
        let hasData = layout.data != nil
        let container = hasData ? staging.appendingPathComponent(selected.outputStem) : staging
        let app = container.appendingPathComponent(selected.outputStem + ".app")
        let contents = app.appendingPathComponent("Contents")
        for folder in ["MacOS", "Resources", "SharedSupport", "Library/Frameworks"] {
            try fm.createDirectory(at: contents.appendingPathComponent(folder), withIntermediateDirectories: true)
        }
        let image = contents.appendingPathComponent("SharedSupport/\(selected.stem).image")
        progress(0.15, "Preparing the game executable", layout.imageData.packedPirates ?
                 "Unpacking Pirates with the included compatibility tools" : "Keeping the original game code")
        if layout.imageData.packedPirates {
            let packed = staging.appendingPathComponent("pirates-packed.macbin")
            try write(layout.imageData.data, to: packed)
            try run(resources.appendingPathComponent("Tools/UnpackPirates"), [packed.path, image.path])
            guard selected.matches(try MachOImage(data: Data(contentsOf: image))) else {
                throw ConverterError.message("The unpacked Pirates executable is not a supported version.")
            }
        } else { try write(layout.imageData.data, to: image) }
        progress(0.25, "Copying your game", "Creating a separate app from your installation")
        try fm.removeItem(at: contents.appendingPathComponent("Resources"))
        try copy(layout.resources, contents.appendingPathComponent("Resources"))
        if let cg = layout.cg {
            let target = contents.appendingPathComponent("Library/Frameworks/Cg.framework")
            try copy(cg, target); try repairFramework(target)
        }
        let sourceContents = inspection.app.appendingPathComponent("Contents")
        for folder in ["PlugIns", "Plugins"] {
            let original = sourceContents.appendingPathComponent(folder)
            if fm.fileExists(atPath: original.path) {
                try copy(original, contents.appendingPathComponent("PlugIns")); break
            }
        }
        if selected == .sagaSteam {
            let target = contents.appendingPathComponent("Frameworks")
            try copy(sourceContents.appendingPathComponent("Frameworks"), target)
            for framework in try fm.contentsOfDirectory(at: target, includingPropertiesForKeys: nil)
                where framework.pathExtension == "framework" { try repairFramework(framework) }
            try installControllerProfiles(contents.appendingPathComponent("Resources"))
        }
        if let steam = layout.steam {
            let thin = try MachOImage.slice(Data(contentsOf: steam), cpu: 0x01000007)
            let target = contents.appendingPathComponent("Resources/libsteam_api.dylib")
            try write(thin, to: target)
            try run(URL(fileURLWithPath: "/usr/bin/codesign"), ["--force", "--sign", "-", target.path])
            if selected == .sagaSteam {
                try write(thin, to: contents.appendingPathComponent("Frameworks/libsteam_api.dylib"))
                try run(URL(fileURLWithPath: "/usr/bin/codesign"), ["--force", "--sign", "-", contents.appendingPathComponent("Frameworks/libsteam_api.dylib").path])
            }
        }
        if selected == .pirates {
            for name in ["GAME.DAT", "GAME0.DAT", "GAME1.DAT", "GAME2.DAT", "GAME3.DAT", "GAME4.DAT"] {
                try copy(sourceContents.appendingPathComponent("MacOS/" + name), contents.appendingPathComponent("MacOS/" + name))
            }
        }
        let steamID = selected == .sagaSteam ? "32440" :
            (selected == .marvel ? "249130" : nil)
        if let steamID = steamID {
            try write(Data((steamID + "\n").utf8), to: contents.appendingPathComponent("MacOS/steam_appid.txt"))
        }
        if let data = layout.data {
            try copy(data, container.appendingPathComponent(selected == .sagaSteam ? "LEGOStarWarsSagaData" : "Data"))
        }
        try copy(loader, contents.appendingPathComponent("MacOS/\(selected.stem)Compat"))
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: contents.appendingPathComponent("MacOS/\(selected.stem)Compat").path)
        guard let template = try PropertyListSerialization.propertyList(from:
            Data(contentsOf: resources.appendingPathComponent("Templates/" + selected.template)), format: nil) as? [String: Any] else {
            throw ConverterError.message("LEGOConverter is missing the game bundle templates.")
        }
        var info = inspection.info.merging(template) { _, new in new }
        if let originalIcon = inspection.info["CFBundleIconFile"] as? String {
            let name = originalIcon.hasSuffix(".icns") ? originalIcon : originalIcon + ".icns"
            if fm.fileExists(atPath: contents.appendingPathComponent("Resources/" + name).path) { info["CFBundleIconFile"] = name }
        }
        if selected == .sagaSteam {
            info["CFBundleIdentifier"] = "com.feralinteractive.saga.steam.compat"
            info["CFBundleShortVersionString"] = "1.2.1-compat"
        }
        info["LP32ContinueWhenInactive"] = continueWhenInactive
        info["LEGOConverterConversion"] = ["version": "1.0", "game": selected.rawValue,
            "created": ISO8601DateFormatter().string(from: Date())]
        try write(PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0),
                  to: contents.appendingPathComponent("Info.plist"))
        progress(0.85, "Finishing your app", "Preparing the converted game to open in Finder")
        if selected == .sagaRetail { try flattenResources(app) }
        try run(URL(fileURLWithPath: "/usr/bin/xattr"), ["-cr", app.path])
        try run(URL(fileURLWithPath: "/usr/bin/codesign"), ["--force", "--deep", "--sign", "-", app.path])
        try run(URL(fileURLWithPath: "/usr/bin/codesign"), ["--verify", "--deep", "--strict", app.path])
        try cancellation.check()
        let item = hasData ? container : app
        let ext = hasData ? "" : ".app"
        for i in 1...10000 {
            let name = selected.outputStem + (i == 1 ? "" : " \(i)") + ext
            let output = destination.appendingPathComponent(name)
            if renamex_np(item.path, output.path, UInt32(RENAME_EXCL)) == 0 {
                let result = hasData ? output.appendingPathComponent(app.lastPathComponent) : output
                record("Converted app: \(result.path)")
                progress(1, "Your game is ready", result.lastPathComponent)
                return ConversionResult(app: result, game: selected, log: log)
            }
            guard errno == EEXIST else {
                throw ConverterError.message("Could not save the converted game: \(String(cString: strerror(errno)))")
            }
        }
        throw ConverterError.message("This folder contains too many converted copies. Choose another output folder.")
    }
}
