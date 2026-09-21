import Cocoa
import InputMethodKit

// The subset of the text client the input method drives. InputMethodKit's
// IMKTextInput is adapted to it, and the raw-input harness uses a recording
// double; keeping the controller behind this protocol is what makes the
// controller testable without a real IMK session.
protocol TextClient: AnyObject {
    func insertText(_ text: String, replacementRange: NSRange)
    func setMarkedText(_ text: NSAttributedString, selectionRange: NSRange, replacementRange: NSRange)
    func selectedRange() -> NSRange
    func markedRange() -> NSRange
    func attributedSubstring(from range: NSRange) -> NSAttributedString?
    func attributes(forCharacterIndex index: Int,
                    lineHeightRectangle: UnsafeMutablePointer<NSRect>?) -> [AnyHashable: Any]?
    func windowLevel() -> Int32
}

// Adapts an IMK client to TextClient.
final class IMKTextClientSink: TextClient {
    let client: IMKTextInput

    init(client: IMKTextInput) {
        self.client = client
    }

    func insertText(_ text: String, replacementRange: NSRange) {
        client.insertText(text, replacementRange: replacementRange)
    }

    func setMarkedText(_ text: NSAttributedString, selectionRange: NSRange, replacementRange: NSRange) {
        client.setMarkedText(text, selectionRange: selectionRange, replacementRange: replacementRange)
    }

    func selectedRange() -> NSRange {
        client.selectedRange()
    }

    func markedRange() -> NSRange {
        client.markedRange()
    }

    func attributedSubstring(from range: NSRange) -> NSAttributedString? {
        client.attributedSubstring(from: range)
    }

    func attributes(forCharacterIndex index: Int,
                    lineHeightRectangle: UnsafeMutablePointer<NSRect>?) -> [AnyHashable: Any]? {
        client.attributes(forCharacterIndex: index, lineHeightRectangle: lineHeightRectangle)
    }

    func windowLevel() -> Int32 {
        client.windowLevel()
    }
}
