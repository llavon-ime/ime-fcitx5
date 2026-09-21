import Cocoa

// Candidate list drawn by the input method itself so paging, selection keys,
// layout and the marking-hint tooltip match the engine's render state.
//
// The engine hands over the whole candidate list plus the current page and page
// size (the fcitx5 addon paginates through fcitx::CommonCandidateList the same
// way); slicing the page is the frontend's job. A single bopomofo syllable can
// have 100+ homophones, so drawing every candidate made the window grow to one
// row per candidate. This panel shows at most
// `CandidatePageWindow.defaultMaxVisibleRows` rows and slides that window to
// follow the cursor when a page is bigger (the engine's Tab-expanded mode).
final class CandidatePanel {
    var onSelect: ((Int) -> Void)?

    fileprivate static let maxTextWidth: CGFloat = 460

    private let panel: NSPanel
    private let container = NSView()
    private let effect = NSVisualEffectView()
    private let stack = NSStackView()
    private let pageLabel = NSTextField(labelWithString: "")
    private var rows: [CandidateRowView] = []

    init() {
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 60, height: 24),
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

        // Vibrancy instead of a hand-painted background: it follows light and
        // dark mode (a frozen CGColor left a white box with white text in dark
        // mode) and matches the blurred candidate windows of other IMEs.
        effect.material = .popover
        effect.blendingMode = .behindWindow
        effect.state = .active
        effect.wantsLayer = true
        effect.layer?.cornerRadius = 8
        effect.layer?.masksToBounds = true
        effect.translatesAutoresizingMaskIntoConstraints = false
        panel.contentView = container
        container.addSubview(effect)

        stack.orientation = .vertical
        stack.alignment = .width
        stack.spacing = 1
        stack.edgeInsets = NSEdgeInsets(top: 4, left: 4, bottom: 4, right: 4)
        stack.translatesAutoresizingMaskIntoConstraints = false
        effect.addSubview(stack)
        NSLayoutConstraint.activate([
            effect.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            effect.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            effect.topAnchor.constraint(equalTo: container.topAnchor),
            effect.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            stack.leadingAnchor.constraint(equalTo: effect.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: effect.trailingAnchor),
            stack.topAnchor.constraint(equalTo: effect.topAnchor),
            stack.bottomAnchor.constraint(equalTo: effect.bottomAnchor),
        ])

        pageLabel.font = .systemFont(ofSize: 10)
        pageLabel.textColor = .tertiaryLabelColor
        pageLabel.alignment = .right
        pageLabel.isHidden = true
    }

    func update(snapshot: RenderSnapshot) {
        for row in rows {
            stack.removeArrangedSubview(row)
            row.removeFromSuperview()
        }
        rows.removeAll()

        let horizontal = snapshot.layoutHint == 2
        if horizontal {
            stack.orientation = .horizontal
            stack.alignment = .centerY
            stack.spacing = 8
            stack.edgeInsets = NSEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        } else {
            stack.orientation = .vertical
            stack.alignment = .width
            stack.spacing = 1
            stack.edgeInsets = NSEdgeInsets(top: 4, left: 4, bottom: 4, right: 4)
        }

        let pageSize = max(snapshot.pageSize, 1)
        let pageOffset = snapshot.page * pageSize
        let pageWindow = CandidatePageWindow.compute(candidateCount: snapshot.candidates.count,
                                                     page: snapshot.page,
                                                     pageSize: snapshot.pageSize,
                                                     cursor: snapshot.cursor)
        let selectable = snapshot.target == RenderTargetC.candidates
            || snapshot.target == RenderTargetC.symbolMenu

        for index in pageWindow.start..<pageWindow.end {
            let keyIndex = index - pageOffset
            let key: String
            if selectable, keyIndex < snapshot.selectionKeys.count,
               let scalar = UnicodeScalar(snapshot.selectionKeys[keyIndex]) {
                key = String(Character(scalar))
            } else {
                key = ""
            }
            let highlighted = snapshot.cursorVisible && index == pageOffset + snapshot.cursor
            let row = CandidateRowView(key: key,
                                       title: snapshot.candidates[index],
                                       highlighted: highlighted,
                                       horizontal: horizontal)
            row.onSelect = { [weak self] in self?.onSelect?(index) }
            rows.append(row)
            stack.addArrangedSubview(row)
        }

        let showPage = snapshot.pageCount > 1
        pageLabel.stringValue = showPage ? "\(snapshot.page + 1)/\(snapshot.pageCount)" : ""
        pageLabel.alignment = horizontal ? .left : .right
        pageLabel.isHidden = !showPage
        if showPage {
            stack.addArrangedSubview(pageLabel)
        }
    }

    func show(anchoredTo rect: NSRect, level: NSWindow.Level) {
        guard !rows.isEmpty else { return }
        stack.layoutSubtreeIfNeeded()
        let size = stack.fittingSize
        guard size.width > 0, size.height > 0 else { return }

        panel.level = level
        let screen = NSScreen.screens.first { $0.frame.intersects(rect) } ?? NSScreen.main
        var origin = NSPoint(x: rect.minX, y: rect.minY - size.height - 4)
        if let visible = screen?.visibleFrame {
            if origin.y < visible.minY {
                origin.y = rect.maxY + 4
            }
            origin.y = min(origin.y, visible.maxY - size.height - 4)
            origin.x = min(max(origin.x, visible.minX + 4), visible.maxX - size.width - 4)
        }

        let frame = NSRect(origin: origin, size: size)
        if panel.frame != frame {
            panel.setFrame(frame, display: true)
        }
        if !panel.isVisible {
            panel.orderFrontRegardless()
        }
    }

    func hide() {
        panel.orderOut(nil)
    }
}

// One clickable candidate row. The whole row is the hit target, so clicking the
// labels selects the candidate too.
private final class CandidateRowView: NSView {
    var onSelect: (() -> Void)?

    private let keyLabel = NSTextField(labelWithString: "")
    private let titleLabel = NSTextField(labelWithString: "")
    private var highlighted = false

    init(key: String, title: String, highlighted: Bool, horizontal: Bool) {
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false

        keyLabel.stringValue = key
        keyLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular)
        keyLabel.alignment = .right
        titleLabel.stringValue = title
        titleLabel.font = .systemFont(ofSize: 16)
        titleLabel.maximumNumberOfLines = 1
        titleLabel.lineBreakMode = .byTruncatingTail
        titleLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        for label in [keyLabel, titleLabel] {
            label.translatesAutoresizingMaskIntoConstraints = false
            addSubview(label)
        }

        let keyWidth: CGFloat = horizontal ? 12 : 14
        NSLayoutConstraint.activate([
            keyLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: horizontal ? 6 : 8),
            keyLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
            keyLabel.widthAnchor.constraint(equalToConstant: keyWidth),
            titleLabel.leadingAnchor.constraint(equalTo: keyLabel.trailingAnchor, constant: horizontal ? 4 : 6),
            titleLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            titleLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
            titleLabel.topAnchor.constraint(greaterThanOrEqualTo: topAnchor, constant: 3),
            bottomAnchor.constraint(greaterThanOrEqualTo: titleLabel.bottomAnchor, constant: 3),
            heightAnchor.constraint(greaterThanOrEqualToConstant: 24),
            titleLabel.widthAnchor.constraint(lessThanOrEqualToConstant: CandidatePanel.maxTextWidth),
        ])
        setHighlighted(highlighted)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    func setHighlighted(_ flag: Bool) {
        highlighted = flag
        titleLabel.textColor = flag ? .alternateSelectedControlTextColor : .labelColor
        keyLabel.textColor = flag ? .alternateSelectedControlTextColor : .secondaryLabelColor
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        guard highlighted else { return }
        NSColor.selectedContentBackgroundColor.setFill()
        NSBezierPath(roundedRect: bounds.insetBy(dx: 2, dy: 1), xRadius: 4, yRadius: 4).fill()
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        guard let parent = superview else { return nil }
        return bounds.contains(convert(point, from: parent)) ? self : nil
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
    }

    override func mouseDown(with event: NSEvent) {
        onSelect?()
    }
}
