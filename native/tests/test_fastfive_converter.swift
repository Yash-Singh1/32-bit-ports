import Foundation

@main
struct FastFiveConverterTests {
    static func main() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("fastfive-tests-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: root) }
        let app = root.appendingPathComponent("FastFive.app")
        let contents = app.appendingPathComponent("Contents")
        let image = contents.appendingPathComponent("MacOS/FastFive")
        let pack = contents.appendingPathComponent("Resources/res/file000000.dat")
        for directory in [image.deletingLastPathComponent(), pack.deletingLastPathComponent()] {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        func check(_ condition: @autoclosure () throws -> Bool, line: Int = #line) throws {
            guard try condition() else { throw ConversionError.message("Fast Five source check failed at line \(line)") }
        }
        func rejects(_ body: () throws -> Void) throws {
            do { try body() } catch { return }
            throw ConversionError.message("Invalid source was accepted")
        }
        let thin = Data([0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0] + [UInt8](repeating: 0, count: 20))
        var info: [String: Any] = ["CFBundleIdentifier": FastFive.bundleID, "CFBundleExecutable": "FastFive"]
        func writeInfo() throws {
            try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
                .write(to: contents.appendingPathComponent("Info.plist"))
        }
        try writeInfo()
        try thin.write(to: image)
        try Data([1]).write(to: pack)
        try check(try FastFiveSource.discover(app).app.path == app.resolvingSymlinksInPath().path)
        try check(try FastFiveSource.discover(root).app.resolvingSymlinksInPath().path == app.resolvingSymlinksInPath().path)
        try check(try FastFiveSource.discover(app).receipt == nil)
        let receipt = contents.appendingPathComponent("_MASReceipt/receipt")
        try fm.createDirectory(at: receipt.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data([0x30]).write(to: receipt)
        try check(try FastFiveSource.discover(app).receipt?.lastPathComponent == "receipt")
        info["CFBundleIdentifier"] = "com.gameloft.asphalt6mac"
        try writeInfo()
        try rejects { _ = try FastFiveSource.discover(app) }
        info["CFBundleIdentifier"] = FastFive.bundleID
        info["LP32GeneratedGame"] = "fastfive"
        try writeInfo()
        try rejects { _ = try FastFiveSource.discover(app) }
        info.removeValue(forKey: "LP32GeneratedGame")
        info["CFBundleExecutable"] = "../other"
        try writeInfo()
        try rejects { _ = try FastFiveSource.discover(app) }
        info["CFBundleExecutable"] = "FastFive"
        try writeInfo()
        try Data().write(to: pack)
        try rejects { _ = try FastFiveSource.discover(app) }
        try Data([1]).write(to: pack)
        var wrongCPU = thin
        wrongCPU[4] = 12
        try wrongCPU.write(to: image)
        try rejects { _ = try FastFiveSource.discover(app) }
        try check(FastFiveSource.isArchive(URL(fileURLWithPath: "Fast_Five_1.0.ZIP")))
        try check(FastFiveSource.isArchive(URL(fileURLWithPath: "FastFive.dmg")))
        print("PASS: Fast Five source identity, original-only conversion, data validation, App Store receipt and i386 requirement")
    }
}
