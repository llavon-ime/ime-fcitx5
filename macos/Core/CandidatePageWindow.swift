import Foundation

// Paging math for the candidate panel. The engine reports the whole candidate
// list together with the current page and page size (the fcitx5 addon lets
// fcitx::CommonCandidateList paginate the same inputs); the frontend slices the
// page and, when a page is taller than the panel may grow, slides a window that
// keeps the highlighted row visible. The engine's Tab-expanded mode sets the
// page size to the full candidate count, so without the sliding window one
// syllable (100+ homophones in the bundled table) would grow a screen-tall
// panel.
struct CandidatePageWindow: Equatable {
    let start: Int
    let end: Int

    static let defaultMaxVisibleRows = 10

    static func compute(candidateCount: Int,
                        page: Int,
                        pageSize: Int,
                        cursor: Int,
                        maxVisibleRows: Int = defaultMaxVisibleRows) -> CandidatePageWindow {
        let count = max(candidateCount, 0)
        let size = max(pageSize, 1)
        let offset = min(max(page, 0) * size, count)
        let pageEnd = min(offset + size, count)
        let rows = pageEnd - offset
        let maximum = max(maxVisibleRows, 0)

        var start = offset
        if rows > maximum {
            let row = min(max(cursor, 0), rows - 1)
            start = offset + min(max(row - maximum / 2, 0), rows - maximum)
        }
        return CandidatePageWindow(start: start, end: min(start + maximum, pageEnd))
    }
}
