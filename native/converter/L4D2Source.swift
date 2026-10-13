import Foundation
import Darwin

// L4D2 v1 accepts a complete installed or assembled Mac game tree. It does not
// search sibling depots or select historical versions on the user's behalf.
struct L4D2Source {
    static let personalPaths = ["left4dead2/cfg/config.cfg", "left4dead2/cfg/video.txt",
                                "left4dead2/cfg/videodefaults.txt"]
    // Include destinations written by Make after source copying and by
    // promote-loader. Never let a pre-existing output link redirect those
    // commands to a source file or another directory outside this bundle.
    static let outputWritePaths = ["", "Contents", "Contents/MacOS", "Contents/MacOS/L4D2Compat",
        "Contents/MacOS/L4D2Compat.new", "Contents/Info.plist", "Contents/Resources",
        "Contents/Resources/game.icns", "Contents/SharedSupport", "Contents/SharedSupport/L4D2",
        "Contents/SharedSupport/L4D2/compat-runtime", "Contents/SharedSupport/L4D2.image"]
    let selectedSource: URL
    let root: URL
    let image: URL
    let resources: URL?
    let icon: URL?

    static func discover(_ selected: URL) throws -> L4D2Source {
        let fm = FileManager.default
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        let candidates = [selected, selected.appendingPathComponent("Contents/MacOS")]
        for root in candidates {
            let image = root.appendingPathComponent("hl2_osx")
            guard fm.fileExists(atPath: image.path),
                  fm.fileExists(atPath: root.appendingPathComponent("left4dead2/gameinfo.txt").path),
                  ["bin/osx32/launcher.dylib", "bin/launcher.dylib"].contains(where: {
                      fm.fileExists(atPath: root.appendingPathComponent($0).path)
                  }) else { continue }
            let slice = try i386Slice(Data(contentsOf: image))
            // MH_EXECUTE: a dylib with the same CPU type is not a launcher.
            guard slice.count >= 28, Array(slice[12..<16]) == [2, 0, 0, 0] else {
                throw ConversionError.message("The L4D2 launcher is not a 32-bit Intel Mac executable.")
            }
            let appResources = selected.appendingPathComponent("Contents/Resources")
            let resources = root != selected && fm.fileExists(atPath: appResources.path) ? appResources : nil
            let icons = (resources.map { [$0.appendingPathComponent("game.icns")] } ?? []) +
                [root.appendingPathComponent("left4dead2/resource/game.icns")]
            return L4D2Source(selectedSource: selected, root: root, image: image, resources: resources,
                              icon: icons.first { fm.fileExists(atPath: $0.path) })
        }
        throw ConversionError.message("Choose a complete Left 4 Dead 2 Mac game folder containing hl2_osx, left4dead2/gameinfo.txt, and the Mac launcher.dylib. Separate depot folders must first be assembled into one game tree.")
    }

    func checkDestination(_ destination: URL) throws {
        guard !isInside(destination, selectedSource), !isInside(destination, root) else {
            throw ConversionError.message("Choose an output folder outside the original game files.")
        }
    }

    // This check needs no installed source, so loader promotion can remain
    // available when the original game volume is not mounted.
    static func validateBundleOutput(_ app: URL) throws {
        let fm = FileManager.default
        for relative in outputWritePaths {
            let path = relative.isEmpty ? app : app.appendingPathComponent(relative)
            if let attributes = try? fm.attributesOfItem(atPath: path.path),
               attributes[.type] as? FileAttributeType == .typeSymbolicLink {
                throw ConversionError.message("The output bundle contains a linked write destination: \(path.path). Choose a separate output app without these links.")
            }
        }
    }

    func copyGame(to destination: URL) throws {
        try checkDestination(destination)
        // Materialize source links so a running converted game cannot write
        // settings through a retained link into the original installation.
        try copySourceGameRoots([root], to: destination, materializeLinks: true)
    }

    func prepareBundle(at app: URL) throws {
        let fm = FileManager.default
        let output = app.appendingPathComponent("Contents")
        let shared = output.appendingPathComponent("SharedSupport")
        let resourcesOutput = output.appendingPathComponent("Resources")
        let game = shared.appendingPathComponent("L4D2")
        let imageOutput = shared.appendingPathComponent("L4D2.image")
        for relative in Self.outputWritePaths {
            try checkDestination(relative.isEmpty ? app : app.appendingPathComponent(relative))
        }
        try Self.validateBundleOutput(app)
        try fm.createDirectory(at: shared, withIntermediateDirectories: true)
        try fm.createDirectory(at: resourcesOutput, withIntermediateDirectories: true)
        let staged = shared.appendingPathComponent(".l4d2-source-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: staged) }
        try copyGame(to: staged)
        // Runtime libraries are always installed privately by Make; a source
        // link must not redirect the later ditto into the original installation.
        let runtime = staged.appendingPathComponent("compat-runtime")
        if (try? fm.attributesOfItem(atPath: runtime.path)) != nil { try fm.removeItem(at: runtime) }
        for path in Self.personalPaths {
            let old = game.appendingPathComponent(path)
            guard (try? fm.attributesOfItem(atPath: old.path)) != nil else { continue }
            let new = staged.appendingPathComponent(path)
            // Remove links in every staged ancestor before retaining settings.
            let parent = new.deletingLastPathComponent()
            var directory = staged
            for component in parent.pathComponents.dropFirst(staged.pathComponents.count) {
                directory.appendPathComponent(component)
                if let attributes = try? fm.attributesOfItem(atPath: directory.path) {
                    if attributes[.type] as? FileAttributeType == .typeDirectory { continue }
                    try fm.removeItem(at: directory)
                }
                try fm.createDirectory(at: directory, withIntermediateDirectories: false)
            }
            if (try? fm.attributesOfItem(atPath: new.path)) != nil { try fm.removeItem(at: new) }
            try copySourceGameRoots([old], to: new, materializeLinks: true)
        }
        if (try? fm.attributesOfItem(atPath: game.path)) != nil {
            guard renamex_np(staged.path, game.path, UInt32(RENAME_SWAP)) == 0 else {
                throw ConversionError.message("Could not replace the game's data: \(String(cString: strerror(errno)))")
            }
        } else { try fm.moveItem(at: staged, to: game) }
        if (try? fm.attributesOfItem(atPath: imageOutput.path)) != nil { try fm.removeItem(at: imageOutput) }
        try Data(contentsOf: image).write(to: imageOutput)
        try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: imageOutput.path)
        if let icon {
            let target = resourcesOutput.appendingPathComponent("game.icns")
            if (try? fm.attributesOfItem(atPath: target.path)) != nil { try fm.removeItem(at: target) }
            try fm.copyItem(at: icon, to: target)
        }
    }
}
