import Foundation

enum GameTitle: String, CaseIterable {
    case cod2, cod4, mw2, mw3
    static var current: GameTitle {
        GameTitle(rawValue: Bundle.main.object(forInfoDictionaryKey: "LP32ConverterGame") as? String ?? "cod4") ?? .cod4
    }
    var short: String { rawValue.uppercased() }
    var title: String {
        switch self {
        case .cod2: return "Call of Duty 2"
        case .cod4: return "Call of Duty 4"
        case .mw2: return "Modern Warfare 2"
        case .mw3: return "Modern Warfare 3"
        }
    }
    var isCarbon: Bool { self == .cod2 || self == .cod4 }
    var usesGLMetal: Bool { self != .cod2 }
    func stem(_ mode: GameMode) -> String { short + (mode == .mp ? "MP" : "") }
    func appID(_ mode: GameMode) -> Int {
        switch self {
        case .cod2: return 2630
        case .cod4: return 7940
        case .mw2: return mode == .mp ? 10190 : 10180
        case .mw3: return mode == .mp ? 42690 : 42680
        }
    }
    func discover(_ selected: URL, mode: GameMode) throws -> any GameSource {
        if isCarbon { return try COD4Source.discover(selected, mode: mode, game: self) }
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        let app = selected.pathExtension == "app" ? selected : selected.appendingPathComponent("COD_\(short)_\(mode.rawValue.uppercased()).app")
        let source = ModernWarfareSource(app: app, mode: mode, game: self)
        _ = try source.validateLayout()
        return source
    }
    func steamInstallation(mode: GameMode, home: URL = FileManager.default.homeDirectoryForCurrentUser) -> URL? {
        if isCarbon { return COD4Source.steamInstallation(home: home, game: self) }
        let steam = home.appendingPathComponent("Library/Application Support/Steam")
        var libraries = [steam]
        if let text = try? String(contentsOf: steam.appendingPathComponent("steamapps/libraryfolders.vdf"), encoding: .utf8) {
            libraries += COD4Source.vdfValues("path", in: text).map { URL(fileURLWithPath: $0) }
        }
        for library in libraries {
            guard let text = try? String(contentsOf: library.appendingPathComponent("steamapps/appmanifest_\(appID(mode)).acf"), encoding: .utf8),
                  COD4Source.vdfValues("StateFlags", in: text).first == "4",
                  let folder = COD4Source.vdfValues("installdir", in: text).first,
                  !folder.isEmpty, folder != ".", folder != "..", !folder.contains("/"), !folder.contains("\\") else { continue }
            if let source = try? discover(library.appendingPathComponent("steamapps/common/" + folder), mode: mode) { return source.app }
        }
        return nil
    }
}

protocol GameSource {
    var app: URL { get }
    var contents: URL { get }
    var image: URL { get }
    var resources: URL { get }
    var gameData: URL { get }
    var steamLibrary: URL? { get }
    var guestLibraries: [URL] { get }
    func validateLayout() throws -> [String: Any]
    func checkDestination(_ destination: URL) throws
}

extension COD4Source: GameSource {
    var guestLibraries: [URL] { binkLibrary.map { [$0] } ?? [] }
}

struct ModernWarfareSource: GameSource {
    let app: URL
    let mode: GameMode
    let game: GameTitle
    var originalExecutable: String { "COD_\(game.short)_\(mode.rawValue.uppercased())" }
    var contents: URL { app.appendingPathComponent("Contents") }
    var image: URL { contents.appendingPathComponent("MacOS/" + originalExecutable + "sub") }
    var resources: URL { contents.appendingPathComponent("Resources") }
    var gameData: URL { app.deletingLastPathComponent().appendingPathComponent("GameData") }
    var steamLibrary: URL? { contents.appendingPathComponent("MacOS/libsteam_api.dylib") }
    var guestLibraries: [URL] {
        [game == .mw3 && mode == .mp ? "libBink2Macx86.dylib" : "libBinkMacx86.dylib", "libMilesX86.dylib"]
            .map { contents.appendingPathComponent("MacOS/" + $0) }
    }
    func validateLayout() throws -> [String: Any] {
        if app.pathComponents.contains("steamapps") && app.pathComponents.contains("downloading") {
            throw ConversionError.message("Let Steam finish downloading before converting.")
        }
        let common = app.deletingLastPathComponent().deletingLastPathComponent()
        if common.lastPathComponent == "common" && common.deletingLastPathComponent().lastPathComponent == "steamapps" {
            let manifest = common.deletingLastPathComponent().appendingPathComponent("appmanifest_\(game.appID(mode)).acf")
            if FileManager.default.fileExists(atPath: manifest.path) {
                let text = try String(contentsOf: manifest, encoding: .utf8)
                guard COD4Source.vdfValues("StateFlags", in: text).first == "4" else {
                    throw ConversionError.message("Let Steam finish installing or updating \(game.title) before converting.")
                }
            }
        }
        let data = try Data(contentsOf: contents.appendingPathComponent("Info.plist"))
        guard let info = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              info["LP32GeneratedGame"] == nil, info["CFBundleExecutable"] as? String == originalExecutable else {
            throw ConversionError.message("Choose the original \(game.title) \(mode.title.lowercased()) Mac app.")
        }
        for file in [image, gameData.appendingPathComponent("main/iw_00.iwd"), steamLibrary!] + guestLibraries {
            let attributes = try FileManager.default.attributesOfItem(atPath: file.path)
            guard attributes[.type] as? FileAttributeType == .typeRegular,
                  (attributes[.size] as? NSNumber)?.intValue ?? 0 > 0 else {
                throw ConversionError.message("The installation is incomplete or contains a link: \(file.path)")
            }
        }
        for file in [image] + guestLibraries { _ = try machOSlice(Data(contentsOf: file), cpu: 7) }
        _ = try machOSlice(Data(contentsOf: steamLibrary!), cpu: 0x01000007)
        return info
    }
    func checkDestination(_ destination: URL) throws {
        guard !isInside(destination, app), !isInside(destination, gameData) else {
            throw ConversionError.message("Choose an output folder outside the original game and GameData folders.")
        }
    }
}
