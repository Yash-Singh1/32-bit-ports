import Foundation

@main
struct Asphalt6ConverterTests {
    static func main() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("asphalt6-tests-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: root) }
        let app = root.appendingPathComponent("Asphalt 6.app")
        let contents = app.appendingPathComponent("Contents")
        let image = contents.appendingPathComponent("MacOS/Asphalt 6")
        let pack = contents.appendingPathComponent("Resources/data/file000000.dat")
        for directory in [image.deletingLastPathComponent(), pack.deletingLastPathComponent()] {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        func check(_ condition: @autoclosure () throws -> Bool, line: Int = #line) throws {
            guard try condition() else { throw ConversionError.message("Asphalt 6 source check failed at line \(line)") }
        }
        func rejects(_ body: () throws -> Void) throws {
            do { try body() } catch { return }
            throw ConversionError.message("Invalid source was accepted")
        }
        let thin = Data([0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0] + [UInt8](repeating: 0, count: 20))
        var info: [String: Any] = ["CFBundleIdentifier": Asphalt6.bundleID, "CFBundleExecutable": "Asphalt 6"]
        func writeInfo() throws {
            try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
                .write(to: contents.appendingPathComponent("Info.plist"))
        }
        try writeInfo()
        try thin.write(to: image)
        try Data([1]).write(to: pack)
        try check(try Asphalt6Source.discover(app).app.path == app.resolvingSymlinksInPath().path)
        try check(try Asphalt6Source.discover(root).app.resolvingSymlinksInPath().path == app.resolvingSymlinksInPath().path)
        info["CFBundleIdentifier"] = "com.gameloft.gangstar2mac"
        try writeInfo()
        try rejects { _ = try Asphalt6Source.discover(app) }
        info["CFBundleIdentifier"] = Asphalt6.bundleID
        info["LP32GeneratedGame"] = "asphalt6"
        try writeInfo()
        try rejects { _ = try Asphalt6Source.discover(app) }
        info.removeValue(forKey: "LP32GeneratedGame")
        info["CFBundleExecutable"] = "../other"
        try writeInfo()
        try rejects { _ = try Asphalt6Source.discover(app) }
        info["CFBundleExecutable"] = "Asphalt 6"
        try writeInfo()
        try Data().write(to: pack)
        try rejects { _ = try Asphalt6Source.discover(app) }
        try Data([1]).write(to: pack)
        var wrongCPU = thin
        wrongCPU[4] = 12
        try wrongCPU.write(to: image)
        try rejects { _ = try Asphalt6Source.discover(app) }
        try check(Asphalt6Source.isArchive(URL(fileURLWithPath: "Asphalt.ZIP")))
        try check(Asphalt6Source.isArchive(URL(fileURLWithPath: "Asphalt.dmg")))
        print("PASS: Asphalt 6 source identity, original-only conversion, data validation and i386 requirement")
    }
}
