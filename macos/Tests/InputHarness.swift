import Cocoa
import InputMethodKit

// Raw-input harness for the native macOS frontend: constructed NSEvents go
// through the real LlavonInputController (its event handling, key translation
// and client effects) -> EngineBridge -> the C ABI -> the engine, and the
// harness observes what a text client would see (marked text, commits) plus
// whether the controller consumed the key. This is the macOS counterpart of the
// fcitx5 test harness, which drives the addon through fcitx5's TestFrontend.
//
// IMKInputController only accepts real IMK client proxies, so the controller is
// built through its test seam (TextClient) instead of a live IMK session.
// AppKit reports the base keysym for some punctuation in real events, so the
// punctuation chords are exercised with both keysym shapes.

// Text client double: keeps a small document model and records the calls the
// input method makes. The marked text lives in the document at the caret, the
// way an IMK client stores it, so surrounding-text sampling can exclude it.
final class RecordingClient: NSObject, TextClient {
    private(set) var document = ""
    private(set) var marked = ""
    private(set) var commits: [String] = []
    private var markedCaret = 0

    func insertText(_ text: String, replacementRange: NSRange) {
        commits.append(text)
        document += text
        marked = ""
        markedCaret = 0
    }

    func setMarkedText(_ text: NSAttributedString, selectionRange: NSRange, replacementRange: NSRange) {
        marked = text.string
        markedCaret = min(selectionRange.location, (marked as NSString).length)
    }

    func selectedRange() -> NSRange {
        NSRange(location: (document as NSString).length + markedCaret, length: 0)
    }

    func markedRange() -> NSRange {
        guard !marked.isEmpty else { return NSRange(location: NSNotFound, length: 0) }
        return NSRange(location: (document as NSString).length, length: (marked as NSString).length)
    }

    func attributedSubstring(from range: NSRange) -> NSAttributedString? {
        let text = (document + marked) as NSString
        guard range.location != NSNotFound, NSMaxRange(range) <= text.length else { return nil }
        return NSAttributedString(string: text.substring(with: range))
    }

    func attributes(forCharacterIndex index: Int,
                    lineHeightRectangle: UnsafeMutablePointer<NSRect>?) -> [AnyHashable: Any]? {
        lineHeightRectangle?.pointee = NSRect(x: 0, y: 0, width: 2, height: 18)
        return nil
    }

    func windowLevel() -> Int32 { 0 }
}

@main
struct InputHarness {
    static var failures = 0

    static func expect(_ condition: Bool, _ label: String, _ detail: String = "") {
        if condition {
            print("[ok] \(label)\(detail.isEmpty ? "" : ": \(detail)")")
        } else {
            failures += 1
            print("[FAIL] \(label)\(detail.isEmpty ? "" : ": \(detail)")")
        }
    }

    @discardableResult
    static func key(_ controller: LlavonInputController, _ client: RecordingClient,
                    _ label: String, keyCode: UInt16, characters: String,
                    ignoringModifiers: String? = nil,
                    shift: Bool = false, capsLock: Bool = false, control: Bool = false) -> Bool {
        var flags: NSEvent.ModifierFlags = []
        if shift { flags.insert(.shift) }
        if capsLock { flags.insert(.capsLock) }
        if control { flags.insert(.control) }
        guard let event = NSEvent.keyEvent(with: .keyDown,
                                           location: .zero,
                                           modifierFlags: flags,
                                           timestamp: 0,
                                           windowNumber: 0,
                                           context: nil,
                                           characters: characters,
                                           charactersIgnoringModifiers: ignoringModifiers ?? characters,
                                           isARepeat: false,
                                           keyCode: keyCode) else {
            fatalError("cannot build the key event for \(label)")
        }
        return controller.handle(event, client: client)
    }

    static func main() {
        guard let tablePath = ProcessInfo.processInfo.environment["LLAVON_IME_TABLE_PATH"],
              !tablePath.isEmpty else {
            FileHandle.standardError.write("set LLAVON_IME_TABLE_PATH\n".data(using: .utf8)!)
            exit(2)
        }

        // IMK and NSPanel want an application instance; never take focus.
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)

        // Deterministic settings: the harness must not read the user's config.
        let config = #"{"caps_lock_inputs_bopomofo":true,"shift_letter_keys":"directly_output_uppercase","smart_english":false}"#
        EngineBridge.shared.start(options: EngineStartOptions(tablePath: tablePath,
                                                              phraseOverridesPath: nil,
                                                              servicePath: nil,
                                                              modelPath: nil,
                                                              tablesDir: nil,
                                                              configJson: config,
                                                              autoStartService: false,
                                                              enableAccessibility: false))

        let client = RecordingClient()
        let controller = LlavonInputController(testClient: client)

        // Bopomofo composition reaches the client as marked text.
        for (label, keyCode, characters) in [("s", UInt16(0x01), "s"), ("u", UInt16(0x20), "u"), ("3", UInt16(0x14), "3")] {
            let consumed = key(controller, client, label, keyCode: keyCode, characters: characters)
            expect(consumed, "type \(label) is consumed")
        }
        expect(client.marked == "你", "su3 marks 你", client.marked)

        // Return commits the marked text.
        key(controller, client, "Return", keyCode: 0x24, characters: "\r")
        expect(client.commits == ["你"], "Return commits 你", "\(client.commits)")
        expect(client.marked.isEmpty, "marked text is cleared after commit")

        // Shift+? must become the fullwidth question mark, whichever keysym
        // AppKit reports for the punctuation key.
        for (label, characters) in [("Shift+? (shifted keysym)", "?"), ("Shift+? (base keysym)", "/")] {
            key(controller, client, label, keyCode: 0x2c, characters: characters, shift: true)
            expect(client.marked == "？", "\(label) marks ？", client.marked)
            key(controller, client, "Return", keyCode: 0x24, characters: "\r")
            expect(client.commits.last == "？", "\(label) commits ？", "\(client.commits)")
        }

        // Shift+letter commits the composition together with the uppercase
        // letter (DirectlyOutputUppercase), and CapsLock keeps bopomofo input.
        for (label, keyCode, characters) in [("s", UInt16(0x01), "s"), ("u", UInt16(0x20), "u"), ("3", UInt16(0x14), "3")] {
            key(controller, client, label, keyCode: keyCode, characters: characters)
        }
        key(controller, client, "Shift+A", keyCode: 0x00, characters: "A", shift: true)
        expect(client.commits.last == "你A", "Shift+A commits the composition plus A", "\(client.commits)")
        expect(client.commits.count == 4, "every commit happened exactly once", "\(client.commits.count)")

        key(controller, client, "Caps+A", keyCode: 0x00, characters: "a", capsLock: true)
        expect(client.marked == "ㄇ", "CapsLock+A stays bopomofo", client.marked)
        key(controller, client, "Escape", keyCode: 0x35, characters: "\u{1b}")
        expect(client.marked.isEmpty, "Escape clears the composition")

        // Keypad digits join the composition instead of committing it; with
        // nothing to compose they are handed back to the application.
        for (label, keyCode, characters) in [("s", UInt16(0x01), "s"), ("u", UInt16(0x20), "u"), ("3", UInt16(0x14), "3")] {
            key(controller, client, label, keyCode: keyCode, characters: characters)
        }
        let commitsBeforeKeypad = client.commits.count
        let keypadConsumed = key(controller, client, "KP_5", keyCode: 0x57, characters: "5")
        expect(keypadConsumed, "keypad 5 is consumed")
        expect(client.marked == "你5", "keypad 5 joins the composition", client.marked)
        expect(client.commits.count == commitsBeforeKeypad, "keypad 5 does not commit",
               "\(client.commits.count)")
        key(controller, client, "Return", keyCode: 0x24, characters: "\r")
        expect(client.commits.last == "你5", "Return commits the composition with the digit",
               "\(client.commits)")
        expect(client.marked.isEmpty, "keypad commit clears the marked text")

        let idleKeypadConsumed = key(controller, client, "KP_5 (idle)", keyCode: 0x57, characters: "5")
        expect(!idleKeypadConsumed, "keypad 5 passes through with nothing to compose")

        // Command shortcuts are handed back to the application.
        let controlConsumed = key(controller, client, "Ctrl+A", keyCode: 0x00, characters: "a", control: true)
        expect(!controlConsumed, "Ctrl+A passes through to the application")

        print(failures == 0 ? "input harness passed" : "input harness failed (\(failures))")
        exit(failures == 0 ? 0 : 1)
    }
}
