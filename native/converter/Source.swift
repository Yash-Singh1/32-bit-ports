import Foundation

enum ConverterError: LocalizedError {
    case message(String)
    case cancelled
    var errorDescription: String? {
        switch self {
        case .message(let text): return text
        case .cancelled: return "Conversion cancelled. Your original game is unchanged."
        }
    }
}

enum LEGOGame: String, CaseIterable, Identifiable {
    case clonewars, pirates, sagaSteam = "saga-steam", sagaRetail = "saga-retail"
    case marvel, batman3, movie, hobbit
    var id: String { rawValue }
    var title: String {
        switch self {
        case .clonewars: return "Star Wars III: The Clone Wars"
        case .pirates: return "Pirates of the Caribbean"
        case .sagaSteam: return "Star Wars: The Complete Saga"
        case .sagaRetail: return "Star Wars: The Complete Saga"
        case .marvel: return "Marvel Super Heroes"
        case .batman3: return "Batman 3: Beyond Gotham"
        case .movie: return "The LEGO Movie Videogame"
        case .hobbit: return "The Hobbit"
        }
    }
    var edition: String {
        switch self {
        case .sagaSteam: return "Steam edition"
        case .sagaRetail: return "Retail edition"
        default: return "Mac edition"
        }
    }
    var menuTitle: String {
        title + (self == .sagaSteam || self == .sagaRetail ? " · " + edition : "")
    }
    var experimental: Bool { [.batman3, .movie, .hobbit].contains(self) }
    var stem: String {
        switch self {
        case .clonewars: return "LEGOCloneWars"
        case .pirates: return "LEGOPirates"
        case .sagaSteam, .sagaRetail: return "LEGOCompleteSaga"
        case .marvel: return "LEGOMarvel"
        case .batman3: return "LEGOBatman3"
        case .movie: return "LEGOMovie"
        case .hobbit: return "LEGOHobbit"
        }
    }
    var outputStem: String { stem + (self == .sagaSteam ? "-Steam" : "") + "-Compat" }
    var template: String {
        switch self {
        case .pirates: return "Info.plist"
        case .clonewars: return "Info-CloneWars.plist"
        case .sagaSteam, .sagaRetail: return "Info-Saga.plist"
        case .marvel: return "Info-Marvel.plist"
        case .batman3: return "Info-Batman3.plist"
        case .movie: return "Info-Movie.plist"
        case .hobbit: return "Info-Hobbit.plist"
        }
    }
    var executableNames: [String] {
        switch self {
        case .pirates: return ["LEGOPirates.macbin"]
        case .clonewars: return ["LEGOCloneWars.macbin"]
        case .marvel: return ["LEGOMarvel.macbin", "LEGO Marvel Super Heroes"]
        case .batman3: return ["LEGO Batman 3"]
        case .movie: return ["The LEGO Movie"]
        case .hobbit: return ["LEGO The Hobbit"]
        case .sagaSteam, .sagaRetail: return ["LEGO Star Wars Saga"]
        }
    }
    var fingerprints: [(UInt32, UInt32)] {
        switch self {
        case .clonewars: return [(0x2550c, 0xfe0000)]
        case .pirates: return [(0x363b0, 0xe56000)]
        case .marvel: return [(0x2720, 0x1753000)]
        case .batman3: return [(0x2cd0, 0x1b0e71c)]
        case .movie: return [(0x2930, 0x1896000)]
        case .hobbit: return [(0x2720, 0x19c8f54)]
        case .sagaSteam: return [(0x16ee80, 0x2ed4640)]
        case .sagaRetail: return [(0x2460, 0x264d000), (0x355b4, 0x276b000)]
        }
    }
    func matches(_ image: MachOImage) -> Bool {
        fingerprints.contains { $0.0 == image.entry && $0.1 == image.end }
    }
    static func from(identifier: String) -> LEGOGame? {
        let value = identifier.lowercased()
        if value.hasSuffix("legostarwarsiii") { return .clonewars }
        if value.hasSuffix("legopirates") || value == "com.t-tales.www" { return .pirates }
        if value.hasSuffix("legomarvelsuperheroes") { return .marvel }
        if value.hasSuffix("legobatman3") { return .batman3 }
        if value.hasSuffix("legomovie") { return .movie }
        if value.hasSuffix("legohobbit") { return .hobbit }
        if value.hasSuffix("saga") { return .sagaRetail }
        return nil
    }
}

struct MachOImage {
    let data: Data
    let entry: UInt32
    let end: UInt32
    var packedPirates: Bool { entry == 0x12ee474 }

    static func word(_ data: Data, _ offset: Int, little: Bool = true) throws -> UInt32 {
        guard offset >= 0, offset <= data.count - 4 else {
            throw ConverterError.message("The executable has a truncated Mach-O header.")
        }
        let a = (0..<4).map { UInt32(data[offset + $0]) }
        return little ? a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24 :
            a[3] | a[2] << 8 | a[1] << 16 | a[0] << 24
    }

    static func slice(_ input: Data, cpu: UInt32) throws -> Data {
        guard input.count >= 8 else { throw ConverterError.message("The executable is empty or incomplete.") }
        let magic = try word(input, 0)
        if magic == 0xfeedface || magic == 0xfeedfacf {
            guard try word(input, 4) == cpu else {
                throw ConverterError.message(cpu == 7 ? "This executable has no 32-bit Intel Mac version." :
                    "The Steam or graphics library is missing its 64-bit Intel version.")
            }
            return input
        }
        let big = try word(input, 0, little: false)
        guard big == 0xcafebabe || big == 0xcafebabf else {
            throw ConverterError.message("This file is not a supported Mac executable.")
        }
        let count = Int(try word(input, 4, little: false))
        let stride = big == 0xcafebabf ? 32 : 20
        guard count > 0, count <= (input.count - 8) / stride else {
            throw ConverterError.message("The executable's architecture table is incomplete.")
        }
        for i in 0..<count {
            let base = 8 + i * stride
            guard try word(input, base, little: false) == cpu else { continue }
            let offset: UInt64, size: UInt64
            if stride == 32 {
                offset = UInt64(try word(input, base + 8, little: false)) << 32 |
                    UInt64(try word(input, base + 12, little: false))
                size = UInt64(try word(input, base + 16, little: false)) << 32 |
                    UInt64(try word(input, base + 20, little: false))
            } else {
                offset = UInt64(try word(input, base + 8, little: false))
                size = UInt64(try word(input, base + 12, little: false))
            }
            guard offset >= UInt64(8 + count * stride), size >= 28,
                  offset <= input.count, size <= UInt64(input.count) - offset else {
                throw ConverterError.message("The executable contains an invalid architecture slice.")
            }
            let result = input.subdata(in: Int(offset)..<Int(offset + size))
            guard try word(result, 0) == (cpu == 7 ? 0xfeedface : 0xfeedfacf),
                  try word(result, 4) == cpu else {
                throw ConverterError.message("The executable contains a damaged Intel slice.")
            }
            return result
        }
        throw ConverterError.message("The required Intel architecture is missing from this file.")
    }

    init(data original: Data) throws {
        data = try Self.slice(original, cpu: 7)
        guard data.count >= 28, try Self.word(data, 12) == 2 else {
            throw ConverterError.message("Choose the original game's 32-bit Mac executable.")
        }
        let count = Int(try Self.word(data, 16)), commandSize = Int(try Self.word(data, 20))
        guard commandSize <= data.count - 28, count <= commandSize / 8 else {
            throw ConverterError.message("The game's Mach-O load commands are incomplete.")
        }
        var cursor = 28, highest: UInt32 = 0, start: UInt32 = 0
        var mainOffset: UInt64?, textAddress: UInt32 = 0, textOffset: UInt32 = 0, textSize: UInt32 = 0
        for _ in 0..<count {
            let command = try Self.word(data, cursor), size = Int(try Self.word(data, cursor + 4))
            guard size >= 8, cursor <= 28 + commandSize - size else {
                throw ConverterError.message("The game has an invalid Mach-O load command.")
            }
            if command == 1 {
                guard size >= 56 else { throw ConverterError.message("A game segment is truncated.") }
                let nameData = data.subdata(in: cursor + 8..<cursor + 24)
                let name = String(decoding: nameData.prefix { $0 != 0 }, as: UTF8.self)
                let address = try Self.word(data, cursor + 24), length = try Self.word(data, cursor + 28)
                guard UInt64(address) + UInt64(length) <= UInt32.max else {
                    throw ConverterError.message("A game segment exceeds the 32-bit address range.")
                }
                if name != "__PAGEZERO" { highest = max(highest, address + length) }
                if name == "__TEXT" {
                    textAddress = address
                    textOffset = try Self.word(data, cursor + 32)
                    textSize = try Self.word(data, cursor + 36)
                }
            } else if command == 5, size >= 80, try Self.word(data, cursor + 8) == 1,
                      try Self.word(data, cursor + 12) >= 16 {
                start = try Self.word(data, cursor + 56)
            } else if command == 0x80000028, size >= 24 {
                mainOffset = UInt64(try Self.word(data, cursor + 8)) |
                    UInt64(try Self.word(data, cursor + 12)) << 32
            }
            cursor += size
        }
        if let offset = mainOffset {
            guard offset >= textOffset, offset - UInt64(textOffset) < textSize else {
                throw ConverterError.message("The game's executable entry point is invalid.")
            }
            start = textAddress + UInt32(offset - UInt64(textOffset))
        }
        guard start != 0, highest != 0 else { throw ConverterError.message("The game has no executable entry point.") }
        entry = start; end = highest
    }
}

struct SourceInspection {
    let app: URL
    let info: [String: Any]
    let candidates: [(URL, MachOImage)]
    let detected: LEGOGame?
    let detectionReason: String

    static func read(_ url: URL) throws -> SourceInspection {
        let app = url.resolvingSymlinksInPath().standardizedFileURL
        guard app.pathExtension.lowercased() == "app" else {
            return try readFolder(app)
        }
        let contents = app.appendingPathComponent("Contents")
        guard let info = try PropertyListSerialization.propertyList(from:
            Data(contentsOf: contents.appendingPathComponent("Info.plist")), format: nil) as? [String: Any] else {
            throw ConverterError.message("This app's Info.plist is missing or unreadable.")
        }
        let identifier = (info["CFBundleIdentifier"] as? String ?? "").lowercased()
        guard !identifier.hasSuffix(".compat"), info["LEGOConverterConversion"] == nil else {
            throw ConverterError.message("This is already a compatibility app. Choose the original Mac game instead.")
        }
        let macos = contents.appendingPathComponent("MacOS")
        let entries = try FileManager.default.contentsOfDirectory(at: macos,
            includingPropertiesForKeys: [.isRegularFileKey])
        var candidates: [(URL, MachOImage)] = []
        for path in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            guard (try? path.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true,
                  let data = try? Data(contentsOf: path, options: .mappedIfSafe),
                  let image = try? MachOImage(data: data) else { continue }
            candidates.append((path, image))
        }
        let recognized = Set(candidates.flatMap { item in
            LEGOGame.allCases.filter { $0.matches(item.1) || ($0 == .pirates && item.1.packedPirates) }
        })
        guard recognized.count <= 1 else {
            throw ConverterError.message("This bundle contains executables for several games. Choose a single original game app.")
        }
        let byExecutable = recognized.first
        let byBundle = LEGOGame.from(identifier: identifier)
        return SourceInspection(app: app, info: info, candidates: candidates,
            detected: byExecutable ?? byBundle,
            detectionReason: byExecutable != nil ? "Matched the original game executable" :
                byBundle != nil ? "Matched the app's bundle identifier" : "Choose a supported game manually")
    }

    private static func readFolder(_ folder: URL) throws -> SourceInspection {
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .isSymbolicLinkKey]
        guard (try? folder.resourceValues(forKeys: keys).isDirectory) == true else {
            throw ConverterError.message("Choose an original Mac .app or its Steam game folder.")
        }
        var found: [SourceInspection] = []
        var visited = 0
        func scan(_ directory: URL, depth: Int) throws {
            let children = try FileManager.default.contentsOfDirectory(at: directory,
                includingPropertiesForKeys: Array(keys), options: [.skipsHiddenFiles])
            var directories: [URL] = []
            let before = found.count
            for child in children {
                visited += 1
                guard visited <= 2000 else {
                    throw ConverterError.message("Choose the specific game's folder inside steamapps/common, or its .app.")
                }
                guard let values = try? child.resourceValues(forKeys: keys),
                      values.isSymbolicLink != true, values.isDirectory == true else { continue }
                if child.pathExtension.lowercased() == "app" {
                    if let inspected = try? read(child) { found.append(inspected) }
                } else { directories.append(child) }
            }
            if found.count == before, depth < 2 {
                for child in directories { try scan(child, depth: depth + 1) }
            }
        }
        try scan(folder, depth: 0)
        let recognized = found.filter { $0.detected != nil }
        let candidates = recognized.isEmpty ? found : recognized
        guard candidates.count == 1, let result = candidates.first else {
            throw ConverterError.message(candidates.isEmpty ?
                "No original Mac game app found. Choose its folder inside steamapps/common, or select the .app directly." :
                "This folder contains several games. Choose the specific game folder or app.")
        }
        return result
    }

    func layout(for game: LEGOGame, dataOverride: URL? = nil) throws -> GameLayout {
        guard let executable = candidates.first(where: { game.matches($0.1) ||
            (game == .pirates && $0.1.packedPirates) }) else {
            if let actual = candidates.flatMap({ item in LEGOGame.allCases.filter { $0.matches(item.1) } }).first {
                throw ConverterError.message("This executable belongs to \(actual.menuTitle). Select that game to continue.")
            }
            throw ConverterError.message("This version of \(game.title) is not supported yet. Use a supported 32-bit Mac release, not a Windows or 64-bit Mac app.")
        }
        let fm = FileManager.default, contents = app.appendingPathComponent("Contents")
        let resources = contents.appendingPathComponent("Resources")
        guard fm.fileExists(atPath: resources.path) else {
            throw ConverterError.message("This installation is missing its Resources folder. Reinstall the original game first.")
        }
        let cg = ["Library/Frameworks/Cg.framework", "Frameworks/Cg.framework"].map {
            contents.appendingPathComponent($0)
        }.first { fm.fileExists(atPath: $0.appendingPathComponent("Cg").path) ||
            fm.fileExists(atPath: $0.appendingPathComponent("Versions/A/Cg").path) }
        if game != .sagaSteam && game != .sagaRetail && cg == nil {
            throw ConverterError.message("The original Cg graphics framework is missing. Choose a complete installation of this game.")
        }
        let steam = ["Resources/libsteam_api.dylib", "Frameworks/libsteam_api.dylib"].map {
            contents.appendingPathComponent($0)
        }.first { fm.fileExists(atPath: $0.path) }
        if game == .sagaSteam && steam == nil {
            throw ConverterError.message("The Steam edition's libsteam_api.dylib is missing. Verify the game files in Steam.")
        }
        if let steam = steam {
            _ = try MachOImage.slice(Data(contentsOf: steam, options: .mappedIfSafe), cpu: 0x01000007)
        }
        var data: URL?
        if game == .sagaSteam || game == .sagaRetail {
            let folder = game == .sagaSteam ? "LEGOStarWarsSagaData" : "Data"
            data = dataOverride ?? app.deletingLastPathComponent().appendingPathComponent(folder)
            let files = game == .sagaSteam ?
                ["game.dat", "savegame.zip", "episode_i.dat", "episode_ii.dat", "episode_iii.dat", "episode_iv.dat", "episode_v.dat", "episode_vi.dat", "audio", "movies", "feralshaders", "stuff"] :
                ["GAME.DAT", "EPISODE_I.DAT", "EPISODE_II.DAT", "EPISODE_III.DAT", "EPISODE_IV.DAT", "EPISODE_V.DAT", "EPISODE_VI.DAT", "Audio", "Movies"]
            let missing = files.filter { !fm.fileExists(atPath: data!.appendingPathComponent($0).path) }
            guard missing.isEmpty else {
                throw ConverterError.message("Complete Saga needs its \(folder) folder beside the original app. Missing: \(missing.prefix(4).joined(separator: ", ")). Use Choose data folder if it is stored elsewhere.")
            }
        }
        if game == .pirates {
            for name in ["GAME.DAT", "GAME0.DAT", "GAME1.DAT", "GAME2.DAT", "GAME3.DAT", "GAME4.DAT"] {
                guard fm.fileExists(atPath: contents.appendingPathComponent("MacOS/" + name).path) else {
                    throw ConverterError.message("The original installation is missing \(name). Choose the complete game app.")
                }
            }
        }
        return GameLayout(inspection: self, game: game, image: executable.0,
            imageData: executable.1, resources: resources, cg: cg, steam: steam, data: data)
    }
}

struct GameLayout {
    let inspection: SourceInspection
    let game: LEGOGame
    let image: URL
    let imageData: MachOImage
    let resources: URL
    let cg: URL?
    let steam: URL?
    let data: URL?
}

func pathIsInside(_ child: URL, _ parent: URL) -> Bool {
    child.resolvingSymlinksInPath().standardizedFileURL.pathComponents.starts(with:
        parent.resolvingSymlinksInPath().standardizedFileURL.pathComponents)
}
