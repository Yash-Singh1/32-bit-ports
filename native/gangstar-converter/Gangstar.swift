import Foundation

// Gameloft's Mac App Store release of Gangstar: Miami Vindication (2011).
// The shared Converter (cod4-converter/Conversion.swift) supplies the i386
// C++ runtime download, GLMetal validation and the copy/publish helpers.
enum Gangstar {
    static let bundleID = "com.gameloft.gangstar2mac"
    static let title = "Gangstar: Miami Vindication"
    static let stem = "Gangstar"
}

struct GangstarSource {
    let app: URL
    let info: [String: Any]
    var contents: URL { app.appendingPathComponent("Contents") }
    var resources: URL { contents.appendingPathComponent("Resources") }
    var image: URL { contents.appendingPathComponent("MacOS/" + (info["CFBundleExecutable"] as? String ?? "")) }

    static func isDiskImage(_ url: URL) -> Bool { url.pathExtension.lowercased() == "dmg" }

    /// The original app, or the first matching app inside a chosen folder.
    static func discover(_ selected: URL) throws -> GangstarSource {
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
            guard (info["CFBundleIdentifier"] as? String)?.lowercased() == Gangstar.bundleID else { continue }
            let source = GangstarSource(app: app, info: info)
            try source.validate()
            return source
        }
        throw ConversionError.message("Choose the Mac \(Gangstar.title) app (GangstarMV.app), the folder that contains it, or its disk image.")
    }

    func validate() throws {
        guard let executable = info["CFBundleExecutable"] as? String, !executable.isEmpty,
              executable != ".", executable != "..", !executable.contains("/") else {
            throw ConversionError.message("The Mac game app has an invalid executable entry in Info.plist.")
        }
        for file in [image, resources.appendingPathComponent("_packMac/file000000.dat")] {
            var directory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: file.path, isDirectory: &directory), !directory.boolValue else {
                throw ConversionError.message("The Mac game installation is incomplete: missing \(file.lastPathComponent).")
            }
        }
        _ = try machOSlice(Data(contentsOf: image), cpu: 7)
    }
}

extension Converter {
    /// Attaches a disk image read-only at a private mount point for `body`.
    private func withMountedImage<T>(_ image: URL, _ body: (URL) throws -> T) throws -> T {
        let mount = FileManager.default.temporaryDirectory.appendingPathComponent("gangstar-source-\(UUID().uuidString)")
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

    func convertGangstar(source: URL, destination: URL, useGLMetal: Bool = true) throws -> URL {
        try cancellation.check()
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let destination = destination.resolvingSymlinksInPath().standardizedFileURL
        let converterApp = resources.deletingLastPathComponent().deletingLastPathComponent()
        guard !isInside(destination, source),
              !(converterApp.pathExtension == "app" && isInside(destination, converterApp)) else {
            throw ConversionError.message("Choose an output folder outside the original game and the converter app.")
        }
        if GangstarSource.isDiskImage(source) {
            return try withMountedImage(source) { try convertGangstar(app: GangstarSource.discover($0), destination: destination, useGLMetal: useGLMetal) }
        }
        return try convertGangstar(app: GangstarSource.discover(source), destination: destination, useGLMetal: useGLMetal)
    }

    private func convertGangstar(app layout: GangstarSource, destination: URL, useGLMetal: Bool) throws -> URL {
        let fm = FileManager.default
        guard !isInside(destination, layout.app) else {
            throw ConversionError.message("Choose an output folder outside the original game.")
        }
        progress("Checking \(Gangstar.title)…", "Reading the Mac executable.")
        let gameImage = try machOSlice(Data(contentsOf: layout.image), cpu: 7)
        let loader = resources.appendingPathComponent("game_loader")
        guard fm.isExecutableFile(atPath: loader.path) else {
            throw ConversionError.message("The converter is missing its game launcher. Use a complete copy of Gangstar-Converter.app.")
        }
        // This also reports a missing Rosetta installation before downloading or copying.
        try run("/usr/bin/arch", ["-x86_64", "/usr/bin/true"])
        let metal = useGLMetal ? try validateGLMetal() : nil
        let runtime = try prepareRuntime()
        let temp = destination.appendingPathComponent(".gangstar-converting-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let stem = Gangstar.stem
        let app = temp.appendingPathComponent(stem + "-Compat.app")
        let output = app.appendingPathComponent("Contents")
        for directory in ["MacOS", "SharedSupport"] {
            try fm.createDirectory(at: output.appendingPathComponent(directory), withIntermediateDirectories: true)
        }
        progress("Copying \(Gangstar.title)…", "Creating your app. This creates an independent copy of the game files.")
        // The game reads its packed data, intro movie and MainMenu.nib from Resources.
        try copyGameTree(layout.resources, to: output.appendingPathComponent("Resources"), check: cancellation.check)
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
        // The bundled SDL.framework is not copied: the launcher implements the
        // SDL calls the game makes. CFBundleName stays: the game names its
        // ~/Library folder after it.
        var info = layout.info
        info["CFBundleExecutable"] = stem + "Compat"
        info["CFBundleIdentifier"] = Gangstar.bundleID + ".compat"
        info["CFBundleDisplayName"] = "\(Gangstar.title) (Compatibility)"
        info["LSMinimumSystemVersion"] = "11.0"
        info["NSHighResolutionCapable"] = false
        info["LP32GeneratedGame"] = "gangstar"
        info["LP32GLMetal"] = metal != nil
        info["LP32ContinueWhenInactive"] = false
        var environment = info["LSEnvironment"] as? [String: String] ?? [:]
        environment["LP32_GL_BACKEND"] = metal != nil ? "metal" : "apple"
        environment.removeValue(forKey: "LP32_GLMETAL_PATH")
        info["LSEnvironment"] = environment
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            .write(to: output.appendingPathComponent("Info.plist"))
        progress("Finishing your app…", "Preparing \(Gangstar.title) to open from Finder.")
        try run("/usr/bin/xattr", ["-cr", app.path])
        try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        try run("/usr/bin/codesign", ["--verify", "--strict", app.path])
        try layout.validate()
        try cancellation.check()
        return try publish(app, in: destination, name: stem + "-Compat")
    }
}
