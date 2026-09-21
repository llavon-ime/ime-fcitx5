import Foundation
import Dispatch
import LlavonIME

// Records what the engine commits and renders; runs on the test thread.
final class RecordingHost: EngineHost {
    var commits: [String] = []
    var redraws = 0
    var surrounding: (text: [UInt16], cursor: Int, anchor: Int)?

    func insertCommit(_ text: String) { commits.append(text) }
    func refreshUI() { redraws += 1 }
    func surroundingSample() -> (text: [UInt16], cursor: Int, anchor: Int)? { surrounding }
}

// Queues engine callbacks so they run on the thread that owns the core,
// mirroring how the macOS app hops to the main queue.
final class PumpQueue {
    private let lock = NSLock()
    private var jobs: [() -> Void] = []

    func post(_ work: @escaping () -> Void) {
        lock.lock()
        jobs.append(work)
        lock.unlock()
    }

    func pumpUntilIdle(idleMilliseconds: Int = 200, maximumMilliseconds: Int = 3000) {
        let maximumDeadline = Date().addingTimeInterval(Double(maximumMilliseconds) / 1000)
        var idleSince = Date()
        while Date() < maximumDeadline {
            lock.lock()
            let job = jobs.isEmpty ? nil : jobs.removeFirst()
            lock.unlock()
            if let job {
                job()
                idleSince = Date()
            } else {
                if Date().timeIntervalSince(idleSince) >= Double(idleMilliseconds) / 1000 { return }
                Thread.sleep(forTimeInterval: 0.01)
            }
        }
    }
}

@main
struct CoreTests {
    static func main() {
        guard let tablePath = ProcessInfo.processInfo.environment["LLAVON_IME_TABLE_PATH"] else {
            FileHandle.standardError.write("set LLAVON_IME_TABLE_PATH\n".data(using: .utf8)!)
            exit(2)
        }

        let overridesPath = "/tmp/llavon-ime-core-tests-\(ProcessInfo.processInfo.processIdentifier).txt"
        let queue = PumpQueue()
        let core = EngineCore()
        core.setPostToMain { work in queue.post(work) }
        core.start(options: EngineStartOptions(tablePath: tablePath,
                                               phraseOverridesPath: overridesPath,
                                               servicePath: "/tmp/llavon-ime-no-service",
                                               modelPath: nil,
                                               tablesDir: nil,
                                               autoStartService: false,
                                               enableAccessibility: false))

        let host = RecordingHost()
        let context = core.allocateContext()
        core.attach(context, host: host)

        // Releases reach the engine and are never consumed.
        let releaseHandled = core.sendKey(context, keyCode: 0, charactersIgnoringModifiers: "a",
                                          modifiers: KeyModifiers(), capsLock: false, isRelease: true)
        precondition(!releaseHandled, "release events must not be consumed")

        // Bopomofo composition commits through the host on Return.
        type(core, context, "su3")
        queue.pumpUntilIdle()
        let returnHandled = core.sendKey(context, keyCode: 0x24, charactersIgnoringModifiers: "\r",
                                         modifiers: KeyModifiers(), capsLock: false, isRelease: false)
        precondition(returnHandled, "Return must be consumed")
        precondition(host.commits == ["你"], "expected 你, got \(host.commits)")

        // Down opens the candidate list with the engine's render state.
        type(core, context, "su3")
        queue.pumpUntilIdle()
        _ = core.sendKey(context, keyCode: 0x7d, charactersIgnoringModifiers: nil,
                         modifiers: KeyModifiers(), capsLock: false, isRelease: false)
        guard let snapshot = core.snapshot(context) else {
            fatalError("expected a render snapshot")
        }
        precondition(snapshot.hasCandidates && !snapshot.candidates.isEmpty, "expected candidates")
        precondition(snapshot.target == RenderTargetC.candidates, "expected the candidate target")
        precondition(!snapshot.selectionKeys.isEmpty, "expected selection keys")
        precondition(!snapshot.preedit.isEmpty, "expected a preedit")

        // Marking mode renders the tooltip, the hint target and underlines.
        _ = core.sendKey(context, keyCode: 0x35, charactersIgnoringModifiers: "\u{1b}",
                         modifiers: KeyModifiers(), capsLock: false, isRelease: false)
        type(core, context, "su3cl3")
        queue.pumpUntilIdle()
        var shift = KeyModifiers()
        shift.shift = true
        for _ in 0..<2 {
            _ = core.sendKey(context, keyCode: 0x7b, charactersIgnoringModifiers: nil,
                             modifiers: shift, capsLock: false, isRelease: false)
        }
        guard let marking = core.snapshot(context) else {
            fatalError("expected a marking snapshot")
        }
        precondition(marking.target == RenderTargetC.markingHint, "expected the marking hint target")
        precondition(!marking.auxUp.isEmpty, "expected the marking tooltip")
        precondition(marking.preedit.contains { $0.underlined }, "expected an underlined segment")

        // The surrounding-text callback path is exercised for a prediction.
        host.surrounding = (Array("history".utf16), 7, 7)
        type(core, context, "su3")
        queue.pumpUntilIdle()
        precondition(host.redraws > 0, "expected the engine to redraw")

        core.detach(context)
        core.stop()
        try? FileManager.default.removeItem(atPath: overridesPath)

        Self.testEngineConfig()
        Self.testCandidatePageWindow()
        Self.testKeyNormalization()
        print("core tests passed")
    }

    private static func testCandidatePageWindow() {
        func window(_ count: Int, _ page: Int, _ size: Int, _ cursor: Int,
                    maxRows: Int = CandidatePageWindow.defaultMaxVisibleRows) -> CandidatePageWindow {
            CandidatePageWindow.compute(candidateCount: count, page: page, pageSize: size,
                                        cursor: cursor, maxVisibleRows: maxRows)
        }
        func expect(_ actual: CandidatePageWindow, _ start: Int, _ end: Int, _ label: String) {
            precondition(actual == CandidatePageWindow(start: start, end: end),
                         "\(label): expected \(start)..<\(end), got \(actual.start)..<\(actual.end)")
        }

        // A syllable with 132 homophones must never draw more than one page.
        expect(window(132, 0, 10, 0), 0, 10, "first page")
        expect(window(132, 5, 10, 2), 50, 60, "middle page")
        expect(window(132, 13, 10, 1), 130, 132, "partial last page")
        expect(window(1, 0, 10, 0), 0, 1, "single candidate")
        expect(window(0, 0, 10, 0), 0, 0, "empty list")

        // Tab-expanded pages report the full count as the page size; the window
        // follows the cursor instead of growing to 132 rows.
        expect(window(132, 0, 132, 0), 0, 10, "expanded at the top")
        expect(window(132, 0, 132, 60), 55, 65, "expanded around the cursor")
        expect(window(132, 0, 132, 131), 122, 132, "expanded at the bottom")

        // A page configured taller than the panel keeps the cursor visible.
        expect(window(40, 0, 15, 14), 5, 15, "oversized page bottom")
        expect(window(40, 0, 15, 0), 0, 10, "oversized page top")
    }

    // The AppKit adapter must hand the engine the same key shape fcitx5's
    // AppKit shapes are resolved into the state-only shape the engine expects;
    // see KeyTranslation.normalized and the engine's normalize_key.
    private static func testKeyNormalization() {
        var shift = KeyModifiers()
        shift.shift = true
        var caps = KeyModifiers()
        caps.capsLock = true
        var capsShift = KeyModifiers()
        capsShift.capsLock = true
        capsShift.shift = true
        var ctrlShift = KeyModifiers()
        ctrlShift.control = true
        ctrlShift.shift = true

        // Letters: the adapter resolves CapsLock (which the engine cannot see
        // in the keysym) and folds Shift so the case carries the intent.
        var translated = KeyTranslation.normalized(sym: 0x61, modifiers: shift) // a
        precondition(translated.sym == 0x41 && !translated.modifiers.shift, "shift+a")
        translated = KeyTranslation.normalized(sym: 0x61, modifiers: caps) // a
        precondition(translated.sym == 0x41 && translated.modifiers.capsLock, "caps+a")
        translated = KeyTranslation.normalized(sym: 0x41, modifiers: capsShift) // A
        precondition(translated.sym == 0x61 && !translated.modifiers.shift, "caps+shift+a")

        // Everything else keeps Shift in the state; the engine folds it, so the
        // macOS and fcitx5 shapes agree. AppKit reports either the shifted or
        // the base keysym depending on the key, and both must work.
        for sym: UInt32 in [0x32, 0x40, 0x2f, 0x3f, 0x2c, 0x3c, 0x60, 0x7e,
                            0xff08, 0xff1b, 0xffff, 0xff8d, 0xffbd, 0xffb1,
                            0x20, 0xff0d, 0xff09, 0xff51, 0xff50, 0xff57, 0xff63] {
            let kept = KeyTranslation.normalized(sym: sym, modifiers: shift)
            precondition(kept.sym == sym && kept.modifiers.shift, "keeps Shift: \(sym)")
        }

        // Ctrl+Shift+letter stays a shortcut: the case folds, the state stays.
        translated = KeyTranslation.normalized(sym: 0x61, modifiers: ctrlShift)
        precondition(translated.sym == 0x41 && translated.modifiers.shift, "ctrl+shift+a")
    }

    // The settings model is schema-driven: the engine exports the field list
    // and values decode generically, so no field name is hardcoded here.
    private static func testEngineConfig() {
        guard let schema = EngineCore.configSchema(), !schema.fields.isEmpty else {
            fatalError("expected the engine to export a config schema")
        }
        for field in schema.fields {
            precondition(!field.key.isEmpty && !field.label.isEmpty && !field.group.isEmpty,
                         "expected every field to carry a key, label and group")
            switch field.kind {
            case .choice:
                precondition(!field.choices.isEmpty, "choice fields need choices: \(field.key)")
            case .integer:
                precondition(field.minimum != nil && field.maximum != nil, "integer bounds: \(field.key)")
            default:
                break
            }
        }

        let json = Data(#"{"candidate_page_size":7,"candidate_layout":"vertical","smart_english":true,"model_path":"/tmp/model.gguf"}"#.utf8)
        guard var config = EngineConfig.decode(schema: schema, json: json) else {
            fatalError("expected config values to decode")
        }
        precondition(config.value("candidate_page_size") == .integer(7), "expected candidate_page_size")
        precondition(config.value("candidate_layout") == .text("vertical"), "expected candidate_layout")
        precondition(config.value("smart_english") == .boolean(true), "expected smart_english")
        precondition(config.value("keyboard_layout") == nil, "missing keys must stay unset")

        config.set("candidate_page_size", .integer(4))
        config.set("selection_keys", .text("asdfghjkl"))
        guard let encoded = config.jsonData(),
              let roundtrip = EngineConfig.decode(schema: schema, json: encoded) else {
            fatalError("expected config values to encode")
        }
        precondition(roundtrip.value("candidate_page_size") == .integer(4), "expected the value to round-trip")
        precondition(roundtrip.value("selection_keys") == .text("asdfghjkl"), "expected the value to round-trip")
        precondition(roundtrip.value("model_path") == .text("/tmp/model.gguf"), "expected model_path to round-trip")

        // The app mirrors the fcitx5 addon by keeping the effective model path
        // in the config JSON: missing/empty gets filled, an existing path wins.
        guard let filled = EngineConfig.fillingModelPath(nil, with: "/tmp/filled.gguf"),
              let filledData = filled.data(using: .utf8),
              let filledObject = try? JSONSerialization.jsonObject(with: filledData) as? [String: Any] else {
            fatalError("expected a filled config JSON")
        }
        precondition(filledObject["model_path"] as? String == "/tmp/filled.gguf", "expected model_path to be filled")
        precondition(EngineConfig.fillingModelPath(#"{"model_path":"/keep.gguf"}"#, with: "/new.gguf")
                     == #"{"model_path":"/keep.gguf"}"#, "expected an existing model path to win")
        let replaced = EngineConfig.fillingModelPath(#"{"model_path":""}"#, with: "/new.gguf")
        precondition(replaced?.contains("/new.gguf") == true, "expected an empty model path to be replaced")
        precondition(EngineConfig.fillingModelPath(#"{"model_path":""}"#, with: nil)
                     == #"{"model_path":""}"#, "expected no path to leave the config alone")
    }

    private static func type(_ core: EngineCore, _ context: UInt64, _ text: String) {
        for scalar in text.unicodeScalars {
            _ = core.sendKey(context, keyCode: 0, charactersIgnoringModifiers: String(scalar),
                             modifiers: KeyModifiers(), capsLock: false, isRelease: false)
        }
    }
}
