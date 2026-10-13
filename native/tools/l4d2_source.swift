import Foundation

@main
struct L4D2SourceTool {
    static func main() {
        do {
            let args = CommandLine.arguments
            guard args.count == 3 || args.count == 4 else {
                throw ConversionError.message("Usage: l4d2_source --image SOURCE | --describe SOURCE | --copy SOURCE OUTPUT_APP | --check-output OUTPUT_APP")
            }
            if args[1] == "--check-output" && args.count == 3 {
                try L4D2Source.validateBundleOutput(URL(fileURLWithPath: args[2]).standardizedFileURL)
                return
            }
            let source = URL(fileURLWithPath: args[2]).standardizedFileURL
            let layout = try L4D2Source.discover(source)
            if args[1] == "--image" && args.count == 3 { print(layout.image.path); return }
            if args[1] == "--describe" && args.count == 3 {
                let data = try JSONSerialization.data(withJSONObject: ["roots": [layout.root.path],
                    "image": layout.image.path, "executable": layout.image.lastPathComponent])
                print(String(decoding: data, as: UTF8.self)); return
            }
            guard args[1] == "--copy" && args.count == 4 else {
                throw ConversionError.message("Expected --image, --describe, --copy or --check-output.")
            }
            let app = URL(fileURLWithPath: args[3]).standardizedFileURL
            guard !isInside(app, source) else { throw ConversionError.message("Output must be outside the source.") }
            try layout.prepareBundle(at: app)
            print("Prepared Left 4 Dead 2 from: " + layout.root.path)
        } catch {
            fputs("\(error.localizedDescription)\n", stderr)
            exit(1)
        }
    }
}
