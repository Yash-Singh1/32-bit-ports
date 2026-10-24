import Foundation

// Gameloft's Mac App Store release of Fast Five (2011).
// The shared Converter (cod4-converter/Conversion.swift) supplies the i386
// C++ runtime download, GLMetal validation and the copy/publish helpers.
enum FastFive {
    static let bundleID = "com.gameloft.fastfivemac"
    static let title = "Fast Five"
    static let stem = "FastFive"
}

struct FastFiveSource {
    let app: URL
    let info: [String: Any]
    var contents: URL { app.appendingPathComponent("Contents") }
    var resources: URL { contents.appendingPathComponent("Resources") }
    var image: URL { contents.appendingPathComponent("MacOS/" + (info["CFBundleExecutable"] as? String ?? "")) }
    /// The Mac App Store copy checks its receipt at launch and quits without
    /// it, as in Contents/_MASReceipt/receipt of the original app.
    var receipt: URL? {
        let receipt = contents.appendingPathComponent("_MASReceipt/receipt")
        var directory: ObjCBool = false
        return FileManager.default.fileExists(atPath: receipt.path, isDirectory: &directory) && !directory.boolValue ?
            receipt : nil
    }

    static func isDiskImage(_ url: URL) -> Bool { url.pathExtension.lowercased() == "dmg" }
    static func isArchive(_ url: URL) -> Bool {
        ["zip", "dmg"].contains(url.pathExtension.lowercased())
    }

    /// The original app, or the first matching app inside a chosen folder.
    static func discover(_ selected: URL) throws -> FastFiveSource {
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        var candidates = selected.pathExtension == "app" ? [selected] : []
        if let items = try? FileManager.default.contentsOfDirectory(at: selected, includingPropertiesForKeys: nil) {
            candidates += items.filter { $0.pathExtension == "app" }.sorted { $0.path < $1.path }
        }
        for app in candidates {
            guard let data = try? Data(contentsOf: app.appendingPathComponent("Contents/Info.plist")),
                  let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else { continue }
            if info["LP32GeneratedGame"] != nil {
                throw ConversionError.message("Choose the original game app, rather than an already converted copy.")
            }
            guard (info["CFBundleIdentifier"] as? String)?.lowercased() == FastFive.bundleID else { continue }
            let source = FastFiveSource(app: app, info: info)
            try source.validate()
            return source
        }
        throw ConversionError.message("Choose the Mac \(FastFive.title) app (FastFive.app), the folder that contains it, or its ZIP archive or disk image.")
    }

    func validate() throws {
        guard let executable = info["CFBundleExecutable"] as? String, !executable.isEmpty,
              executable != ".", executable != "..", !executable.contains("/") else {
            throw ConversionError.message("The Mac game app has an invalid executable entry in Info.plist.")
        }
        for file in [image, resources.appendingPathComponent("res/file000000.dat")] {
            var directory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: file.path, isDirectory: &directory), !directory.boolValue,
                  (try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0) > 0 else {
                throw ConversionError.message("The Mac game installation is incomplete: missing \(file.lastPathComponent).")
            }
        }
        _ = try machOSlice(Data(contentsOf: image), cpu: 7)
    }
}

extension Converter {
    private func withExtractedZIP<T>(_ archive: URL, _ body: (URL) throws -> T) throws -> T {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("fastfive-source-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        progress("Opening the ZIP archive...", "Extracting \(archive.lastPathComponent).")
        try run("/usr/bin/ditto", ["-x", "-k", archive.path, directory.path])
        return try body(directory)
    }

    /// Attaches a disk image read-only at a private mount point for `body`.
    private func withMountedImage<T>(_ image: URL, _ body: (URL) throws -> T) throws -> T {
        let mount = FileManager.default.temporaryDirectory.appendingPathComponent("fastfive-source-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: mount, withIntermediateDirectories: false)
        defer {
            if (try? run("/usr/bin/hdiutil", ["detach", mount.path], cancellable: false)) == nil {
                _ = try? run("/usr/bin/hdiutil", ["detach", "-force", mount.path], cancellable: false)
            }
            try? FileManager.default.removeItem(at: mount)
        }
        progress("Opening the disk image…", "Reading \(image.lastPathComponent) without changing it.")
        try run("/usr/bin/hdiutil", ["attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint", mount.path, image.path])
        return try body(mount)
    }

    func convertFastFive(source: URL, destination: URL, useGLMetal: Bool = false) throws -> URL {
        try cancellation.check()
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let destination = destination.resolvingSymlinksInPath().standardizedFileURL
        let converterApp = resources.deletingLastPathComponent().deletingLastPathComponent()
        guard !isInside(destination, source),
              !(converterApp.pathExtension == "app" && isInside(destination, converterApp)) else {
            throw ConversionError.message("Choose an output folder outside the original game and the converter app.")
        }
        if source.pathExtension.lowercased() == "zip" {
            return try withExtractedZIP(source) { try convertFastFive(app: FastFiveSource.discover($0), destination: destination, useGLMetal: useGLMetal) }
        }
        if FastFiveSource.isDiskImage(source) {
            return try withMountedImage(source) { try convertFastFive(app: FastFiveSource.discover($0), destination: destination, useGLMetal: useGLMetal) }
        }
        return try convertFastFive(app: FastFiveSource.discover(source), destination: destination, useGLMetal: useGLMetal)
    }

    private func convertFastFive(app layout: FastFiveSource, destination: URL, useGLMetal: Bool) throws -> URL {
        let fm = FileManager.default
        guard !isInside(destination, layout.app) else {
            throw ConversionError.message("Choose an output folder outside the original game.")
        }
        progress("Checking \(FastFive.title)…", "Reading the Mac executable.")
        let gameImage = try machOSlice(Data(contentsOf: layout.image), cpu: 7)
        let loader = resources.appendingPathComponent("game_loader")
        guard fm.isExecutableFile(atPath: loader.path) else {
            throw ConversionError.message("The converter is missing its game launcher. Use a complete copy of FastFive-Converter.app.")
        }
        try checkRosetta()
        let metal = useGLMetal ? try validateGLMetal() : nil
        let runtime = try prepareRuntime()
        let temp = destination.appendingPathComponent(".fastfive-converting-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let stem = FastFive.stem
        let app = temp.appendingPathComponent(stem + "-Compat.app")
        let output = app.appendingPathComponent("Contents")
        for directory in ["MacOS", "SharedSupport"] {
            try fm.createDirectory(at: output.appendingPathComponent(directory), withIntermediateDirectories: true)
        }
        progress("Copying \(FastFive.title)…", "Creating your app. This creates an independent copy of the game files.")
        // The game reads its packed res/ data and MainWindow_Mac.nib from Resources.
        try copyGameTree(layout.resources, to: output.appendingPathComponent("Resources"), check: cancellation.check)
        // The game reads the receipt from the bundle it runs as.
        if let receipt = layout.receipt {
            try fm.createDirectory(at: output.appendingPathComponent("_MASReceipt"), withIntermediateDirectories: false)
            try fm.copyItem(at: receipt, to: output.appendingPathComponent("_MASReceipt/receipt"))
        }
        let imagePath = output.appendingPathComponent("SharedSupport/" + stem + ".image")
        try gameImage.write(to: imagePath)
        try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: imagePath.path)
        try copyGameTree(runtime, to: output.appendingPathComponent("SharedSupport/compat-runtime"), check: cancellation.check)
        if let metal {
            try copyGameTree(metal, to: output.appendingPathComponent("Frameworks/GLMetal"), check: cancellation.check)
            try fm.copyItem(at: resources.appendingPathComponent("GLMetal-build-info.json"),
                            to: output.appendingPathComponent("Resources/GLMetal-build-info.json"))
        }
        let executable = output.appendingPathComponent("MacOS/" + stem + "Compat")
        try fm.copyItem(at: loader, to: executable)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
        // Keep CFBundleName because the game uses it for its data directory.
        var info = layout.info
        info["CFBundleExecutable"] = stem + "Compat"
        info["CFBundleIdentifier"] = FastFive.bundleID + ".compat"
        info["CFBundleDisplayName"] = "\(FastFive.title) (Compatibility)"
        info["LSMinimumSystemVersion"] = "11.0"
        info["NSHighResolutionCapable"] = false
        info["LP32GeneratedGame"] = "fastfive"
        info["LP32GLMetal"] = metal != nil
        info["LP32ContinueWhenInactive"] = false
        var environment = info["LSEnvironment"] as? [String: String] ?? [:]
        environment["LP32_GL_BACKEND"] = metal != nil ? "metal" : "apple"
        environment.removeValue(forKey: "LP32_GLMETAL_PATH")
        info["LSEnvironment"] = environment
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            .write(to: output.appendingPathComponent("Info.plist"))
        progress("Finishing your app…", "Preparing \(FastFive.title) to open from Finder.")
        try run("/usr/bin/xattr", ["-cr", app.path])
        try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        try run("/usr/bin/codesign", ["--verify", "--strict", app.path])
        try layout.validate()
        try cancellation.check()
        return try publish(app, in: destination, name: stem + "-Compat")
    }
}
