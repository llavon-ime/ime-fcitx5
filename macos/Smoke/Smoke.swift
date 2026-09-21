// Swift smoke test for the llavon-ime C ABI. It drives the engine without
// InputMethodKit: keys in, commits/renders out. Build on macOS with the
// engine static library (see macos/README.md).
import Foundation
import LlavonIME

private final class HostState {
    var commits: [String] = []
    var redraws = 0
    var surrounding = "history"
}

private func bridge(_ user: UnsafeMutableRawPointer?) -> HostState {
    Unmanaged<HostState>.fromOpaque(user!).takeUnretainedValue()
}

private func postBody(_ user: UnsafeMutableRawPointer?, _ body: (@convention(c) (UnsafeMutableRawPointer?) -> Void)?,
                      _ bodyUser: UnsafeMutableRawPointer?) {
    // Run posted work immediately; a real host would dispatch to the main queue.
    body?(bodyUser)
}

private func commitCallback(_ user: UnsafeMutableRawPointer?, _ context: UInt64,
                            _ text: UnsafePointer<UInt16>?, _ length: Int) {
    guard let text, length > 0 else { return }
    let value = String(decoding: UnsafeBufferPointer(start: text, count: length), as: UTF16.self)
    bridge(user).commits.append(value)
}

private func updateUICallback(_ user: UnsafeMutableRawPointer?, _ context: UInt64) {
    bridge(user).redraws += 1
}

private func surroundingCallback(_ user: UnsafeMutableRawPointer?, _ context: UInt64,
                                 _ out: UnsafeMutablePointer<lv_surrounding_text>?) -> Int32 {
    guard let out, let buffer = out.pointee.text else { return 0 }
    let units = Array(bridge(user).surrounding.utf16)
    out.pointee.length = units.count
    out.pointee.cursor = units.count
    out.pointee.anchor = units.count
    out.pointee.valid = 1
    let count = min(units.count, out.pointee.capacity)
    for index in 0..<count { buffer[index] = units[index] }
    return 1
}

private func isSensitiveCallback(_ user: UnsafeMutableRawPointer?, _ context: UInt64) -> Int32 { 0 }

private func makeKey(_ symbol: UInt32, _ states: UInt32 = 0, release: Bool = false) -> lv_key {
    lv_key(sym: symbol, states: states, frontend_states: states, raw_states: states,
           caps_lock: 0, release: release ? 1 : 0)
}

@discardableResult
private func sendKey(_ engine: OpaquePointer, _ context: UInt64, _ key: lv_key) -> Int32 {
    var mutable = key
    return lv_engine_key_event(engine, context, &mutable)
}

private func typeText(_ engine: OpaquePointer, _ context: UInt64, _ text: String) {
    for scalar in text.unicodeScalars {
        sendKey(engine, context, makeKey(scalar.value))
    }
}

@main
struct Smoke {
    static func main() {
        guard let table = ProcessInfo.processInfo.environment["LLAVON_IME_TABLE_PATH"] else {
            FileHandle.standardError.write("set LLAVON_IME_TABLE_PATH\n".data(using: .utf8)!)
            exit(2)
        }
        guard let tablePath = table.withCString({ strdup($0) }) else {
            FileHandle.standardError.write("strdup failed\n".data(using: .utf8)!)
            exit(2)
        }
        defer { free(tablePath) }

        let state = HostState()
        state.surrounding = "history"
        let user = Unmanaged.passUnretained(state).toOpaque()

        // C strings must stay valid for the engine's lifetime.
        guard let overridesPath = "/tmp/llavon-ime-swift-smoke-overrides.txt".withCString({ strdup($0) }),
              let socketPath = "/tmp/llavon-ime-swift-smoke-no-service.sock".withCString({ strdup($0) }) else {
            FileHandle.standardError.write("strdup failed\n".data(using: .utf8)!)
            exit(2)
        }
        defer {
            free(overridesPath)
            free(socketPath)
        }

        var host = lv_host()
        host.user = user
        host.post = postBody
        host.commit = commitCallback
        host.update_ui = updateUICallback
        host.surrounding_text = surroundingCallback
        host.is_sensitive = isSensitiveCallback

        var options = lv_engine_options()
        lv_engine_options_init(&options)
        options.table_path = tablePath
        options.phrase_overrides_path = overridesPath
        options.auto_start_service = 0
        options.enable_accessibility = 0
        options.socket_path = socketPath

        var engine: OpaquePointer?
        guard lv_engine_create(&options, &host, &engine) == 0, let engine else {
            FileHandle.standardError.write("lv_engine_create failed\n".data(using: .utf8)!)
            exit(1)
        }
        defer { lv_engine_destroy(engine) }

        let context: UInt64 = 1
        lv_engine_attach(engine, context)

        // Release events reach the engine and are not consumed.
        precondition(sendKey(engine, context, makeKey(UInt32(UInt8(ascii: "a")), 0, release: true)) == 0)

        typeText(engine, context, "su3")
        Thread.sleep(forTimeInterval: 0.2)
        sendKey(engine, context, makeKey(0xff0d))

        guard state.commits == ["你"] else {
            FileHandle.standardError.write("expected 你, got \(state.commits)\n".data(using: .utf8)!)
            exit(1)
        }

        typeText(engine, context, "su3")
        Thread.sleep(forTimeInterval: 0.2)
        sendKey(engine, context, makeKey(0xff54))
        guard let info = lv_engine_render(engine, context), info.pointee.has_candidates != 0,
              info.pointee.candidate_count > 0 else {
            FileHandle.standardError.write("expected candidates\n".data(using: .utf8)!)
            exit(1)
        }
        print("swift smoke test passed")
    }
}
