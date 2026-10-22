import AppKit
import UniformTypeIdentifiers

private func label(_ text: String, size: CGFloat, weight: NSFont.Weight = .regular) -> NSTextField {
    let view = NSTextField(wrappingLabelWithString: text)
    view.font = .systemFont(ofSize: size, weight: weight)
    view.alignment = .center
    return view
}

final class DropView: NSView {
    var receive: ((URL) -> Void)?
    var enabled = true
    private var hovering = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
    }
    required init?(coder: NSCoder) { fatalError("Not used") }

    private func file(from sender: NSDraggingInfo) -> URL? {
        guard enabled else { return nil }
        return (sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
            options: [.urlReadingFileURLsOnly: true]) as? [URL])?.first
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        hovering = file(from: sender) != nil
        needsDisplay = true
        return hovering ? .copy : []
    }
    override func draggingExited(_ sender: NSDraggingInfo?) {
        hovering = false
        needsDisplay = true
    }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        hovering = false
        needsDisplay = true
        guard let url = file(from: sender) else { return false }
        // Let the drag session finish before presenting the folder picker.
        DispatchQueue.main.async { self.receive?(url) }
        return true
    }
    override func draw(_ dirtyRect: NSRect) {
        let shape = NSBezierPath(roundedRect: bounds.insetBy(dx: 2, dy: 2), xRadius: 16, yRadius: 16)
        (hovering ? NSColor.controlAccentColor.withAlphaComponent(0.12) : NSColor.controlBackgroundColor).setFill()
        shape.fill()
        (hovering ? NSColor.controlAccentColor : NSColor.separatorColor).setStroke()
        shape.lineWidth = 1.5
        shape.setLineDash([6, 5], count: 2, phase: 0)
        shape.stroke()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private let drop = DropView()
    private let status = label("Drop \(FastFive.title) here", size: 20, weight: .semibold)
    private let detail = label("Drop the 32-bit Mac app (FastFive.app), a folder, a ZIP archive, or a disk image.", size: 13)
    private let footnote = label("Creates a new copy. Your original game stays untouched.", size: 11)
    private let progress = NSProgressIndicator()
    private let choose = NSButton(title: "Choose Game…", target: nil, action: #selector(chooseSource))
    private let secondary = NSButton(title: "Cancel", target: nil, action: #selector(secondaryAction))
    private let renderer = NSPopUpButton(frame: .zero, pullsDown: false)
    private let another = NSButton(title: "Convert Another", target: nil, action: #selector(convertAnother))
    private var converter: Converter?
    private var result: URL?
    private var choosing = false
    private var quitting = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        showWindow()
    }

    private func showWindow() {
        if window != nil {
            window.makeKeyAndOrderFront(nil)
            return
        }
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About Fast Five Converter", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide Fast Five Converter", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit Fast Five Converter", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let root = NSMenu()
        let menuItem = NSMenuItem()
        menuItem.submenu = appMenu
        root.addItem(menuItem)
        NSApp.mainMenu = root

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 580, height: 440),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "Fast Five Converter"
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()
        let content = window.contentView!
        let title = label("\(FastFive.title), on modern macOS.", size: 23, weight: .bold)
        let subtitle = label("Drop in your game. Get a version for modern macOS.", size: 13)
        subtitle.textColor = .secondaryLabelColor
        detail.textColor = .secondaryLabelColor
        footnote.textColor = .secondaryLabelColor
        progress.style = .bar
        progress.isIndeterminate = true
        progress.isHidden = true
        secondary.isHidden = true
        renderer.addItems(withTitles: ["Apple OpenGL (recommended)", "GLMetal (experimental)"])
        another.isHidden = true
        for button in [choose, secondary, another] {
            button.bezelStyle = .rounded
            button.target = self
        }
        choose.keyEquivalent = "\r"
        let buttons = NSStackView(views: [secondary, choose, another])
        let options = NSStackView(views: [label("Graphics:", size: 13), renderer])
        options.spacing = 12
        buttons.spacing = 8
        for view in [title, subtitle, options, drop, progress, buttons, footnote] {
            view.translatesAutoresizingMaskIntoConstraints = false
            content.addSubview(view)
        }
        for view in [status, detail] {
            view.translatesAutoresizingMaskIntoConstraints = false
            drop.addSubview(view)
        }
        NSLayoutConstraint.activate([
            title.topAnchor.constraint(equalTo: content.topAnchor, constant: 28),
            title.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            subtitle.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 8),
            subtitle.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            options.topAnchor.constraint(equalTo: subtitle.bottomAnchor, constant: 18),
            options.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            drop.topAnchor.constraint(equalTo: options.bottomAnchor, constant: 18),
            drop.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 28),
            drop.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -28),
            drop.heightAnchor.constraint(equalToConstant: 160),
            status.centerYAnchor.constraint(equalTo: drop.centerYAnchor, constant: -20),
            status.leadingAnchor.constraint(equalTo: drop.leadingAnchor, constant: 22),
            status.trailingAnchor.constraint(equalTo: drop.trailingAnchor, constant: -22),
            detail.topAnchor.constraint(equalTo: status.bottomAnchor, constant: 12),
            detail.leadingAnchor.constraint(equalTo: status.leadingAnchor),
            detail.trailingAnchor.constraint(equalTo: status.trailingAnchor),
            progress.topAnchor.constraint(equalTo: drop.bottomAnchor, constant: 10),
            progress.leadingAnchor.constraint(equalTo: drop.leadingAnchor, constant: 2),
            progress.trailingAnchor.constraint(equalTo: drop.trailingAnchor, constant: -2),
            buttons.topAnchor.constraint(equalTo: drop.bottomAnchor, constant: 28),
            buttons.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            footnote.topAnchor.constraint(equalTo: buttons.bottomAnchor, constant: 16),
            footnote.leadingAnchor.constraint(equalTo: drop.leadingAnchor),
            footnote.trailingAnchor.constraint(equalTo: drop.trailingAnchor)
        ])
        drop.receive = { [weak self] url in self?.selectDestination(for: url) }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc private func chooseSource() {
        guard converter == nil, !choosing else { return }
        if let result {
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.createsNewApplicationInstance = true
            NSWorkspace.shared.openApplication(at: result, configuration: configuration) { _, error in
                if let error { DispatchQueue.main.async { self.showError(error, title: "Couldn’t open \(FastFive.title)") } }
            }
            return
        }
        choosing = true
        let panel = NSOpenPanel()
        panel.message = "Choose the Mac \(FastFive.title) app, a folder, a ZIP archive, or a disk image."
        panel.prompt = "Choose Game"
        panel.allowedContentTypes = [.applicationBundle, .folder, .zip, .diskImage]
        panel.treatsFilePackagesAsDirectories = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.beginSheetModal(for: window) { response in
            self.choosing = false
            if response == .OK, let url = panel.url { self.selectDestination(for: url) }
        }
    }

    @objc private func convertAnother() {
        result = nil
        another.isHidden = true
        secondary.isHidden = true
        choose.title = "Choose Game…"
        status.stringValue = "Drop \(FastFive.title) here"
        detail.stringValue = "Drop the 32-bit Mac app (FastFive.app), a folder, a ZIP archive, or a disk image."
    }

    private func selectDestination(for source: URL) {
        guard converter == nil, !choosing else { NSSound.beep(); return }
        // Disk images are checked once mounted, during conversion.
        if !FastFiveSource.isArchive(source) {
            do { _ = try FastFiveSource.discover(source) } catch { showError(error); return }
        }
        choosing = true
        let panel = NSOpenPanel()
        panel.message = "Choose where to save your converted \(FastFive.title) app."
        panel.prompt = "Create App Here"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = FileManager.default.urls(for: .applicationDirectory, in: .userDomainMask).first
        panel.beginSheetModal(for: window) { response in
            self.choosing = false
            if response == .OK, let destination = panel.url { self.start(source, destination) }
        }
    }

    private func start(_ source: URL, _ destination: URL) {
        result = nil
        choose.title = "Choose Game…"
        choose.isEnabled = false
        renderer.isEnabled = false
        another.isHidden = true
        secondary.title = "Cancel"
        secondary.isEnabled = true
        secondary.isHidden = false
        drop.enabled = false
        progress.isHidden = false
        progress.startAnimation(nil)
        let worker = Converter(resources: Bundle.main.resourceURL!) { title, detail in
            DispatchQueue.main.async {
                guard self.secondary.isEnabled else { return }
                self.status.stringValue = title
                self.detail.stringValue = detail
            }
        }
        converter = worker
        let metal = renderer.indexOfSelectedItem == 1
        DispatchQueue.global(qos: .userInitiated).async {
            let outcome = Result { try worker.convertFastFive(source: source, destination: destination, useGLMetal: metal) }
            DispatchQueue.main.async { self.finish(outcome) }
        }
    }

    private func finish(_ outcome: Result<URL, Error>) {
        converter = nil
        progress.stopAnimation(nil)
        progress.isHidden = true
        choose.isEnabled = true
        renderer.isEnabled = true
        secondary.isEnabled = true
        secondary.isHidden = true
        drop.enabled = true
        switch outcome {
        case .success(let url):
            result = url
            another.isHidden = false
            status.stringValue = "Your \(FastFive.title) app is ready."
            detail.stringValue = url.path
            choose.title = "Open Game"
            secondary.title = "Show in Finder"
            secondary.isHidden = false
        case .failure(ConversionError.cancelled):
            status.stringValue = "Drop \(FastFive.title) here"
            detail.stringValue = "Conversion cancelled. You can try again whenever you're ready."
        case .failure(let error):
            status.stringValue = "Let’s try that again."
            detail.stringValue = "Drop \(FastFive.title) here, or choose it below."
            if !quitting { showError(error) }
        }
        if quitting { NSApp.reply(toApplicationShouldTerminate: true) }
    }

    @objc private func secondaryAction() {
        if let converter {
            status.stringValue = "Cancelling…"
            detail.stringValue = "Removing the unfinished copy."
            secondary.isEnabled = false
            converter.cancellation.cancel()
        } else if let result {
            NSWorkspace.shared.activateFileViewerSelecting([result])
        }
    }

    private func showError(_ error: Error, title: String = "Couldn’t finish converting \(FastFive.title)") {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = error.localizedDescription
        alert.addButton(withTitle: "OK")
        alert.beginSheetModal(for: window)
    }

    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        showWindow()
        if let filename = filenames.first { selectDestination(for: URL(fileURLWithPath: filename)) }
        sender.reply(toOpenOrPrint: .success)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showWindow()
        return true
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        NSApp.terminate(nil)
        return false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard converter != nil else { return .terminateNow }
        quitting = true
        secondaryAction()
        return .terminateLater
    }
}
