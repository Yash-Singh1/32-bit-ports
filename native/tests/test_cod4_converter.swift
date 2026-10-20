import Foundation
import Darwin

private func expect(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
    if try !value() { throw ConversionError.message("FAIL: " + message) }
}

private func rejects(_ description: String, _ body: () throws -> Void) throws {
    do { try body() } catch { return }
    throw ConversionError.message("FAIL: accepted " + description)
}

@main
struct ConverterTests {
    static func main() throws {
        let fm = FileManager.default
        let temp = fm.temporaryDirectory.appendingPathComponent("cod4-tests-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let resources = temp.appendingPathComponent("Converter.app/Contents/Resources")
        try fm.createDirectory(at: resources, withIntermediateDirectories: true)
        let converter = Converter(resources: resources, cache: temp.appendingPathComponent("cache")) { _, _ in }

        let thin = Data([0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0] + [UInt8](repeating: 0, count: 24))
        func be(_ word: UInt32) -> [UInt8] {
            [UInt8(truncatingIfNeeded: word >> 24), UInt8(truncatingIfNeeded: word >> 16),
             UInt8(truncatingIfNeeded: word >> 8), UInt8(truncatingIfNeeded: word)]
        }
        var fat = Data([0xca, 0xfe, 0xba, 0xbe] + be(2))
        fat.append(contentsOf: be(18) + be(0) + be(48) + be(4) + be(0))
        fat.append(contentsOf: be(7) + be(3) + be(52) + be(UInt32(thin.count)) + be(0))
        fat.append(Data([0, 0, 0, 0]))
        fat.append(thin)
        try expect(try machOSlice(fat, cpu: 7) == thin, "extract i386 from a multi-architecture file")
        try expect(try machOSlice(thin, cpu: 7) == thin, "accept a thin i386 library")
        for size in 0..<52 {
            try rejects("truncated Mach-O at \(size)") { _ = try machOSlice(Data(fat.prefix(size)), cpu: 7) }
        }
        var invalid = fat
        invalid.replaceSubrange(36..<40, with: be(UInt32.max))
        try rejects("out-of-bounds slice") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid.replaceSubrange(36..<40, with: be(0))
        try rejects("overlapping header") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid.replaceSubrange(4..<8, with: be(UInt32.max))
        try rejects("invalid architecture count") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid.replaceSubrange(28..<32, with: be(18))
        try rejects("missing i386") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid[56] = 18
        try rejects("incorrect slice CPU type") { _ = try machOSlice(invalid, cpu: 7) }
        print("PASS: Mach-O extraction and malformed input bounds")

        let digestFile = temp.appendingPathComponent("digest")
        try Data("abc".utf8).write(to: digestFile)
        try expect(try converter.sha256(digestFile) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256")

        let thin64 = Data([0xcf, 0xfa, 0xed, 0xfe, 7, 0, 0, 1] + [UInt8](repeating: 0, count: 24))
        var fat64 = fat
        fat64.replaceSubrange(28..<32, with: be(0x01000007))
        fat64.replaceSubrange(52..<fat64.count, with: thin64)
        try expect(try machOSlice(fat64, cpu: 0x01000007) == thin64, "extract x86_64 Steam SDK without lipo")
        try expect(try machOSlice(thin64, cpu: 0x01000007) == thin64, "accept thin x86_64")
        try rejects("wrong Steam SDK architecture") { _ = try machOSlice(thin, cpu: 0x01000007) }

        let steamRoot = temp.appendingPathComponent("Steam Library ' $ nested")
        let source = steamRoot.appendingPathComponent("steamapps/common/Call of Duty 4/Call of Duty 4.app")
        let destination = temp.appendingPathComponent("outputs")
        try fm.createDirectory(at: destination, withIntermediateDirectories: false)
        for mode in GameMode.allCases {
            let layout = COD4Source(app: source, mode: mode)
            for directory in [layout.contents.appendingPathComponent("MacOS"), layout.resources, layout.gameData.appendingPathComponent("main")] {
                try fm.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            for (url, bytes) in [(layout.image, thin), (layout.contents.appendingPathComponent("MacOS/libBinkMachOx86.dylib"), thin),
                                 (layout.contents.appendingPathComponent("MacOS/libsteam_api.dylib"), fat64),
                                 (layout.resources.appendingPathComponent(mode.icon), Data("icon".utf8)),
                                 (layout.gameData.appendingPathComponent("main/iw_00.iwd"), Data("game".utf8))] {
                try bytes.write(to: url)
            }
            let info = ["CFBundleIdentifier": "com.aspyr.callofduty4.\(mode.rawValue).steam",
                        "CFBundleShortVersionString": "1.7.2", "CFBundleExecutable": mode.originalExecutable]
            try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0).write(to: layout.contents.appendingPathComponent("Info.plist"))
            for selection in [source, source.deletingLastPathComponent(), steamRoot,
                              steamRoot.appendingPathComponent("steamapps"), steamRoot.appendingPathComponent("steamapps/common")] {
                try expect(try COD4Source.discover(selection, mode: mode).image == layout.image, "Steam folder and mode discovery")
            }
            try expect(try machOSlice(Data(contentsOf: layout.image), cpu: 7) == thin, "other i386 builds are accepted")
        }
        let nested = source.appendingPathComponent("Contents/Call of Duty 4 Multiplayer.app")
        try expect(try COD4Source.discover(nested, mode: .mp).app.path == source.path, "nested multiplayer selection")
        let home = temp.appendingPathComponent("home")
        let steam = home.appendingPathComponent("Library/Application Support/Steam/steamapps")
        try fm.createDirectory(at: steam, withIntermediateDirectories: true)
        try "\"libraryfolders\" { \"1\" { \"path\" \"\(steamRoot.path)\" } }".write(to: steam.appendingPathComponent("libraryfolders.vdf"), atomically: true, encoding: .utf8)
        let manifest = steamRoot.appendingPathComponent("steamapps/appmanifest_7940.acf")
        try "\"installdir\" \"Call of Duty 4\"".write(to: manifest, atomically: true, encoding: .utf8)
        try expect(COD4Source.steamInstallation(home: home)?.path == source.path, "external Steam library detected")
        try "\"installdir\" \"../Call of Duty 4\"".write(to: manifest, atomically: true, encoding: .utf8)
        try expect(COD4Source.steamInstallation(home: home) == nil, "manifest traversal rejected")
        let link = temp.appendingPathComponent("source-link")
        try fm.createSymbolicLink(at: link, withDestinationURL: source)
        try expect(isInside(link.appendingPathComponent("Contents"), source), "resolve symlink containment")
        try expect(!isInside(temp.appendingPathComponent("Call of Duty 4.app backup"), source), "containment uses path components")
        try rejects("output inside source") { _ = try converter.convert(source: source, destination: link) }
        try rejects("output inside converter") { _ = try converter.convert(source: source, destination: resources) }
        try rejects("missing game") { _ = try COD4Source.discover(destination) }
        let layout = try COD4Source.discover(source)
        let retail = temp.appendingPathComponent("Retail/Call of Duty 4.app")
        let retailContents = retail.appendingPathComponent("Contents")
        try fm.createDirectory(at: retailContents.appendingPathComponent("MacOS"), withIntermediateDirectories: true)
        try fm.createDirectory(at: retailContents.appendingPathComponent("Call of Duty 4 Data/main"), withIntermediateDirectories: true)
        try thin.write(to: retailContents.appendingPathComponent("MacOS/COD4 Retail"))
        try Data("game".utf8).write(to: retailContents.appendingPathComponent("Call of Duty 4 Data/main/iw_00.iwd"))
        let retailInfo = ["CFBundleIdentifier": "com.aspyr.callofduty4.retail", "CFBundleShortVersionString": "1.0",
                          "CFBundleExecutable": "COD4 Retail"]
        try PropertyListSerialization.data(fromPropertyList: retailInfo, format: .xml, options: 0)
            .write(to: retailContents.appendingPathComponent("Info.plist"))
        let retailLayout = try COD4Source.discover(retail)
        try expect(retailLayout.image.lastPathComponent == "COD4 Retail" && retailLayout.steamLibrary == nil,
                   "non-Steam executable and layout accepted")
        let directMP = temp.appendingPathComponent("Standalone/Call of Duty 4 Multiplayer.app")
        let directContents = directMP.appendingPathComponent("Contents")
        try fm.createDirectory(at: directContents.appendingPathComponent("MacOS"), withIntermediateDirectories: true)
        try fm.createDirectory(at: directMP.deletingLastPathComponent().appendingPathComponent("Call of Duty 4 Data/main"), withIntermediateDirectories: true)
        try thin.write(to: directContents.appendingPathComponent("MacOS/COD4 MP"))
        try Data("game".utf8).write(to: directMP.deletingLastPathComponent().appendingPathComponent("Call of Duty 4 Data/main/iw_00.iwd"))
        let directInfo = ["CFBundleExecutable": "COD4 MP"]
        try PropertyListSerialization.data(fromPropertyList: directInfo, format: .xml, options: 0)
            .write(to: directContents.appendingPathComponent("Info.plist"))
        try expect(try COD4Source.discover(directMP, mode: .mp).image.lastPathComponent == "COD4 MP",
                   "standalone multiplayer app accepted")
        let copy = temp.appendingPathComponent("data-copy")
        try copyGameTree(layout.gameData, to: copy)
        try expect(try Data(contentsOf: copy.appendingPathComponent("main/iw_00.iwd")) == Data("game".utf8), "independent data copy")
        let linked = layout.gameData.appendingPathComponent("escape")
        try fm.createSymbolicLink(at: linked, withDestinationURL: destination)
        try rejects("source links") { try copyGameTree(layout.gameData, to: temp.appendingPathComponent("linked-copy")) }
        try fm.removeItem(at: linked)
        try fm.removeItem(at: layout.gameData.appendingPathComponent("main/iw_00.iwd"))
        try rejects("incomplete data") { _ = try COD4Source.discover(source) }
        try Data("game".utf8).write(to: layout.gameData.appendingPathComponent("main/iw_00.iwd"))
        print("PASS: SP/MP layouts, Steam discovery, other editions, copy, links, containment and missing data")

        let cod2 = steamRoot.appendingPathComponent("steamapps/common/Call of Duty 2/Call of Duty 2.app")
        for mode in GameMode.allCases {
            let layout = COD4Source(app: cod2, mode: mode, game: .cod2)
            for directory in [layout.contents.appendingPathComponent("MacOS"), layout.resources, layout.gameData.appendingPathComponent("main")] {
                try fm.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            let original = "Call of Duty 2" + (mode == .mp ? " Multiplayer" : "")
            let info: [String: Any] = ["CFBundleExecutable": original, "CFBundleIdentifier": "test.cod2.\(mode.rawValue)"]
            let plist = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            try plist.write(to: layout.contents.appendingPathComponent("Info.plist"))
            try thin.write(to: layout.image)
            try Data("cod2 game".utf8).write(to: layout.gameData.appendingPathComponent("main/iw_00.iwd"))
            for selection in [cod2, cod2.deletingLastPathComponent(), steamRoot] {
                try expect(try GameTitle.cod2.discover(selection, mode: mode).image == layout.image, "COD2 folder and mode discovery")
            }
            try expect(GameTitle.cod2.stem(mode) == (mode == .sp ? "COD2" : "COD2MP"), "COD2 output names")
            try expect(GameTitle.cod2.appID(mode) == 2630, "COD2 shares one Steam app ID")
            try rejects("output inside COD2 data") { try layout.checkDestination(layout.gameData) }
            var generated = info
            generated["LP32GeneratedGame"] = "cod2"
            try PropertyListSerialization.data(fromPropertyList: generated, format: .xml, options: 0)
                .write(to: layout.contents.appendingPathComponent("Info.plist"))
            try rejects("converted COD2 input") { _ = try layout.validateLayout() }
            try plist.write(to: layout.contents.appendingPathComponent("Info.plist"))
        }
        let cod2Nested = cod2.appendingPathComponent("Contents/Call of Duty 2 Multiplayer.app")
        try expect(try GameTitle.cod2.discover(cod2Nested, mode: .mp).app.path == cod2.path, "COD2 nested multiplayer selection")
        let cod2Manifest = steamRoot.appendingPathComponent("steamapps/appmanifest_2630.acf")
        try "\"installdir\" \"Call of Duty 2\"".write(to: cod2Manifest, atomically: true, encoding: .utf8)
        try expect(GameTitle.cod2.steamInstallation(mode: .mp, home: home)?.path == cod2.path, "COD2 external Steam library")
        try expect(GameTitle.cod2.isCarbon && !GameTitle.cod2.usesGLMetal, "COD2 uses private runtime and Apple OpenGL")
        print("PASS: COD2 campaign/multiplayer discovery, Steam, output names and source protection")

        for game in [GameTitle.mw2, .mw3] {
            let library = temp.appendingPathComponent("\(game.short) Steam Library")
            let install = library.appendingPathComponent("steamapps/common/\(game.title)")
            let data = install.appendingPathComponent("GameData/main")
            try fm.createDirectory(at: data, withIntermediateDirectories: true)
            try Data("original assets".utf8).write(to: data.appendingPathComponent("iw_00.iwd"))
            for mode in GameMode.allCases {
                let app = install.appendingPathComponent("COD_\(game.short)_\(mode.rawValue.uppercased()).app")
                let mw = ModernWarfareSource(app: app, mode: mode, game: game)
                try fm.createDirectory(at: mw.contents.appendingPathComponent("MacOS"), withIntermediateDirectories: true)
                try fm.createDirectory(at: mw.resources, withIntermediateDirectories: true)
                try thin.write(to: mw.image)
                try thin64.write(to: mw.steamLibrary!)
                for file in mw.guestLibraries { try thin.write(to: file) }
                try Data("launcher must not be selected".utf8).write(to: mw.contents.appendingPathComponent("MacOS/" + mw.originalExecutable))
                let info = ["CFBundleExecutable": mw.originalExecutable]
                let plist = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
                try plist.write(to: mw.contents.appendingPathComponent("Info.plist"))
                let acf = library.appendingPathComponent("steamapps/appmanifest_\(game.appID(mode)).acf")
                let complete = "\"StateFlags\" \"4\" \"installdir\" \"\(game.title)\""
                try complete.write(to: acf, atomically: true, encoding: .utf8)
                for selection in [app, install] {
                    let found = try game.discover(selection, mode: mode)
                    try expect(found.image == mw.image && found.gameData == mw.gameData, "\(game.short) \(mode.rawValue) selects game sub executable and shared data")
                }
                let otherMode: GameMode = mode == .sp ? .mp : .sp
                try rejects("wrong MW mode") { _ = try game.discover(app, mode: otherMode) }
                let otherGame: GameTitle = game == .mw2 ? .mw3 : .mw2
                try rejects("wrong MW title") { _ = try otherGame.discover(app, mode: mode) }
                for unsafe in [app, app.appendingPathComponent("output"), mw.gameData, mw.gameData.appendingPathComponent("output")] {
                    try rejects("output inside MW source or data") { try mw.checkDestination(unsafe) }
                }
                let alias = temp.appendingPathComponent("\(game.short)-\(mode.rawValue)-data-link")
                try fm.createSymbolicLink(at: alias, withDestinationURL: mw.gameData)
                try rejects("output symlink into MW data") { try mw.checkDestination(alias) }
                try mw.checkDestination(destination)
                let mwHome = temp.appendingPathComponent("\(game.short)-\(mode.rawValue)-home")
                let mwSteam = mwHome.appendingPathComponent("Library/Application Support/Steam/steamapps")
                try fm.createDirectory(at: mwSteam, withIntermediateDirectories: true)
                try "\"path\" \"\(library.path)\"".write(to: mwSteam.appendingPathComponent("libraryfolders.vdf"), atomically: true, encoding: .utf8)
                try expect(game.steamInstallation(mode: mode, home: mwHome)?.path == app.path, "MW external Steam library discovery")
                try "\"StateFlags\" \"1026\" \"installdir\" \"\(game.title)\"".write(to: acf, atomically: true, encoding: .utf8)
                try rejects("pending MW download") { _ = try game.discover(app, mode: mode) }
                try expect(game.steamInstallation(mode: mode, home: mwHome) == nil, "pending MW installation excluded")
                try complete.write(to: acf, atomically: true, encoding: .utf8)
                let downloading = ModernWarfareSource(app: library.appendingPathComponent("steamapps/downloading/\(game.appID(mode))/\(app.lastPathComponent)"), mode: mode, game: game)
                try rejects("Steam downloading tree") { _ = try downloading.validateLayout() }
                if game == .mw3 && mode == .mp {
                    try expect(mw.guestLibraries.first?.lastPathComponent == "libBink2Macx86.dylib", "MW3 MP uses Bink 2")
                    try expect(!fm.fileExists(atPath: mw.contents.appendingPathComponent("MacOS/libBinkMacx86.dylib").path), "MW3 MP does not require Bink 1")
                }
                let required = mw.guestLibraries[0]
                try fm.removeItem(at: required)
                try rejects("missing MW Bink library") { _ = try mw.validateLayout() }
                try thin64.write(to: required)
                try rejects("wrong MW guest library architecture") { _ = try mw.validateLayout() }
                try thin.write(to: required)
                var generated: [String: Any] = info
                generated["LP32GeneratedGame"] = game.rawValue
                try PropertyListSerialization.data(fromPropertyList: generated, format: .xml, options: 0).write(to: mw.contents.appendingPathComponent("Info.plist"))
                try rejects("converted MW input") { _ = try mw.validateLayout() }
                try plist.write(to: mw.contents.appendingPathComponent("Info.plist"))
            }
        }
        print("PASS: MW2/MW3 SP/MP layouts, Bink 2, Steam downloads, modes and destination preservation")

        let metal = resources.deletingLastPathComponent().appendingPathComponent("Frameworks/GLMetal")
        try fm.createDirectory(at: metal, withIntermediateDirectories: true)
        let metalManifest = resources.appendingPathComponent("GLMetal-build-info.json")
        try rejects("missing GLMetal manifest") { _ = try converter.validateGLMetal() }
        try Data("invalid JSON".utf8).write(to: metalManifest)
        try rejects("malformed GLMetal manifest") { _ = try converter.validateGLMetal() }
        try Data("{}".utf8).write(to: metalManifest)
        try rejects("missing GLMetal manifest entries") { _ = try converter.validateGLMetal() }
        var files: [String: [String: String]] = [:]
        for name in ["libGLMetal.dylib", "glmetal-compiler"] {
            let file = metal.appendingPathComponent(name)
            try fm.copyItem(at: URL(fileURLWithPath: "/usr/bin/true"), to: file)
            try converter.run("/usr/bin/codesign", ["--force", "--sign", "-", file.path])
            let hash = try converter.sha256(file)
            files[name] = ["source_sha256": hash, "bundled_sha256": hash]
        }
        let validManifest = try JSONSerialization.data(withJSONObject: ["files": files])
        try validManifest.write(to: metalManifest)
        try expect(try converter.validateGLMetal().path == metal.path, "packaged hashes and signatures validate without running GPU code")
        for name in ["libGLMetal.dylib", "glmetal-compiler"] {
            let file = metal.appendingPathComponent(name)
            let original = try Data(contentsOf: file)
            try fm.removeItem(at: file)
            try rejects("missing packaged GLMetal file") { _ = try converter.validateGLMetal() }
            try original.write(to: file)
            try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: file.path)
            try rejects("nonexecutable packaged GLMetal file") { _ = try converter.validateGLMetal() }
            try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: file.path)
            var damaged = original
            damaged.append(0)
            try damaged.write(to: file)
            try rejects("damaged packaged GLMetal file") { _ = try converter.validateGLMetal() }
            try original.write(to: file)
            try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: file.path)
        }
        var wrongFiles = files
        wrongFiles["glmetal-compiler"]?["bundled_sha256"] = String(repeating: "0", count: 64)
        try JSONSerialization.data(withJSONObject: ["files": wrongFiles]).write(to: metalManifest)
        try rejects("incorrect packaged GLMetal manifest digest") { _ = try converter.validateGLMetal() }
        try validManifest.write(to: metalManifest)
        print("PASS: GLMetal manifests, hashes, executable files and signatures without GPU access")

        let existing = destination.appendingPathComponent("COD4-Compat.app")
        try fm.createDirectory(at: existing, withIntermediateDirectories: false)
        let save = existing.appendingPathComponent("SAVE")
        try Data("existing checkpoint".utf8).write(to: save)
        let resultLock = NSLock()
        var outputs: [URL] = []
        var failures: [Error] = []
        DispatchQueue.concurrentPerform(iterations: 12) { index in
            do {
                let stage = temp.appendingPathComponent("stage-\(index)")
                try fm.createDirectory(at: stage, withIntermediateDirectories: false)
                let output = try publish(stage, in: destination)
                resultLock.lock()
                outputs.append(output)
                resultLock.unlock()
            } catch {
                resultLock.lock()
                failures.append(error)
                resultLock.unlock()
            }
        }
        try expect(failures.isEmpty && Set(outputs).count == 12, "concurrent conversions have unique output paths")
        try expect(try String(contentsOf: save, encoding: .utf8) == "existing checkpoint", "existing save not overwritten")
        print("PASS: concurrent output publication preserves existing saves")

        let literal = "Call of Duty 4's $(touch SHOULD_NOT_EXIST) `date`; app"
        try expect(try converter.run("/usr/bin/printf", ["%s", literal]) == literal, "paths are passed as literal arguments")
        try rejects("failed child command") { try converter.run("/usr/bin/false", []) }
        // An Intel-only executable run as arm64 fails like any Intel one without Rosetta.
        let intelOnly = temp.appendingPathComponent("intel-only")
        try converter.run("/usr/bin/lipo", ["/usr/bin/true", "-thin", "x86_64", "-output", intelOnly.path])
        do {
            try converter.checkRosetta(["-arm64", intelOnly.path])
            throw ConversionError.message("FAIL: accepted a missing Rosetta")
        } catch ConversionError.message(let text) {
            try expect(text.contains("Rosetta 2 is not installed") && text.contains("softwareupdate --install-rosetta"),
                       "missing Rosetta explains how to install it: \(text)")
        }
        try converter.checkRosetta()
        let started = Date()
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.2) { converter.cancellation.cancel() }
        do {
            try converter.run("/bin/sleep", ["30"])
            throw ConversionError.message("FAIL: child was not cancelled")
        } catch ConversionError.cancelled {}
        try expect(Date().timeIntervalSince(started) < 5, "cancellation promptly stops child")
        try rejects("cancelled conversion") { _ = try converter.convert(source: source, destination: destination) }
        try expect(try fm.contentsOfDirectory(atPath: destination.path).count == 13, "cancellation creates no unfinished output")
        try expect(try Data(contentsOf: source.appendingPathComponent("Contents/MacOS/Call of Duty 4")) == thin, "source remains untouched")
        print("PASS: literal arguments, command failure, missing Rosetta, cancellation and source preservation")
    }
}
