import Foundation

// Gameloft's Mac App Store release of Asphalt 6: Adrenaline (2011).
// The shared Converter (cod4-converter/Conversion.swift) supplies the i386
// C++ runtime download, GLMetal validation and the copy/publish helpers.
enum Asphalt6 {
    static let bundleID = "com.gameloft.asphalt6mac"
    static let title = "Asphalt 6: Adrenaline"
    static let stem = "Asphalt6"
}

struct Asphalt6Source {
    let app: URL
    let info: [String: Any]
    var contents: URL { app.appendingPathComponent("Contents") }
    var resources: URL { contents.appendingPathComponent("Resources") }
    var image: URL { contents.appendingPathComponent("MacOS/" + (info["CFBundleExecutable"] as? String ?? "")) }

    static func isDiskImage(_ url: URL) -> Bool { url.pathExtension.lowercased() == "dmg" }
    static func isArchive(_ url: URL) -> Bool {
        ["zip", "dmg"].contains(url.pathExtension.lowercased())
    }

    /// The original app, or the first matching app inside a chosen folder.
    static func discover(_ selected: URL) throws -> Asphalt6Source {
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
            guard (info["CFBundleIdentifier"] as? String)?.lowercased() == Asphalt6.bundleID else { continue }
            let source = Asphalt6Source(app: app, info: info)
            try source.validate()
            return source
        }
        throw ConversionError.message("Choose the Mac \(Asphalt6.title) app (Asphalt 6.app), the folder that contains it, or its ZIP archive or disk image.")
    }

    func validate() throws {
        guard let executable = info["CFBundleExecutable"] as? String, !executable.isEmpty,
              executable != ".", executable != "..", !executable.contains("/") else {
            throw ConversionError.message("The Mac game app has an invalid executable entry in Info.plist.")
        }
        for file in [image, resources.appendingPathComponent("data/file000000.dat")] {
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
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("asphalt6-source-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        progress("Opening the ZIP archive...", "Extracting \(archive.lastPathComponent).")
        try run("/usr/bin/ditto", ["-x", "-k", archive.path, directory.path])
        return try body(directory)
    }

    /// Attaches a disk image read-only at a private mount point for `body`.
    private func withMountedImage<T>(_ image: URL, _ body: (URL) throws -> T) throws -> T {
        let mount = FileManager.default.temporaryDirectory.appendingPathComponent("asphalt6-source-\(UUID().uuidString)")
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

    func convertAsphalt6(source: URL, destination: URL, useGLMetal: Bool = false, ipadIPA: URL? = nil) throws -> URL {
        try cancellation.check()
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let destination = destination.resolvingSymlinksInPath().standardizedFileURL
        guard source.pathExtension.lowercased() != "ipa" else {
            throw ConversionError.message("The iPad IPA supplies optional textures. Choose the original Mac game as the source and pass the IPA with --ipad-assets.")
        }
        let converterApp = resources.deletingLastPathComponent().deletingLastPathComponent()
        if let ipadIPA {
            guard ipadIPA.pathExtension.lowercased() == "ipa", FileManager.default.isReadableFile(atPath: ipadIPA.path) else {
                throw ConversionError.message("Choose a readable Asphalt 6: Adrenaline HD 1.5.1 iPad IPA.")
            }
        }
        guard !isInside(destination, source),
              !(converterApp.pathExtension == "app" && isInside(destination, converterApp)) else {
            throw ConversionError.message("Choose an output folder outside the original game and the converter app.")
        }
        if source.pathExtension.lowercased() == "zip" {
            return try withExtractedZIP(source) { try convertAsphalt6(app: Asphalt6Source.discover($0), destination: destination, useGLMetal: useGLMetal, ipadIPA: ipadIPA) }
        }
        if Asphalt6Source.isDiskImage(source) {
            return try withMountedImage(source) { try convertAsphalt6(app: Asphalt6Source.discover($0), destination: destination, useGLMetal: useGLMetal, ipadIPA: ipadIPA) }
        }
        return try convertAsphalt6(app: Asphalt6Source.discover(source), destination: destination, useGLMetal: useGLMetal, ipadIPA: ipadIPA)
    }

    private func convertAsphalt6(app layout: Asphalt6Source, destination: URL, useGLMetal: Bool, ipadIPA: URL?) throws -> URL {
        let fm = FileManager.default
        guard !isInside(destination, layout.app) else {
            throw ConversionError.message("Choose an output folder outside the original game.")
        }
        progress("Checking \(Asphalt6.title)…", "Reading the Mac executable.")
        let gameImage = try machOSlice(Data(contentsOf: layout.image), cpu: 7)
        let loader = resources.appendingPathComponent("game_loader")
        guard fm.isExecutableFile(atPath: loader.path) else {
            throw ConversionError.message("The converter is missing its game launcher. Use a complete copy of Asphalt6-Converter.app.")
        }
        try checkRosetta()
        let metal = useGLMetal ? try validateGLMetal() : nil
        let runtime = try prepareRuntime()
        let temp = destination.appendingPathComponent(".asphalt6-converting-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let stem = Asphalt6.stem
        let app = temp.appendingPathComponent(stem + "-Compat.app")
        let output = app.appendingPathComponent("Contents")
        for directory in ["MacOS", "SharedSupport"] {
            try fm.createDirectory(at: output.appendingPathComponent(directory), withIntermediateDirectories: true)
        }
        progress("Copying \(Asphalt6.title)…", "Creating your app. This creates an independent copy of the game files.")
        // The game reads its packed data, intro movie and MainMenu.nib from Resources.
        try copyGameTree(layout.resources, to: output.appendingPathComponent("Resources"), check: cancellation.check)
        var ipadImport: [String: Any]?
        if let ipadIPA {
            ipadImport = try importAsphalt6IPadAssets(ipadIPA, into: output.appendingPathComponent("Resources"))
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
        info["CFBundleIdentifier"] = Asphalt6.bundleID + ".compat"
        info["CFBundleDisplayName"] = "\(Asphalt6.title) (Compatibility)"
        info["LSMinimumSystemVersion"] = "11.0"
        info["NSHighResolutionCapable"] = false
        info["LP32GeneratedGame"] = "asphalt6"
        info["LP32GLMetal"] = metal != nil
        info["LP32ContinueWhenInactive"] = false
        if let ipadImport {
            info["LP32IPadTextures"] = ipadImport
        }
        var environment = info["LSEnvironment"] as? [String: String] ?? [:]
        environment["LP32_GL_BACKEND"] = metal != nil ? "metal" : "apple"
        environment["LP32_ASPHALT6_SCENE_SCALE"] = "3"
        environment["LP32_ASPHALT6_MSAA"] = "8"
        environment.removeValue(forKey: "LP32_GLMETAL_PATH")
        info["LSEnvironment"] = environment
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            .write(to: output.appendingPathComponent("Info.plist"))
        progress("Finishing your app…", "Preparing \(Asphalt6.title) to open from Finder.")
        try run("/usr/bin/xattr", ["-cr", app.path])
        try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        try run("/usr/bin/codesign", ["--verify", "--strict", app.path])
        try layout.validate()
        try cancellation.check()
        return try publish(app, in: destination, name: stem + "-Compat")
    }

    /// Import into the unpublished copy so cancellation or an invalid IPA never
    /// alters the original Mac game or publishes an incomplete converted app.
    func importAsphalt6IPadAssets(_ ipa: URL, into gameResources: URL) throws -> [String: Any] {
        let helper = resources.appendingPathComponent("asphalt6-ipad-assets")
        guard FileManager.default.isExecutableFile(atPath: helper.path) else {
            throw ConversionError.message("The converter is missing its iPad texture importer. Use a complete copy of Asphalt6-Converter.app.")
        }
        let pack = gameResources.appendingPathComponent("data/file000017.dat")
        let imported = pack.deletingLastPathComponent().appendingPathComponent(".ipad-textures-\(UUID().uuidString).dat")
        let report = gameResources.appendingPathComponent("Asphalt6-iPad-textures.json")
        defer { try? FileManager.default.removeItem(at: imported) }
        progress("Importing iPad textures…", "Decoding larger track textures and comparing their SHA-256 hashes with the Mac game.")
        try run(helper.path, ["--import", ipa.path, pack.path, imported.path, report.path])
        try cancellation.check()
        guard let json = try JSONSerialization.jsonObject(with: Data(contentsOf: report)) as? [String: Any],
              let version = json["ipadVersion"] as? String,
              let count = json["upgradedTextures"] as? Int, count > 0 else {
            throw ConversionError.message("The iPad texture importer returned an incomplete report.")
        }
        _ = try FileManager.default.replaceItemAt(pack, withItemAt: imported)
        progress("Imported \(count) iPad textures.", "A texture hash comparison is saved inside the converted app.")
        return ["version": version, "upgradedTextures": count, "report": report.lastPathComponent]
    }
}
