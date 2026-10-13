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
struct L4D2SourceTests {
    static func main() throws {
        let fm = FileManager.default
        let temp = fm.temporaryDirectory.resolvingSymlinksInPath()
            .appendingPathComponent("l4d2-tests-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        // A small i386 MH_EXECUTE header is sufficient for packaging validation.
        let thin = Data([0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0, 3, 0, 0, 0,
                         2, 0, 0, 0] + [UInt8](repeating: 0, count: 16))
        func fixture(_ root: URL, _ path: String, _ value: String = "fixture") throws {
            let file = root.appendingPathComponent(path)
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(value.utf8).write(to: file)
        }
        func complete(_ root: URL, library: String = "bin/osx32/launcher.dylib") throws {
            try fixture(root, "hl2_osx")
            try thin.write(to: root.appendingPathComponent("hl2_osx"))
            try fixture(root, "left4dead2/gameinfo.txt")
            try fixture(root, library)
        }
        let source = temp.appendingPathComponent("Left 4 Dead 2 ' $ ` literal")
        try complete(source)
        let layout = try L4D2Source.discover(source)
        try expect(layout.root.path == source.path && layout.image.path == source.appendingPathComponent("hl2_osx").path, "complete flat game")
        let wrapper = temp.appendingPathComponent("Left 4 Dead 2.app")
        try complete(wrapper.appendingPathComponent("Contents/MacOS"), library: "bin/launcher.dylib")
        try fixture(wrapper, "Contents/Resources/game.icns")
        let appLayout = try L4D2Source.discover(wrapper)
        try expect(appLayout.root.path == wrapper.appendingPathComponent("Contents/MacOS").path && appLayout.icon != nil,
                   "complete app wrapper and original icon")
        try rejects("parent-folder scanning") { _ = try L4D2Source.discover(temp) }
        let missing = temp.appendingPathComponent("missing-content")
        try fixture(missing, "hl2_osx")
        try rejects("missing content") { _ = try L4D2Source.discover(missing) }
        try fixture(missing, "left4dead2/gameinfo.txt")
        try rejects("missing Mac launcher library") { _ = try L4D2Source.discover(missing) }
        try fixture(missing, "bin/launcher.so")
        try rejects("Linux-only tree") { _ = try L4D2Source.discover(missing) }
        try fixture(missing, "bin/launcher.dylib")
        try rejects("truncated Mach-O") { _ = try L4D2Source.discover(missing) }
        var dylib = thin
        dylib[12] = 6
        try dylib.write(to: missing.appendingPathComponent("hl2_osx"))
        try rejects("dylib posing as a launcher") { _ = try L4D2Source.discover(missing) }
        var wrongCPU = thin
        wrongCPU[4] = 12
        try wrongCPU.write(to: missing.appendingPathComponent("hl2_osx"))
        try rejects("non-i386 launcher") { _ = try L4D2Source.discover(missing) }
        print("PASS: complete flat/wrapper discovery and missing/non-Mac launcher rejection")

        let alias = temp.appendingPathComponent("source-link")
        try fm.createSymbolicLink(at: alias, withDestinationURL: source)
        try rejects("output under original source") { try layout.prepareBundle(at: source.appendingPathComponent("output.app")) }
        try rejects("output under source link") { try layout.prepareBundle(at: alias.appendingPathComponent("output.app")) }
        let redirected = temp.appendingPathComponent("redirected.app/Contents")
        try fm.createDirectory(at: redirected, withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: redirected.appendingPathComponent("SharedSupport"), withDestinationURL: source)
        try rejects("output parent linked to source") { try layout.prepareBundle(at: redirected.deletingLastPathComponent()) }

        let external = temp.appendingPathComponent("external")
        try fixture(external, "keep", "untouched")
        let first = temp.appendingPathComponent("overlay-first")
        let second = temp.appendingPathComponent("overlay-second")
        try fm.createDirectory(at: first, withIntermediateDirectories: false)
        try fm.createSymbolicLink(at: first.appendingPathComponent("overlay"), withDestinationURL: external)
        try fixture(second, "overlay/keep", "new copy")
        let overlay = temp.appendingPathComponent("merged")
        try copySourceGameRoots([first, second], to: overlay)
        try expect(try String(contentsOf: external.appendingPathComponent("keep")) == "untouched", "overlay never writes through a copied link")
        try expect(try String(contentsOf: overlay.appendingPathComponent("overlay/keep")) == "new copy", "overlay replaces the link itself")
        print("PASS: source containment and copied-link overlay protection")

        for path in L4D2Source.personalPaths { try fixture(source, path, "factory") }
        try fixture(source, "left4dead2/resource/game.icns")
        try fixture(source, ".DepotDownloader/staging/partial")
        try fm.createSymbolicLink(at: source.appendingPathComponent("compat-runtime"), withDestinationURL: external)
        let output = temp.appendingPathComponent("Output ' $.app")
        let rich = try L4D2Source.discover(source)
        try rich.prepareBundle(at: output)
        let game = output.appendingPathComponent("Contents/SharedSupport/L4D2")
        try expect(try Data(contentsOf: output.appendingPathComponent("Contents/SharedSupport/L4D2.image")) == thin, "private launcher image bytes")
        try expect(fm.fileExists(atPath: output.appendingPathComponent("Contents/Resources/game.icns").path), "original game icon")
        try expect(!fm.fileExists(atPath: game.appendingPathComponent(".DepotDownloader").path), "download metadata excluded")
        try expect((try? fm.attributesOfItem(atPath: game.appendingPathComponent("compat-runtime").path)) == nil, "input runtime link removed before private runtime install")
        for path in L4D2Source.personalPaths { try fixture(game, path, "user settings") }
        try fixture(game, "bin/obsolete.dylib")
        try fixture(source, "bin/osx32/launcher.dylib", "updated library")
        try rich.prepareBundle(at: output)
        for path in L4D2Source.personalPaths {
            try expect(try String(contentsOf: game.appendingPathComponent(path)) == "user settings", "retain settings: " + path)
            try expect(try String(contentsOf: source.appendingPathComponent(path)) == "factory", "original settings unchanged")
        }
        try expect(!fm.fileExists(atPath: game.appendingPathComponent("bin/obsolete.dylib").path), "stale libraries removed")
        try expect(try String(contentsOf: game.appendingPathComponent("bin/osx32/launcher.dylib")) == "updated library", "fresh library selected")
        try expect(try String(contentsOf: external.appendingPathComponent("keep")) == "untouched", "runtime link target unchanged")
        try expect(!(try fm.contentsOfDirectory(atPath: output.appendingPathComponent("Contents/SharedSupport").path)).contains { $0.hasPrefix(".l4d2-source-") }, "staging cleanup")
        print("PASS: complete bundle layout, source unchanged, settings retention and stale-library cleanup")

        // Retaining settings must also avoid a link in the newly copied cfg.
        let cfg = source.appendingPathComponent("left4dead2/cfg")
        try fm.removeItem(at: cfg)
        try fm.createSymbolicLink(at: cfg, withDestinationURL: external)
        try rich.prepareBundle(at: output)
        try expect(try String(contentsOf: external.appendingPathComponent("keep")) == "untouched", "cfg link target unchanged")
        try expect(try String(contentsOf: game.appendingPathComponent("left4dead2/cfg/config.cfg")) == "user settings", "settings replace copied cfg link")
        try expect((try fm.attributesOfItem(atPath: game.appendingPathComponent("left4dead2/cfg").path)[.type] as? FileAttributeType) == .typeDirectory,
                   "converted settings have private storage rather than source links")
        let externalSetting = external.appendingPathComponent("config.cfg")
        try Data("user settings".utf8).write(to: externalSetting)
        let oldSetting = game.appendingPathComponent("left4dead2/cfg/config.cfg")
        try fm.removeItem(at: oldSetting)
        try fm.createSymbolicLink(at: oldSetting, withDestinationURL: externalSetting)
        try rich.prepareBundle(at: output)
        try expect((try fm.attributesOfItem(atPath: oldSetting.path)[.type] as? FileAttributeType) == .typeRegular,
                   "retained settings links become private files")

        // A launcher link is read into a private image rather than chmodded.
        let realImage = temp.appendingPathComponent("original-executable")
        try thin.write(to: realImage)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: realImage.path)
        try fm.removeItem(at: source.appendingPathComponent("hl2_osx"))
        try fm.createSymbolicLink(at: source.appendingPathComponent("hl2_osx"), withDestinationURL: realImage)
        try L4D2Source.discover(source).prepareBundle(at: output)
        try expect((try fm.attributesOfItem(atPath: realImage.path)[.posixPermissions] as? NSNumber)?.intValue == 0o755, "original launcher permissions unchanged")
        try fm.createSymbolicLink(at: source.appendingPathComponent("recursive"), withDestinationURL: source)
        try rejects("recursive source link") { try rich.prepareBundle(at: output) }
        try expect(try String(contentsOf: game.appendingPathComponent("left4dead2/cfg/config.cfg")) == "user settings", "failed copy preserves previous converted settings")
        print("PASS: settings and launcher links cannot redirect bundle writes")
    }
}
