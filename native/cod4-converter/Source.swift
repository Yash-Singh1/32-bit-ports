import Foundation

enum GameMode: String, CaseIterable {
    case sp, mp
    var title: String { self == .sp ? "Campaign" : "Multiplayer" }
    var stem: String { self == .sp ? "COD4-Compat" : "COD4MP-Compat" }
    var executable: String { self == .sp ? "COD4Compat" : "COD4MPCompat" }
    var imageName: String { self == .sp ? "COD4.image" : "COD4MP.image" }
    var originalExecutable: String { self == .sp ? "Call of Duty 4" : "Call of Duty 4 Multiplayer" }
    var icon: String { self == .sp ? "Game.icns" : "Game_mp.icns" }
}

struct COD4Source {
    let app: URL
    let mode: GameMode
    var directMultiplayer = false
    var game: GameTitle = .cod4
    var contents: URL {
        app.appendingPathComponent(mode == .sp || directMultiplayer ? "Contents" : "Contents/\(game.title) Multiplayer.app/Contents")
    }
    var image: URL {
        let info = (try? Data(contentsOf: contents.appendingPathComponent("Info.plist")))
            .flatMap { try? PropertyListSerialization.propertyList(from: $0, format: nil) as? [String: Any] }
        let name = info?["CFBundleExecutable"] as? String ?? (game.title + (mode == .mp ? " Multiplayer" : ""))
        return contents.appendingPathComponent("MacOS/" + name)
    }
    var resources: URL { contents.appendingPathComponent("Resources") }
    var gameData: URL {
        let candidates = [app.appendingPathComponent("Contents/\(game.title) Data"),
                          app.appendingPathComponent("Contents/Resources/\(game.title) Data"),
                          app.deletingLastPathComponent().appendingPathComponent("\(game.title) Data")]
        return candidates.first { FileManager.default.fileExists(atPath: $0.appendingPathComponent("main/iw_00.iwd").path) } ?? candidates[0]
    }
    private func library(_ name: String) -> URL? {
        for directory in ["MacOS", "Frameworks", "Resources"] {
            let url = contents.appendingPathComponent(directory + "/" + name)
            if FileManager.default.fileExists(atPath: url.path) { return url }
        }
        return nil
    }
    var steamLibrary: URL? { library("libsteam_api.dylib") }
    var binkLibrary: URL? { library("libBinkMachOx86.dylib") }

    static func discover(_ selected: URL, mode: GameMode = .sp, game: GameTitle = .cod4) throws -> COD4Source {
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        var candidates = [selected]
        // Finder can select the nested multiplayer app as well as the outer app.
        if selected.lastPathComponent == "\(game.title) Multiplayer.app" {
            candidates.insert(selected.deletingLastPathComponent().deletingLastPathComponent(), at: 0)
        }
        for prefix in ["", "\(game.title)/", "common/\(game.title)/", "steamapps/common/\(game.title)/"] {
            candidates.append(selected.appendingPathComponent(prefix + game.title + ".app"))
        }
        var firstError: Error?
        for app in candidates {
            guard FileManager.default.fileExists(atPath: app.appendingPathComponent("Contents/Info.plist").path) else { continue }
            let source = COD4Source(app: app, mode: mode,
                                    directMultiplayer: mode == .mp && app == selected &&
                                        app.lastPathComponent == "\(game.title) Multiplayer.app", game: game)
            do {
                _ = try source.validateLayout()
                return source
            } catch {
                if firstError == nil { firstError = error }
            }
        }
        if let firstError { throw firstError }
        throw ConversionError.message("Choose the original Mac \(game.title) app or its game folder. The executable must contain 32-bit Intel Mac code.")
    }

    func validateLayout() throws -> [String: Any] {
        let fm = FileManager.default
        let infoURL = contents.appendingPathComponent("Info.plist")
        let data = try Data(contentsOf: infoURL)
        guard let info = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let executable = info["CFBundleExecutable"] as? String,
              !executable.isEmpty, executable != ".", executable != "..",
              !executable.contains("/"), !executable.contains("\\") else {
            throw ConversionError.message("The Mac game app has an invalid executable entry in Info.plist.")
        }
        for file in [image, infoURL, gameData.appendingPathComponent("main/iw_00.iwd")] {
            var directory: ObjCBool = false
            guard fm.fileExists(atPath: file.path, isDirectory: &directory), !directory.boolValue else {
                throw ConversionError.message("The Mac game installation is incomplete: missing \(file.lastPathComponent).")
            }
        }
        if info["LP32GeneratedGame"] != nil {
            throw ConversionError.message("Choose the original game app, rather than an already converted copy.")
        }
        return info
    }

    static func steamInstallation(home: URL = FileManager.default.homeDirectoryForCurrentUser, game: GameTitle = .cod4) -> URL? {
        let steam = home.appendingPathComponent("Library/Application Support/Steam")
        var libraries = [steam]
        if let text = try? String(contentsOf: steam.appendingPathComponent("steamapps/libraryfolders.vdf"), encoding: .utf8) {
            for path in vdfValues("path", in: text) { libraries.append(URL(fileURLWithPath: path)) }
        }
        for library in libraries {
            guard let manifest = try? String(contentsOf: library.appendingPathComponent("steamapps/appmanifest_\(game.appID(.sp)).acf"), encoding: .utf8),
                  let folder = vdfValues("installdir", in: manifest).first,
                  !folder.isEmpty, folder != ".", folder != "..", !folder.contains("/"), !folder.contains("\\") else { continue }
            let app = library.appendingPathComponent("steamapps/common/" + folder + "/" + game.title + ".app")
            if (try? discover(app, game: game)) != nil { return app }
        }
        return nil
    }

    static func vdfValues(_ key: String, in text: String) -> [String] {
        let pattern = "\"" + NSRegularExpression.escapedPattern(for: key) + "\"\\s+\"((?:\\\\.|[^\"\\\\])*)\""
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return [] }
        return regex.matches(in: text, range: NSRange(text.startIndex..., in: text)).compactMap {
            guard let range = Range($0.range(at: 1), in: text) else { return nil }
            return String(text[range]).replacingOccurrences(of: "\\\\", with: "\\").replacingOccurrences(of: "\\\"", with: "\"")
        }
    }

    func checkDestination(_ destination: URL) throws {
        guard !isInside(destination, app), !isInside(destination, gameData) else {
            throw ConversionError.message("Choose an output folder outside the original game files.")
        }
    }
}

// Materialize an independent copy. Reject links before any signing or metadata
// changes could follow them back into the source installation.
func copyGameTree(_ source: URL, to target: URL, excluding excluded: URL? = nil,
                  check: () throws -> Void = {}) throws {
    if let excluded, source.standardizedFileURL == excluded.standardizedFileURL { return }
    let fm = FileManager.default
    try check()
    let attributes = try fm.attributesOfItem(atPath: source.path)
    switch attributes[.type] as? FileAttributeType {
    case .typeDirectory:
        try fm.createDirectory(at: target, withIntermediateDirectories: true)
        for item in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
            try copyGameTree(item, to: target.appendingPathComponent(item.lastPathComponent),
                             excluding: excluded, check: check)
        }
    case .typeRegular:
        try fm.copyItem(at: source, to: target)
    default:
        throw ConversionError.message("The installation contains a link or unsupported file at \(source.path). Choose a complete original Mac installation.")
    }
}
