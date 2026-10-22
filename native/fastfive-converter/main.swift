import AppKit

// Exercise exactly the GUI's conversion code without application windows.
if CommandLine.arguments.dropFirst().first == "--convert" {
    let args = CommandLine.arguments
    guard args.count >= 4 else {
        fputs("Usage: FastFiveConverter --convert SOURCE_APP_FOLDER_ZIP_OR_DMG OUTPUT_FOLDER [--metal | --apple-gl] [--cache CACHE_FOLDER]\n", stderr)
        exit(2)
    }
    var cache = Converter.defaultCache
    var metal = false
    var index = 4
    while index < args.count {
        switch args[index] {
        case "--metal":
            metal = true
            index += 1
        case "--apple-gl":
            metal = false
            index += 1
        case "--cache":
            guard index + 1 < args.count else { fputs("Missing option value.\n", stderr); exit(2) }
            cache = URL(fileURLWithPath: args[index + 1])
            index += 2
        default:
            fputs("Unknown option: \(args[index])\n", stderr)
            exit(2)
        }
    }
    let converter = Converter(resources: Bundle.main.resourceURL!, cache: cache) { title, detail in
        fputs("\(title) \(detail)\n", stderr)
    }
    signal(SIGINT, SIG_IGN)
    signal(SIGTERM, SIG_IGN)
    let signals = [SIGINT, SIGTERM].map { number -> DispatchSourceSignal in
        let source = DispatchSource.makeSignalSource(signal: number, queue: .global())
        source.setEventHandler { converter.cancellation.cancel() }
        source.resume()
        return source
    }
    do {
        let output = try converter.convertFastFive(source: URL(fileURLWithPath: args[2]),
                                                   destination: URL(fileURLWithPath: args[3]), useGLMetal: metal)
        print(output.path)
        withExtendedLifetime(signals) {}
    } catch {
        fputs("\(error.localizedDescription)\n", stderr)
        exit(1)
    }
} else {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.setActivationPolicy(.regular)
    app.delegate = delegate
    withExtendedLifetime(delegate) { app.run() }
}
