#pragma once

#include "needle.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace llavon::memscan {

// A previously successful region for a PID; tried before the full scan so a
// repeat probe is fast.
struct Hint {
    pid_t pid = -1;
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
};

struct ScanLimits {
    std::size_t max_bytes_per_pid = 512ull * 1024 * 1024;
    std::chrono::milliseconds timeout{3000};
    std::size_t window_limit = 64 * 1024;
    bool all_mappings = false;
};

struct Match {
    pid_t pid = -1;
    Encoding encoding = Encoding::Utf8;
    std::uintptr_t address = 0;
    std::string before;
    std::string after;
    std::size_t before_bytes = 0;
    std::size_t after_bytes = 0;
    std::size_t scanned_bytes = 0;
    bool truncated = false;
};

struct ScanError {
    std::string code;
    std::string detail;
};

// One readable mapping of the target; exposed for diagnostics and tests.
struct Region {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    bool writable = false;
};

std::vector<Region> list_regions(pid_t pid, bool all_mappings);

// PIDs owned by the calling user, excluding this process and its parent (the
// input method that spawned the helper). Sorted by resident set size so large
// GUI applications are scanned first, capped at `limit`.
std::vector<int> same_uid_processes(std::size_t limit);

// True when /proc/<pid> is owned by the calling user.
bool same_uid(pid_t pid);

// Finds the probe token in one process and returns the decoded window around
// it. Refuses foreign PIDs, validates the caller-provided budget, and never
// returns more than ScanLimits::window_limit bytes per side.
//
// `expect_suffix` is the text the input method just committed: the caret sits
// right after it, so a match whose window ends with that text is the document
// copy and wins over protocol buffers or layout caches that also hold the
// token.
std::optional<Match> scan_pid(pid_t pid, const Needle& needle, std::size_t before_bytes,
                              std::size_t after_bytes, const ScanLimits& limits,
                              const std::vector<Hint>& hints, const std::string& expect_suffix,
                              ScanError& error);

}  // namespace llavon::memscan
