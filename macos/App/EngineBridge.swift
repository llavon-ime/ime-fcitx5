import Cocoa
import Carbon

// macOS specialization of the engine core: posts engine callbacks to the main
// queue, reads secure-input state and resolves the packaged resource paths.
final class EngineBridge: EngineCore {
    static let shared = EngineBridge()

    private let supportRoot = "/Library/Application Support/llavon-ime"

    private override init() {
        super.init()
        setPostToMain { work in
            if Thread.isMainThread {
                work()
            } else {
                DispatchQueue.main.async(execute: work)
            }
        }
        setSensitiveCheck { IsSecureEventInputEnabled() }
    }

    func startResolved() {
        start(options: EngineStartOptions(tablePath: resolveTablePath(),
                                          phraseOverridesPath: resolvePhraseOverridesPath(),
                                          servicePath: resolveServicePath(),
                                          modelPath: resolveModelPath(),
                                          tablesDir: resolveTablesDir(),
                                          autoStartService: true,
                                          // InputMethodKit supplies surrounding text.
                                          enableAccessibility: false))
    }

    // MARK: - Paths

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
}
