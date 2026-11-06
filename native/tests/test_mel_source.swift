import Foundation

private func expect(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
    if try !value() { throw ConversionError.message("FAIL: " + message) }
}
private func rejects(_ body: () throws -> Void) throws {
    do { try body() } catch { return }
    throw ConversionError.message("FAIL: accepted invalid source or destination")
}

@main
struct MelSourceTests {
    static func main() throws {
        let fm = FileManager.default
        let temp = fm.temporaryDirectory.resolvingSymlinksInPath()
            .appendingPathComponent("mel-tests-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: temp) }
        func fixture(_ root: URL, _ path: String, _ text: String = "fixture") throws {
            let file = root.appendingPathComponent(path)
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(text.utf8).write(to: file)
        }
        let thin = Data([0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0, 3, 0, 0, 0,
                         2, 0, 0, 0] + [UInt8](repeating: 0, count: 16))
        func complete(_ root: URL) throws {
            try fixture(root, "portal_stories/gameinfo.txt")
            try fixture(root, "bin/osx32/launcher.dylib")
            try thin.write(to: root.appendingPathComponent("portal2_osx"))
        }
        let source = temp.appendingPathComponent("Portal Stories Mel ' $ `")
        try complete(source)
        try fixture(source, "portal_stories/resource/game.icns")
        try fixture(source, "portal_stories/SAVE/quick.sav", "source save")
        try fixture(source, "portal_stories/cfg/config.cfg", "source config")
        let layout = try MelSource.discover(source)
        let wrapper = temp.appendingPathComponent("Mel.app")
        try complete(wrapper.appendingPathComponent("Contents/MacOS"))
        try expect(try MelSource.discover(wrapper).root == wrapper.appendingPathComponent("Contents/MacOS"), "app discovery")
        let portal = temp.appendingPathComponent("Portal 2")
        try fixture(portal, "portal2/gameinfo.txt")
        try fixture(portal, "bin/osx32/launcher.dylib")
        try thin.write(to: portal.appendingPathComponent("portal2_osx"))
        try rejects { _ = try MelSource.discover(portal) }
        try rejects { try layout.prepareBundle(at: source.appendingPathComponent("output.app")) }
        let linked = temp.appendingPathComponent("linked.app/Contents")
        try fm.createDirectory(at: linked, withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: linked.appendingPathComponent("SharedSupport"), withDestinationURL: source)
        try rejects { try layout.prepareBundle(at: linked.deletingLastPathComponent()) }
        let app = temp.appendingPathComponent("Output.app")
        try layout.prepareBundle(at: app)
        let game = app.appendingPathComponent("Contents/SharedSupport/PortalStoriesMel")
        try expect(try Data(contentsOf: app.appendingPathComponent("Contents/SharedSupport/PortalStoriesMel.image")) == thin, "launcher bytes")
        try fixture(game, "portal_stories/SAVE/quick.sav", "progress")
        try fixture(game, "portal_stories/cfg/config.cfg", "settings")
        try fixture(game, "bin/obsolete.dylib")
        try layout.prepareBundle(at: app)
        try expect(try String(contentsOf: game.appendingPathComponent("portal_stories/SAVE/quick.sav")) == "progress", "save retention")
        try expect(try String(contentsOf: game.appendingPathComponent("portal_stories/cfg/config.cfg")) == "settings", "config retention")
        try expect(try String(contentsOf: source.appendingPathComponent("portal_stories/SAVE/quick.sav")) == "source save", "source untouched")
        try expect(!fm.fileExists(atPath: game.appendingPathComponent("bin/obsolete.dylib").path), "stale library removed")
        let stage = temp.appendingPathComponent("staged.app")
        try fixture(stage, "marker")
        let published = try publish(stage, in: temp)
        try expect(published.lastPathComponent == "PortalStoriesMel-Compat.app", "Mel output identity")
        try fixture(stage, "marker")
        try expect(try publish(stage, in: temp).lastPathComponent == "PortalStoriesMel-Compat 2.app", "existing conversion preserved")
        print("PASS: Mel discovery, isolation, saves/config retention, fresh libraries and numbered outputs")
    }
}
