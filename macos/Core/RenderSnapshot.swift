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
