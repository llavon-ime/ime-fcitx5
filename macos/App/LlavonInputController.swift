import Cocoa
import InputMethodKit

// One controller per client session; every controller owns a ContextId and
// forwards keys, commits and renders through the shared EngineBridge.
@objc(LlavonInputController)
final class LlavonInputController: IMKInputController, EngineHost {
    private let contextId: UInt64
    private let candidatePanel: CandidatePanel
    private var lastSnapshot: RenderSnapshot?
    private var hasMarkedText = false

    override init!(server: IMKServer!, delegate: Any!, client inputClient: Any!) {
        contextId = EngineBridge.shared.allocateContext()
        candidatePanel = CandidatePanel()
        super.init(server: server, delegate: delegate, client: inputClient)
        candidatePanel.onSelect = { [weak self] index in
            self?.selectCandidate(index)
        }
        EngineBridge.shared.attach(contextId, host: self)
    }

    deinit {
        EngineBridge.shared.detach(contextId)
    }

    // MARK: - IMKInputController

    override func recognizedEvents(_ sender: Any!) -> Int {
        let events: NSEvent.EventTypeMask = [.keyDown, .keyUp]
        return Int(events.rawValue)
    }

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard let event else { return false }
        switch event.type {
        case .keyDown, .keyUp:
            return EngineBridge.shared.sendKey(contextId,
                                               keyCode: event.keyCode,
                                               charactersIgnoringModifiers: event.charactersIgnoringModifiers,
                                               modifiers: Keysym.modifiers(for: event),
                                               capsLock: event.modifierFlags.contains(.capsLock),
                                               isRelease: event.type == .keyUp)
        default:
            return false
        }
    }

    override func activateServer(_ sender: Any!) {
        EngineBridge.shared.activate(contextId)
        super.activateServer(sender)
    }

    override func deactivateServer(_ sender: Any!) {
        EngineBridge.shared.deactivate(contextId)
        candidatePanel.hide()
        hasMarkedText = false
        super.deactivateServer(sender)
    }

    override func commitComposition(_ sender: Any!) {
        EngineBridge.shared.finalize(contextId)
    }

    // MARK: - Host callbacks

    func insertCommit(_ text: String) {
        guard let textClient = client() else { return }
        textClient.insertText(text, replacementRange: NSRange(location: NSNotFound, length: 0))
        hasMarkedText = false
    }

    func refreshUI() {
        guard let textClient = client(),
              let snapshot = EngineBridge.shared.snapshot(contextId) else { return }
        lastSnapshot = snapshot

        if snapshot.compositionEmpty && snapshot.preedit.isEmpty {
            if hasMarkedText {
                textClient.setMarkedText("",
                                         selectionRange: NSRange(location: 0, length: 0),
                                         replacementRange: NSRange(location: NSNotFound, length: 0))
                hasMarkedText = false
            }
        } else {
            let marked = NSMutableAttributedString()
            for segment in snapshot.preedit {
                var attributes: [NSAttributedString.Key: Any] = [:]
                if segment.underlined {
                    attributes[.underlineStyle] = NSUnderlineStyle.single.rawValue
                }
                marked.append(NSAttributedString(string: segment.text, attributes: attributes))
            }
            let caret = min(max(snapshot.caret, 0), marked.length)
            textClient.setMarkedText(marked,
                                     selectionRange: NSRange(location: caret, length: 0),
                                     replacementRange: NSRange(location: NSNotFound, length: 0))
            hasMarkedText = true
        }

        if snapshot.hasCandidates {
            let level = NSWindow.Level(rawValue: Int(textClient.windowLevel()) + 1)
            candidatePanel.update(snapshot: snapshot)
            candidatePanel.show(anchoredTo: anchorRect(client: textClient), level: level)
        } else {
            candidatePanel.hide()
        }
    }

    // Text before the caret, as UTF-16 units (the engine clips it further).
    func surroundingSample() -> (text: [UInt16], cursor: Int, anchor: Int)? {
        guard let textClient = client() else { return nil }
        let selected = textClient.selectedRange()
        guard selected.location != NSNotFound else { return nil }

        let limit = 512
        let start = max(0, selected.location - limit)
        let range = NSRange(location: start, length: selected.location - start)
        guard let attribute = textClient.attributedSubstring(from: range) else { return nil }
        let units = Array(attribute.string.utf16)
        return (units, units.count, units.count)
    }

    // MARK: - Helpers

    private func selectCandidate(_ index: Int) {
        guard let snapshot = lastSnapshot else { return }
        if snapshot.target == RenderTargetC.symbolMenu {
            EngineBridge.shared.selectSymbol(contextId, index: Int32(index), epoch: snapshot.symbolEpoch)
        } else if snapshot.target == RenderTargetC.candidates {
            EngineBridge.shared.selectCandidate(contextId, index: Int32(index))
        }
    }

    private func anchorRect(client: IMKTextInput) -> NSRect {
        var rect = NSRect.zero
        _ = client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
        return rect
    }
}
