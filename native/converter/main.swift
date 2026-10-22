import AppKit

// A headless entry point exercises exactly the shipped conversion code during
// development. Normal Finder launches go straight to the small drop window.
if CommandLine.arguments.count > 1 && CommandLine.arguments[1] == "--convert" {
    let args = CommandLine.arguments
    guard args.count == 4 || args.count == 6 && args[4] == "--cache" else {
        fputs("Usage: \(ConverterGame.stem)Converter --convert SOURCE_APP_OR_FOLDER OUTPUT_FOLDER [--cache CACHE_FOLDER]\n", stderr)
        exit(2)
    }
    let cache = args.count == 6 ? URL(fileURLWithPath: args[5]) : Converter.defaultCache
    let converter = Converter(resources: Bundle.main.resourceURL!, cache: cache) { title, detail in
        fputs("\(title) \(detail)\n", stderr)
    }
    // Keep a headless Ctrl-C on the same cleanup path as the GUI's Cancel button.
    signal(SIGINT, SIG_IGN)
    signal(SIGTERM, SIG_IGN)
    let signals = [SIGINT, SIGTERM].map { number -> DispatchSourceSignal in
        let source = DispatchSource.makeSignalSource(signal: number, queue: .global())
        source.setEventHandler { converter.cancellation.cancel() }
        source.resume()
        return source
    }
    do {
        let output = try converter.convert(source: URL(fileURLWithPath: args[2]), destination: URL(fileURLWithPath: args[3]))
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
    // NSApplication does not retain its delegate.
    withExtendedLifetime(delegate) { app.run() }
}
