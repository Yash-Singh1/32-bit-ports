import AppKit

let arguments = Array(CommandLine.arguments.dropFirst())

func fail(_ error: Error, status: Int32 = 1) -> Never {
    fputs("\(error.localizedDescription)\n", stderr)
    exit(status)
}

if arguments.first == "--inspect" {
    guard arguments.count == 2 else { fail(ConverterError.message("Usage: LEGOConverter --inspect GAME.app"), status: 2) }
    do {
        let inspection = try SourceInspection.read(URL(fileURLWithPath: arguments[1]))
        let result: [String: Any] = ["source": inspection.app.path,
            "game": inspection.detected?.rawValue ?? NSNull(), "reason": inspection.detectionReason,
            "executables": inspection.candidates.map { ["name": $0.0.lastPathComponent,
                "entry": String(format: "0x%08x", $0.1.entry), "end": String(format: "0x%08x", $0.1.end)] }]
        let json = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
        print(String(decoding: json, as: UTF8.self))
    } catch { fail(error) }
} else if arguments.first == "--convert" || arguments.first == "--rebuild-runtime" {
    let rebuildOnly = arguments.first == "--rebuild-runtime"
    let minimum = rebuildOnly ? 2 : 3
    guard arguments.count >= minimum else {
        fail(ConverterError.message("Usage: LEGOConverter --convert SOURCE.app OUTPUT_FOLDER [--game GAME] [--data DATA_FOLDER] [--rebuild] [--continue-when-inactive] [--log-dir LOG_FOLDER]\n       LEGOConverter --rebuild-runtime OUTPUT_FOLDER"), status: 2)
    }
    var game: LEGOGame?, data: URL?, logDirectory: URL?
    var rebuild = false, background = false, index = minimum
    while index < arguments.count {
        let option = arguments[index]
        if option == "--rebuild" { rebuild = true; index += 1; continue }
        if option == "--continue-when-inactive" { background = true; index += 1; continue }
        guard index + 1 < arguments.count else { fail(ConverterError.message("Missing value for \(option)."), status: 2) }
        let value = arguments[index + 1]
        switch option {
        case "--game":
            guard let selected = LEGOGame(rawValue: value) else {
                fail(ConverterError.message("Unknown game. Choose \(LEGOGame.allCases.map { $0.rawValue }.joined(separator: ", "))."), status: 2)
            }
            game = selected
        case "--data": data = URL(fileURLWithPath: value)
        case "--log-dir": logDirectory = URL(fileURLWithPath: value)
        default: fail(ConverterError.message("Unknown option: \(option)"), status: 2)
        }
        index += 2
    }
    do {
        let engine = try LEGOConverter(resources: Bundle.main.resourceURL!, logDirectory: logDirectory) { _, title, detail in
            fputs("\(title): \(detail)\n", stderr)
        }
        fputs("Conversion log: \(engine.log.path)\n", stderr)
        signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN)
        let signals = [SIGINT, SIGTERM].map { number -> DispatchSourceSignal in
            let source = DispatchSource.makeSignalSource(signal: number, queue: .global())
            source.setEventHandler { engine.cancellation.cancel() }
            source.resume(); return source
        }
        do {
            if rebuildOnly { print(try engine.rebuildRuntime(in: URL(fileURLWithPath: arguments[1])).path) }
            else {
                let result = try engine.convert(source: URL(fileURLWithPath: arguments[1]),
                    destination: URL(fileURLWithPath: arguments[2]), game: game, dataFolder: data,
                    continueWhenInactive: background, rebuild: rebuild)
                print(result.app.path)
            }
            withExtendedLifetime(signals) {}
        } catch { engine.recordFailure(error); withExtendedLifetime(signals) {}; fail(error) }
    } catch { fail(error) }
} else {
    let application = NSApplication.shared
    let delegate = AppDelegate()
    application.setActivationPolicy(.regular)
    application.delegate = delegate
    withExtendedLifetime(delegate) { application.run() }
}
