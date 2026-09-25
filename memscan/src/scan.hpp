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

// True when /proc/<pid> is owned by the calling user.
bool same_uid(pid_t pid);

// Finds the probe token in one process and returns the decoded window around
// it. Refuses foreign PIDs, validates the caller-provided budget, and never
// returns more than ScanLimits::window_limit bytes per side.
std::optional<Match> scan_pid(pid_t pid, const Needle& needle, std::size_t before_bytes,
                              std::size_t after_bytes, const ScanLimits& limits,
                              const std::vector<Hint>& hints, ScanError& error);

}  // namespace llavon::memscan
