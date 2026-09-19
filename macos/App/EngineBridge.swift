import Cocoa
import Carbon
import LlavonIME

// Snapshot of one render pass, copied out of the C ABI so the UI never reads
// engine memory directly.
struct RenderSnapshot {
    struct Segment {
        let text: String
        let underlined: Bool
    }

    var compositionEmpty = true
    var preedit: [Segment] = []
    var caret = 0
    var auxUp = ""
    var hasCandidates = false
    var target: Int32 = 0
    var symbolEpoch: UInt64 = 0
    var candidates: [String] = []
    var page = 0
    var pageSize = 0
    var pageCount = 0
    var cursor = 0
    var cursorVisible = false
    var selectionKeys: [UInt32] = []
    var layoutHint: Int32 = 0
}

enum RenderTargetC {
    static let candidates = Int32(LV_TARGET_CANDIDATES.rawValue)
    static let symbolMenu = Int32(LV_TARGET_SYMBOL_MENU.rawValue)
    static let markingHint = Int32(LV_TARGET_MARKING_HINT.rawValue)
}

enum ResetReasonC {
    static let explicit = Int32(LV_RESET_EXPLICIT.rawValue)
    static let focusOut = Int32(LV_RESET_FOCUS_OUT.rawValue)
    static let deactivate = Int32(LV_RESET_DEACTIVATE.rawValue)
}

// Process-wide bridge to the llavon-ime engine. IMK creates one controller per
// client session; they all share this engine and register their ContextId.
final class EngineBridge {
    static let shared = EngineBridge()

    private final class WeakController {
        weak var value: LlavonInputController?
        init(_ value: LlavonInputController) { self.value = value }
    }

    private var engine: OpaquePointer?
    private var nextContext: UInt64 = 1
    private var controllers: [UInt64: WeakController] = [:]
    private var pathStorage: [UnsafeMutablePointer<CChar>] = []

    private init() {}

    // MARK: - Lifecycle

    func start() {
        guard engine == nil else { return }

        var options = lv_engine_options()
        lv_engine_options_init(&options)
        options.table_path = store(resolveTablePath())
        options.phrase_overrides_path = store(resolvePhraseOverridesPath())
        options.service_path = store(resolveServicePath())
        options.model_path = store(resolveModelPath())
        options.tables_dir = store(resolveTablesDir())
        options.auto_start_service = 1
        // InputMethodKit supplies surrounding text; no accessibility provider.
        options.enable_accessibility = 0

        var host = lv_host()
        host.user = Unmanaged.passUnretained(self).toOpaque()
        host.post = enginePostTrampoline
        host.commit = engineCommitTrampoline
        host.update_ui = engineUpdateUITrampoline
        host.surrounding_text = engineSurroundingTrampoline
        host.is_sensitive = engineSensitiveTrampoline

        var created: OpaquePointer?
        if lv_engine_create(&options, &host, &created) != 0 || created == nil {
            NSLog("llavon-ime: engine creation failed")
            return
        }
        engine = created
        NSLog("llavon-ime: engine started")
    }

    func stop() {
        if let engine { lv_engine_destroy(engine) }
        engine = nil
        for pointer in pathStorage { free(pointer) }
        pathStorage.removeAll()
    }

    // MARK: - Contexts

    func allocateContext() -> UInt64 {
        let context = nextContext
        nextContext += 1
        return context
    }

    func attach(_ context: UInt64, controller: LlavonInputController) {
        controllers[context] = WeakController(controller)
        if let engine { lv_engine_attach(engine, context) }
    }

    func detach(_ context: UInt64) {
        controllers.removeValue(forKey: context)
        if let engine { lv_engine_detach(engine, context) }
    }

    func activate(_ context: UInt64) {
        if let engine { lv_engine_activate(engine, context) }
    }

    func deactivate(_ context: UInt64) {
        if let engine { lv_engine_deactivate(engine, context) }
    }

    // Commits a complete composition, used when IMK asks to finalize.
    func finalize(_ context: UInt64) {
        if let engine { lv_engine_reset(engine, context, ResetReasonC.focusOut, 1) }
    }

    func controller(for context: UInt64) -> LlavonInputController? {
        controllers[context]?.value
    }

    // MARK: - Input

    @discardableResult
    func sendKey(_ context: UInt64, event: NSEvent) -> Bool {
        guard let engine else { return false }
        var key = lv_key()
        key.sym = Keysym.symbol(for: event)
        let states = Keysym.states(for: event)
        key.states = states
        key.frontend_states = states
        key.raw_states = states
        key.caps_lock = event.modifierFlags.contains(.capsLock) ? 1 : 0
        key.release = event.type == .keyUp ? 1 : 0
        return lv_engine_key_event(engine, context, &key) != 0
    }

    func selectCandidate(_ context: UInt64, index: Int32) {
        if let engine { lv_engine_select_candidate(engine, context, index) }
    }

    func selectSymbol(_ context: UInt64, index: Int32, epoch: UInt64) {
        if let engine { lv_engine_select_symbol(engine, context, index, epoch) }
    }

    // MARK: - Rendering

    func snapshot(_ context: UInt64) -> RenderSnapshot? {
        guard let engine, let info = lv_engine_render(engine, context) else { return nil }
        var snapshot = RenderSnapshot()
        snapshot.compositionEmpty = info.pointee.composition_empty != 0
        snapshot.caret = Int(info.pointee.caret)
        snapshot.hasCandidates = info.pointee.has_candidates != 0
        snapshot.target = info.pointee.target
        snapshot.symbolEpoch = info.pointee.symbol_epoch
        snapshot.page = Int(info.pointee.page)
        snapshot.pageSize = Int(info.pointee.page_size)
        snapshot.pageCount = Int(info.pointee.page_count)
        snapshot.cursor = Int(info.pointee.cursor)
        snapshot.cursorVisible = info.pointee.cursor_visible != 0
        snapshot.layoutHint = lv_engine_layout_hint(engine)
        snapshot.auxUp = readUTF16(Int(info.pointee.aux_up_length)) { buffer, capacity in
            lv_engine_aux_up(engine, buffer, capacity)
        }

        snapshot.preedit.reserveCapacity(Int(info.pointee.preedit_segment_count))
        for index in 0..<Int(info.pointee.preedit_segment_count) {
            let length = lv_engine_preedit_segment(engine, index, nil, 0, nil)
            var buffer = [UInt16](repeating: 0, count: length)
            var underlined: Int32 = 0
            _ = lv_engine_preedit_segment(engine, index, &buffer, length, &underlined)
            let text = String(decoding: buffer, as: UTF16.self)
            snapshot.preedit.append(RenderSnapshot.Segment(text: text, underlined: underlined != 0))
        }

        snapshot.candidates.reserveCapacity(Int(info.pointee.candidate_count))
        for index in 0..<Int(info.pointee.candidate_count) {
            let length = lv_engine_candidate(engine, index, nil, 0)
            snapshot.candidates.append(readUTF16(length) { buffer, capacity in
                lv_engine_candidate(engine, index, buffer, capacity)
            })
        }

        let keyCount = Int(info.pointee.selection_key_count)
        if keyCount > 0 {
            var keys = [UInt32](repeating: 0, count: keyCount)
            _ = lv_engine_selection_keys(engine, &keys, keyCount)
            snapshot.selectionKeys = keys
        }
        return snapshot
    }

    private func readUTF16(_ length: Int, _ copy: (UnsafeMutablePointer<UInt16>?, Int) -> Int) -> String {
        guard length > 0 else { return "" }
        var buffer = [UInt16](repeating: 0, count: length)
        _ = copy(&buffer, length)
        return String(decoding: buffer, as: UTF16.self)
    }

    // MARK: - Paths

    private let supportRoot = "/Library/Application Support/llavon-ime"
    private var payloadRoot: String { supportRoot + "/payload" }
    private var home: String { FileManager.default.homeDirectoryForCurrentUser.path }

    private func resolveTablePath() -> String {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_TABLE_PATH"] { return value }
        let candidates = [
            payloadRoot + "/share/llavon-ime/tables/bopomofo_char.json",
            home + "/Library/fcitx5/share/llavon-ime/tables/bopomofo_char.json",
        ]
        return firstExisting(candidates) ?? candidates[0]
    }

    private func resolvePhraseOverridesPath() -> String {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_PHRASE_OVERRIDES_PATH"] { return value }
        return home + "/Library/Application Support/llavon-ime/phrase_overrides.txt"
    }

    private func resolveServicePath() -> String? {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_UNIX_SERVICE_PATH"] { return value }
        return firstExisting([
            payloadRoot + "/bin/llavon-ime-unix-service",
            home + "/Library/fcitx5/bin/llavon-ime-unix-service",
        ])
    }

    private func resolveTablesDir() -> String? {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_TABLES_DIR"] { return value }
        return firstExisting([
            payloadRoot + "/share/llavon-ime/tables",
            home + "/Library/fcitx5/share/llavon-ime/tables",
        ])
    }

    private func resolveModelPath() -> String? {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_MODEL_PATH"] { return value }
        return firstExisting([
            supportRoot + "/models/llavon-ime-llama-250m-Q4_K_M.gguf",
        ])
    }

    private func firstExisting(_ candidates: [String]) -> String? {
        candidates.first { FileManager.default.fileExists(atPath: $0) }
    }

    private func store(_ value: String?) -> UnsafeMutablePointer<CChar>? {
        guard let value, let pointer = value.withCString({ strdup($0) }) else { return nil }
        pathStorage.append(pointer)
        return pointer
    }
}

// MARK: - C callbacks (main thread unless documented otherwise)

private func enginePostTrampoline(_ user: UnsafeMutableRawPointer?,
                                 _ body: (@convention(c) (UnsafeMutableRawPointer?) -> Void)?,
                                 _ bodyUser: UnsafeMutableRawPointer?) {
    guard let body else { return }
    if Thread.isMainThread {
        body(bodyUser)
    } else {
        DispatchQueue.main.async { body(bodyUser) }
    }
}

private func engineCommitTrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64,
                                    _ text: UnsafePointer<UInt16>?, _ length: Int) {
    guard let user, let text, length > 0 else { return }
    let bridge = Unmanaged<EngineBridge>.fromOpaque(user).takeUnretainedValue()
    let value = String(decoding: UnsafeBufferPointer(start: text, count: length), as: UTF16.self)
    bridge.controller(for: context)?.insertCommit(value)
}

private func engineUpdateUITrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64) {
    guard let user else { return }
    let bridge = Unmanaged<EngineBridge>.fromOpaque(user).takeUnretainedValue()
    bridge.controller(for: context)?.refreshUI()
}

private func engineSurroundingTrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64,
                                         _ out: UnsafeMutablePointer<lv_surrounding_text>?) -> Int32 {
    guard let user, let out, let buffer = out.pointee.text else { return 0 }
    let bridge = Unmanaged<EngineBridge>.fromOpaque(user).takeUnretainedValue()
    guard let sample = bridge.controller(for: context)?.surroundingSample() else { return 0 }
    out.pointee.length = sample.text.count
    out.pointee.cursor = sample.cursor
    out.pointee.anchor = sample.anchor
    out.pointee.valid = 1
    let count = min(sample.text.count, Int(out.pointee.capacity))
    for index in 0..<count { buffer[index] = sample.text[index] }
    return 1
}

private func engineSensitiveTrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64) -> Int32 {
    IsSecureEventInputEnabled() ? 1 : 0
}
