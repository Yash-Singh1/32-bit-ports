import Foundation

// Overlay complete trees without writing through a link supplied by an earlier
// tree. Used by both Make helpers and the existing Portal converter.
func copySourceGameRoots(_ roots: [URL], to destination: URL,
                         materializeLinks: Bool = false,
                         check: () throws -> Void = {}) throws {
    let fm = FileManager.default
    func copy(_ source: URL, _ target: URL, ancestors: Set<String> = []) throws {
        try check()
        if source.lastPathComponent == ".DepotDownloader" { return }
        let old = try? fm.attributesOfItem(atPath: target.path)
        var copiedSource = source
        var attributes = try fm.attributesOfItem(atPath: source.path)
        if materializeLinks && attributes[.type] as? FileAttributeType == .typeSymbolicLink {
            copiedSource = source.resolvingSymlinksInPath()
            attributes = try fm.attributesOfItem(atPath: copiedSource.path)
        }
        if attributes[.type] as? FileAttributeType == .typeDirectory {
            let canonical = copiedSource.resolvingSymlinksInPath().path
            guard !ancestors.contains(canonical) else {
                throw ConversionError.message("The game files contain a recursive directory link: \(source.path)")
            }
            if let old, old[.type] as? FileAttributeType != .typeDirectory { try fm.removeItem(at: target) }
            try fm.createDirectory(at: target, withIntermediateDirectories: true)
            for item in try fm.contentsOfDirectory(at: copiedSource, includingPropertiesForKeys: nil) {
                try copy(item, target.appendingPathComponent(item.lastPathComponent), ancestors: ancestors.union([canonical]))
            }
        } else {
            if old != nil { try fm.removeItem(at: target) }
            try fm.copyItem(at: copiedSource, to: target)
        }
    }
    for root in roots { try copy(root, destination) }
}

// Shared by the GUI converter and Makefile. Depot downloads are overlaid into
// one private game root; no links back to the user's download are introduced.
struct Portal2Source {
    static let personalPaths = ["portal2/SAVE", "portal2/save", "portal2/cfg/config.cfg",
                                "portal2/cfg/video.txt", "portal2/cfg/videodefaults.txt"]
    let roots: [URL]
    let image: URL
    let resources: URL?
    let icon: URL?

    static func discover(_ selected: URL) throws -> Portal2Source {
        let fm = FileManager.default
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        func exists(_ root: URL, _ path: String) -> Bool {
            fm.fileExists(atPath: root.appendingPathComponent(path).path)
        }
        func content(_ root: URL) -> Bool {
            exists(root, "portal2_osx") && exists(root, "portal2/gameinfo.txt")
        }
        func client(_ root: URL) -> Bool {
            exists(root, "bin/osx32/launcher.dylib") || exists(root, "bin/launcher.dylib")
        }
        func result(_ roots: [URL], resources: URL? = nil) -> Portal2Source {
            let icons = (resources.map { [$0.appendingPathComponent("game.icns")] } ?? []) +
                roots.map { $0.appendingPathComponent("portal2/resource/game.icns") }
            return Portal2Source(roots: roots, image: roots[0].appendingPathComponent("portal2_osx"),
                                 resources: resources, icon: icons.first { fm.fileExists(atPath: $0.path) })
        }
        let appRoot = selected.appendingPathComponent("Contents/MacOS")
        if content(appRoot) && client(appRoot) {
            let resources = selected.appendingPathComponent("Contents/Resources")
            return result([appRoot], resources: exists(selected, "Contents/Resources") ? resources : nil)
        }
        // Accept the game, common, steamapps, or Steam library folder. Do not
        // crawl unrelated folders or silently combine multiple installations.
        for suffix in ["", "Portal 2", "common/Portal 2", "steamapps/common/Portal 2"] {
            let root = suffix.isEmpty ? selected : selected.appendingPathComponent(suffix)
            if content(root) && client(root) { return result([root]) }
        }
        var depotRoot = selected.appendingPathComponent("depots")
        if !exists(depotRoot, "621") && !exists(depotRoot, "623") {
            depotRoot = selected
            for _ in 0..<3 {
                if exists(depotRoot, "621") || exists(depotRoot, "623") { break }
                depotRoot.deleteLastPathComponent()
            }
        }
        func versions(_ id: String, matching predicate: (URL) -> Bool) -> [URL] {
            let root = depotRoot.appendingPathComponent(id)
            if predicate(root) { return [root] }
            return ((try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil,
                                                options: [.skipsHiddenFiles])) ?? [])
                .filter(predicate).sorted { $0.path < $1.path }
        }
        var common = versions("621", matching: content)
        var mac = versions("623", matching: client)
        // Selecting a specific downloaded version disambiguates that depot.
        if common.contains(where: { $0.resolvingSymlinksInPath().path == selected.path }) { common = [selected] }
        if mac.contains(where: { $0.resolvingSymlinksInPath().path == selected.path }) { mac = [selected] }
        if common.count == 1 && mac.count > 1 {
            mac = mac.filter { $0.lastPathComponent == common[0].lastPathComponent }
        } else if mac.count == 1 && common.count > 1 {
            common = common.filter { $0.lastPathComponent == mac[0].lastPathComponent }
        }
        if common.count == 1 && mac.count == 1 { return result([common[0], mac[0]]) }
        if common.count > 1 || mac.count > 1 {
            throw ConversionError.message("Several Portal 2 depot versions were found. Choose a specific version folder inside depot 621 or 623, or combine the matching depots into one game folder.")
        }
        throw ConversionError.message("Choose Portal 2.app, a Steam Portal 2 game folder, or the folder containing your downloaded depots. The download needs both the shared game content (depot 621) and the Mac client libraries (depot 623).")
    }

    func checkDestination(_ destination: URL) throws {
        guard !roots.contains(where: { isInside(destination, $0) }) else {
            throw ConversionError.message("Choose an output folder outside the original game files.")
        }
    }

    func copyGame(to destination: URL, check: () throws -> Void = {}) throws {
        try copySourceGameRoots(roots, to: destination, check: check)
    }
}
