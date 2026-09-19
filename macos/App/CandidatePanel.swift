import Cocoa

// Candidate list drawn by the input method itself so paging, selection keys,
// layout and the marking-hint tooltip match the engine's render state.
final class CandidatePanel {
    var onSelect: ((Int) -> Void)?

    private let panel: NSPanel
    private let stack = NSStackView()

    init() {
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 40, height: 24),
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered,
                        defer: false)
        panel.isFloatingPanel = true
        panel.becomesKeyOnlyIfNeeded = true
        panel.worksWhenModal = true
        panel.hidesOnDeactivate = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.contentView = stack

        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 2
        stack.edgeInsets = NSEdgeInsets(top: 4, left: 6, bottom: 4, right: 6)
        stack.wantsLayer = true
        stack.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        stack.layer?.cornerRadius = 6
        stack.layer?.borderWidth = 0.5
        stack.layer?.borderColor = NSColor.separatorColor.cgColor
    }

    func update(snapshot: RenderSnapshot) {
        for view in stack.arrangedSubviews {
            stack.removeArrangedSubview(view)
            view.removeFromSuperview()
        }
        stack.orientation = snapshot.layoutHint == 2 ? .horizontal : .vertical

        let pageOffset = snapshot.page * max(snapshot.pageSize, 1)
        for (row, candidate) in snapshot.candidates.enumerated() {
            let scalar = row < snapshot.selectionKeys.count
                ? UnicodeScalar(snapshot.selectionKeys[row])
                : nil
            let key = scalar.map { String(Character($0)) } ?? ""
            let title = key.isEmpty ? candidate : "\(key) \(candidate)"
            let button = NSButton(title: title, target: self, action: #selector(clicked(_:)))
            button.tag = pageOffset + row
            button.isBordered = false
            button.bezelStyle = .inline
            button.font = .systemFont(ofSize: 16)
            if snapshot.cursorVisible && row == snapshot.cursor {
                button.contentTintColor = .controlAccentColor
            }
            stack.addArrangedSubview(button)
        }
    }

    @objc private func clicked(_ sender: NSButton) {
        onSelect?(sender.tag)
    }

    func show(anchoredTo rect: NSRect, level: NSWindow.Level) {
        guard !stack.arrangedSubviews.isEmpty else { return }
        stack.layoutSubtreeIfNeeded()
        let size = stack.fittingSize
        guard size.width > 0, size.height > 0 else { return }

        panel.level = level
        let screen = NSScreen.screens.first { $0.frame.intersects(rect) } ?? NSScreen.main
        var origin = NSPoint(x: rect.minX, y: rect.minY - size.height - 4)
        if let visible = screen?.visibleFrame, origin.y < visible.minY {
            origin.y = rect.maxY + 4
        }
        panel.setFrame(NSRect(origin: origin, size: size), display: true)
        panel.orderFrontRegardless()
    }

    func hide() {
        panel.orderOut(nil)
    }
}
