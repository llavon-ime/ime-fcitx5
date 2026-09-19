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
        print("core tests passed")
    }

    private static func type(_ core: EngineCore, _ context: UInt64, _ text: String) {
        for scalar in text.unicodeScalars {
            _ = core.sendKey(context, keyCode: 0, charactersIgnoringModifiers: String(scalar),
                             modifiers: KeyModifiers(), capsLock: false, isRelease: false)
        }
    }
}
