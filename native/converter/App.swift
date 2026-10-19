import AppKit
import UniformTypeIdentifiers

private func label(_ text: String, size: CGFloat = 12, weight: NSFont.Weight = .regular) -> NSTextField {
    let field = NSTextField(wrappingLabelWithString: text)
    field.font = .systemFont(ofSize: size, weight: weight)
    return field
}

final class GameDropView: NSView {
    var receive: ((URL) -> Void)?
    var enabled = true
    private var hovering = false
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
    }
    required init?(coder: NSCoder) { fatalError("Not used") }
    private func file(_ sender: NSDraggingInfo) -> URL? {
        guard enabled,
              let files = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                options: [.urlReadingFileURLsOnly: true]) as? [URL], files.count == 1 else { return nil }
        return files.first
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        hovering = file(sender) != nil; needsDisplay = true
        return hovering ? .copy : []
    }
    override func draggingExited(_ sender: NSDraggingInfo?) { hovering = false; needsDisplay = true }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        hovering = false; needsDisplay = true
        guard let url = file(sender) else { return false }
        DispatchQueue.main.async { self.receive?(url) }
        return true
    }
    override func draw(_ dirtyRect: NSRect) {
        let path = NSBezierPath(roundedRect: bounds.insetBy(dx: 1, dy: 1), xRadius: 12, yRadius: 12)
        (hovering ? NSColor.controlAccentColor.withAlphaComponent(0.08) : NSColor.controlBackgroundColor).setFill()
        path.fill()
        (hovering ? NSColor.controlAccentColor : NSColor.separatorColor).setStroke()
        path.lineWidth = 1.5; path.setLineDash([5, 4], count: 2, phase: 0); path.stroke()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private let drop = GameDropView()
    private let icon = NSImageView()
    private let sourceTitle = label("Drop a LEGO app or Steam game folder", size: 17, weight: .semibold)
    private let sourceDetail = label("Your original game stays unchanged.", size: 11)
    private let choose = NSButton(title: "Choose…", target: nil, action: #selector(chooseSource))
    private let gameMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let outputField = label("", size: 11)
    private let changeOutput = NSButton(title: "Change…", target: nil, action: #selector(chooseDestination))
    private let dataField = label("Detected automatically", size: 11)
    private let dataButton = NSButton(title: "Choose…", target: nil, action: #selector(chooseData))
    private var dataRow: NSStackView!
    private let status = label("", size: 11)
    private let progress = NSProgressIndicator()
    private let convert = NSButton(title: "Convert", target: nil, action: #selector(convertAction))
    private let reveal = NSButton(title: "Show in Finder", target: nil, action: #selector(revealResult))
    private var sourceURL: URL?
    private var inspection: SourceInspection?
    private var destination = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications")
    private var dataFolder: URL?
    private var engine: LEGOConverter?
    private var result: URL?
    private var quitting = false
    private var closing = false
    private var report: URL?
    private var selectedGame: LEGOGame? {
        guard let value = gameMenu.selectedItem?.representedObject as? String else { return inspection?.detected }
        return LEGOGame(rawValue: value)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        makeWindow()
        if let index = CommandLine.arguments.firstIndex(of: "--open"), CommandLine.arguments.count > index + 1 {
            receive(URL(fileURLWithPath: CommandLine.arguments[index + 1]))
        }
    }
    private func makeWindow() {
        guard window == nil else { window.makeKeyAndOrderFront(nil); return }
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 570, height: 440),
            styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "LEGO Converter"; window.isReleasedWhenClosed = false; window.delegate = self
        let content = window.contentView!
        icon.image = NSImage(systemSymbolName: "tray.and.arrow.down", accessibilityDescription: nil)
        icon.contentTintColor = .secondaryLabelColor
        icon.imageScaling = .scaleProportionallyUpOrDown
        sourceTitle.alignment = .center; sourceDetail.alignment = .center
        sourceTitle.maximumNumberOfLines = 2
        sourceDetail.maximumNumberOfLines = 1; sourceDetail.lineBreakMode = .byTruncatingMiddle
        sourceDetail.textColor = .secondaryLabelColor
        for button in [choose, changeOutput, dataButton, convert, reveal] {
            button.bezelStyle = .rounded; button.target = self
        }
        convert.keyEquivalent = "\r"; convert.isEnabled = false
        convert.bezelColor = .controlAccentColor
        reveal.isHidden = true
        gameMenu.target = self; gameMenu.action = #selector(gameChanged)
        gameMenu.addItem(withTitle: "Detect automatically")
        gameMenu.menu?.addItem(.separator())
        for game in LEGOGame.allCases {
            gameMenu.addItem(withTitle: game.menuTitle)
            gameMenu.lastItem?.representedObject = game.rawValue
        }
        gameMenu.toolTip = "If the detected title is wrong, select the correct game here."
        outputField.stringValue = shortPath(destination)
        outputField.lineBreakMode = .byTruncatingMiddle
        outputField.maximumNumberOfLines = 1
        status.textColor = .secondaryLabelColor
        progress.style = .bar; progress.minValue = 0; progress.maxValue = 1
        progress.isIndeterminate = false; progress.isHidden = true
        let dropContents = NSStackView(views: [icon, sourceTitle, sourceDetail, choose])
        dropContents.orientation = .vertical; dropContents.alignment = .centerX; dropContents.spacing = 9
        dropContents.translatesAutoresizingMaskIntoConstraints = false
        drop.addSubview(dropContents)
        let gameRow = row("Game:", [gameMenu])
        let outputRow = row("Save to:", [outputField, changeOutput])
        dataRow = row("Data:", [dataField, dataButton]); dataRow.isHidden = true
        let buttons = NSStackView(views: [reveal, convert]); buttons.spacing = 8
        let footer = NSView()
        buttons.translatesAutoresizingMaskIntoConstraints = false; footer.addSubview(buttons)
        let stack = NSStackView(views: [drop, gameRow, outputRow, dataRow, status, progress, footer])
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 15
        stack.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(stack)
        for view: NSView in [drop, gameRow, outputRow, dataRow, status, progress, footer] {
            view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 24),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -24),
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 24),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor, constant: -20),
            drop.heightAnchor.constraint(equalToConstant: 175),
            icon.widthAnchor.constraint(equalToConstant: 36), icon.heightAnchor.constraint(equalToConstant: 36),
            dropContents.centerXAnchor.constraint(equalTo: drop.centerXAnchor),
            dropContents.centerYAnchor.constraint(equalTo: drop.centerYAnchor),
            dropContents.leadingAnchor.constraint(greaterThanOrEqualTo: drop.leadingAnchor, constant: 15),
            dropContents.trailingAnchor.constraint(lessThanOrEqualTo: drop.trailingAnchor, constant: -15),
            footer.heightAnchor.constraint(equalToConstant: 32),
            buttons.trailingAnchor.constraint(equalTo: footer.trailingAnchor),
            buttons.centerYAnchor.constraint(equalTo: footer.centerYAnchor)
        ])
        outputField.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        gameMenu.setContentHuggingPriority(.defaultLow, for: .horizontal)
        gameMenu.widthAnchor.constraint(greaterThanOrEqualToConstant: 420).isActive = true
        drop.receive = { [weak self] url in self?.receive(url) }
        makeMenu(); window.center(); window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    private func row(_ title: String, _ controls: [NSView]) -> NSStackView {
        let title = label(title)
        title.alignment = .right; title.widthAnchor.constraint(equalToConstant: 48).isActive = true
        let row = NSStackView(views: [title] + controls)
        row.orientation = .horizontal; row.alignment = .centerY; row.spacing = 9
        return row
    }
    private func receive(_ url: URL) {
        guard engine == nil else { NSSound.beep(); return }
        if window == nil { makeWindow() }
        do {
            let info = try SourceInspection.read(url)
            sourceURL = url; inspection = info; result = nil; dataFolder = nil; report = nil
            sourceTitle.stringValue = info.app.deletingPathExtension().lastPathComponent
            sourceDetail.stringValue = shortPath(url)
            icon.image = NSWorkspace.shared.icon(forFile: info.app.path)
            choose.title = "Choose another…"
            gameMenu.item(at: 0)?.title = info.detected.map { $0.menuTitle + " · Auto" } ?? "Detect automatically"
            gameMenu.selectItem(at: 0)
            status.stringValue = info.detected == nil ? "Choose the game manually from the menu." : ""
            convert.title = "Convert"; reveal.isHidden = true
            updateSelection()
        } catch { showError(error) }
    }
    @objc private func gameChanged() {
        result = nil; convert.title = "Convert"; reveal.isHidden = true
        updateSelection()
    }
    private func updateSelection() {
        convert.isEnabled = inspection != nil && selectedGame != nil
        let saga = selectedGame == .sagaSteam || selectedGame == .sagaRetail
        dataRow.isHidden = !saga
        dataField.stringValue = dataFolder.map { shortPath($0) } ?? "Detected automatically"
        if selectedGame?.experimental == true { status.stringValue = "Experimental support for this game." }
        else if inspection != nil { status.stringValue = selectedGame == nil ? "Choose the game manually from the menu." : "" }
    }
    @objc private func chooseSource() {
        guard engine == nil else { return }
        let panel = NSOpenPanel()
        panel.title = "Choose a LEGO app or Steam game folder"
        panel.message = "Select the original .app, or its game folder inside steamapps/common."
        panel.prompt = "Choose"
        panel.allowedContentTypes = [.applicationBundle, .folder]
        panel.canChooseFiles = true; panel.canChooseDirectories = true
        panel.treatsFilePackagesAsDirectories = false; panel.allowsMultipleSelection = false
        panel.beginSheetModal(for: window) { [weak self] response in
            if response == .OK, let url = panel.url { self?.receive(url) }
        }
    }
    @objc private func chooseDestination() {
        let panel = NSOpenPanel(); panel.title = "Save converted app to"
        panel.prompt = "Save here"; panel.canChooseFiles = false; panel.canChooseDirectories = true
        panel.canCreateDirectories = true; panel.allowsMultipleSelection = false; panel.directoryURL = destination
        panel.beginSheetModal(for: window) { response in
            if response == .OK, let url = panel.url { self.destination = url; self.outputField.stringValue = self.shortPath(url) }
        }
    }
    @objc private func chooseData() {
        let panel = NSOpenPanel(); panel.title = "Choose Complete Saga's data folder"
        panel.prompt = "Choose"; panel.canChooseFiles = false; panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.beginSheetModal(for: window) { response in
            if response == .OK, let url = panel.url { self.dataFolder = url; self.updateSelection() }
        }
    }
    @objc private func convertAction() {
        if let engine = engine { status.stringValue = "Cancelling…"; engine.cancellation.cancel(); return }
        if let result = result { NSWorkspace.shared.open(result); return }
        guard let source = sourceURL, let game = selectedGame else { return }
        do {
            let worker = try LEGOConverter(resources: Bundle.main.resourceURL!) { [weak self] value, title, detail in
                DispatchQueue.main.async {
                    self?.progress.doubleValue = value
                    self?.status.stringValue = title + (detail.isEmpty ? "" : " · " + detail)
                }
            }
            engine = worker; report = worker.log
            setRunning(true)
            let destination = self.destination, data = dataFolder
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                do {
                    let output = try worker.convert(source: source, destination: destination, game: game, dataFolder: data)
                    DispatchQueue.main.async { self?.finished(output: output, error: nil) }
                } catch {
                    worker.recordFailure(error)
                    DispatchQueue.main.async { self?.finished(output: nil, error: error) }
                }
            }
        } catch { showError(error) }
    }
    private func setRunning(_ running: Bool) {
        choose.isEnabled = !running; gameMenu.isEnabled = !running
        changeOutput.isEnabled = !running; dataButton.isEnabled = !running; drop.enabled = !running
        progress.isHidden = !running; convert.title = running ? "Cancel" : "Convert"
        if running { status.stringValue = "Checking the game…"; progress.doubleValue = 0 }
    }
    private func finished(output: ConversionResult?, error: Error?) {
        engine = nil; setRunning(false)
        if let output = output {
            result = output.app; convert.title = "Open app"; reveal.isHidden = false
            status.stringValue = output.hasSeparateData ? "Done. Keep the app and its data folder together." : "Done. Your converted app is ready."
        } else if let error = error {
            status.stringValue = "Conversion did not finish."
            if case ConverterError.cancelled = error { status.stringValue = "Cancelled." }
            else if !quitting && !closing { showError(error) }
        }
        if quitting { NSApp.reply(toApplicationShouldTerminate: true) }
        else if closing { window.close(); closing = false }
    }
    @objc private func revealResult() {
        if let result = result { NSWorkspace.shared.activateFileViewerSelecting([result]) }
    }
    private func showError(_ error: Error) {
        let alert = NSAlert()
        alert.messageText = "Couldn't convert the game"
        alert.informativeText = error.localizedDescription
        alert.addButton(withTitle: "OK")
        if report != nil { alert.addButton(withTitle: "Save log…") }
        alert.beginSheetModal(for: window) { response in
            if response == .alertSecondButtonReturn { self.saveLog() }
        }
    }
    private func saveLog() {
        guard let report = report else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = "LEGO-conversion.log"
        panel.beginSheetModal(for: window) { response in
            guard response == .OK, let url = panel.url else { return }
            do { try Data(contentsOf: report).write(to: url, options: .atomic) }
            catch { self.showError(error) }
        }
    }
    private func shortPath(_ url: URL) -> String {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        return url.path.hasPrefix(home + "/") ? "~" + url.path.dropFirst(home.count) : url.path
    }
    func application(_ application: NSApplication, open urls: [URL]) {
        if let url = urls.first { receive(url); window.makeKeyAndOrderFront(nil) }
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil); return true
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let engine = engine else { return .terminateNow }
        quitting = true; engine.cancellation.cancel(); return .terminateLater
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard let engine = engine else { return true }
        closing = true; engine.cancellation.cancel(); return false
    }
    private func makeMenu() {
        let root = NSMenu(), app = NSMenu(), file = NSMenu()
        app.addItem(withTitle: "About LEGO Converter", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        app.addItem(.separator())
        app.addItem(withTitle: "Hide LEGO Converter", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        app.addItem(withTitle: "Quit LEGO Converter", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let appItem = NSMenuItem(); appItem.submenu = app; root.addItem(appItem)
        let fileItem = NSMenuItem(title: "File", action: nil, keyEquivalent: ""); fileItem.submenu = file
        let choose = NSMenuItem(title: "Choose app or folder…", action: #selector(chooseSource), keyEquivalent: "o")
        choose.target = self; file.addItem(choose); root.addItem(fileItem)
        NSApp.mainMenu = root
    }
}
