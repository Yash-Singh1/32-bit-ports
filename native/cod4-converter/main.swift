import AppKit

// Exercise exactly the GUI's conversion code without application windows.
if CommandLine.arguments.dropFirst().first == "--convert" {
    let args = CommandLine.arguments
    guard args.count >= 4 else {
        fputs("Usage: \(GameTitle.current.short)Converter --convert SOURCE_APP_OR_FOLDER OUTPUT_FOLDER [--mode sp|mp] [--cache CACHE_FOLDER]\n", stderr)
        exit(2)
    }
    var mode = GameMode.sp
    var cache = Converter.defaultCache
    var index = 4
    while index < args.count {
        guard index + 1 < args.count else { fputs("Missing option value.\n", stderr); exit(2) }
        switch args[index] {
        case "--mode":
            guard let value = GameMode(rawValue: args[index + 1]) else { fputs("Mode must be sp or mp.\n", stderr); exit(2) }
            mode = value
        case "--cache": cache = URL(fileURLWithPath: args[index + 1])
        default: fputs("Unknown option: \(args[index])\n", stderr); exit(2)
        }
        index += 2
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
        let output = try converter.convert(source: URL(fileURLWithPath: args[2]), destination: URL(fileURLWithPath: args[3]), mode: mode)
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
