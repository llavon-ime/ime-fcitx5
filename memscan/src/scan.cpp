#include "scan.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace llavon::memscan {

namespace {

constexpr std::size_t kChunkBytes = 4 * 1024 * 1024;

struct Deadline {
    std::chrono::steady_clock::time_point at;
    bool expired() const { return std::chrono::steady_clock::now() >= at; }
};

std::optional<std::uintptr_t> parse_hex(std::string_view text) {
    std::uintptr_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
    return value;
}

std::vector<Region> read_regions(pid_t pid, bool all_mappings) {
    std::vector<Region> regions;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string range;
        std::string perms;
        if (!(stream >> range >> perms)) continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        const auto start = parse_hex(std::string_view(range).substr(0, dash));
        const auto end = parse_hex(std::string_view(range).substr(dash + 1));
        if (!start || !end || *end <= *start) continue;
        if (perms.size() < 3 || perms[0] != 'r') continue;

        std::string offset;
        std::string device;
        std::string inode;
        std::string path;
        stream >> offset >> device >> inode;
        std::getline(stream, path);
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        const bool special = path.starts_with("[vvar]") || path.starts_with("[vdso]") ||
                             path.starts_with("[vsyscall]") || path.starts_with("[stack]");
        if (special) continue;
        // Anonymous mappings (including [heap]) are where freshly inserted UI
        // text lives; file backed mappings are opt-in for debugging.
        const bool anonymous = path.empty() || path.starts_with("[heap]");
        if (!anonymous && !all_mappings) continue;

        regions.push_back(Region{*start, *end, perms.size() > 1 && perms[1] == 'w'});
    }
    // Writable anonymous memory first, newest (highest address) first: that is
    // where a text buffer allocated while typing usually sits.
    std::ranges::stable_sort(regions, [](const Region& left, const Region& right) {
        if (left.writable != right.writable) return left.writable;
        return left.start > right.start;
    });
    return regions;
}

std::vector<std::byte> read_memory(pid_t pid, std::uintptr_t address, std::size_t size,
                                   int* error_out = nullptr) {
    if (error_out != nullptr) *error_out = 0;
    std::vector<std::byte> buffer(size);
    iovec local{buffer.data(), size};
    iovec remote{reinterpret_cast<void*>(address), size};
    const ssize_t read = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (read <= 0) {
        if (error_out != nullptr) *error_out = errno;
        return {};
    }
    buffer.resize(static_cast<std::size_t>(read));
    return buffer;
}

void append_utf8(std::string& output, char32_t codepoint) {
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        codepoint = 0xFFFD;
    }
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string decode_utf8(const std::vector<std::byte>& bytes) {
    std::string output;
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        char32_t codepoint = 0;
        std::size_t length = 0;
        if (first < 0x80) {
            codepoint = first;
            length = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            length = 2;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            length = 3;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            length = 4;
        } else {
            append_utf8(output, 0xFFFD);
            ++index;
            continue;
        }
        if (index + length > bytes.size()) {
            append_utf8(output, 0xFFFD);
            break;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(bytes[index + offset]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (!valid || (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000)) {
            append_utf8(output, 0xFFFD);
            ++index;
            continue;
        }
        append_utf8(output, codepoint);
        index += length;
    }
    return output;
}

std::string decode_utf16le(const std::vector<std::byte>& bytes) {
    std::string output;
    const std::size_t units = bytes.size() / 2;
    for (std::size_t index = 0; index < units; ++index) {
        const auto low = static_cast<unsigned char>(bytes[index * 2]);
        const auto high = static_cast<unsigned char>(bytes[index * 2 + 1]);
        char32_t unit = static_cast<char32_t>(low | (high << 8));
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units) {
            const auto low2 = static_cast<unsigned char>(bytes[(index + 1) * 2]);
            const auto high2 = static_cast<unsigned char>(bytes[(index + 1) * 2 + 1]);
            const char32_t next = static_cast<char32_t>(low2 | (high2 << 8));
            if (next >= 0xDC00 && next <= 0xDFFF) {
                append_utf8(output, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                ++index;
                continue;
            }
        }
        append_utf8(output, unit);
    }
    return output;
}

std::string decode_utf32le(const std::vector<std::byte>& bytes) {
    std::string output;
    const std::size_t units = bytes.size() / 4;
    for (std::size_t index = 0; index < units; ++index) {
        const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data() + index * 4);
        const char32_t codepoint = static_cast<char32_t>(raw[0] | (raw[1] << 8) | (raw[2] << 16) |
                                                         (static_cast<char32_t>(raw[3]) << 24));
        append_utf8(output, codepoint);
    }
    return output;
}

std::string decode(const std::vector<std::byte>& bytes, Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return decode_utf8(bytes);
        case Encoding::Utf16Le: return decode_utf16le(bytes);
        case Encoding::Utf32Le: return decode_utf32le(bytes);
    }
    return {};
}

std::size_t unit_size(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return 1;
        case Encoding::Utf16Le: return 2;
        case Encoding::Utf32Le: return 4;
    }
    return 1;
}

}  // namespace

std::vector<Region> list_regions(pid_t pid, bool all_mappings) {
    return read_regions(pid, all_mappings);
}

bool same_uid(pid_t pid) {
    if (pid <= 1) return false;
    struct stat info {};
    if (::stat(("/proc/" + std::to_string(pid)).c_str(), &info) != 0) return false;
    return info.st_uid == ::getuid();
}

std::optional<Match> scan_pid(pid_t pid, const Needle& needle, std::size_t before_bytes,
                              std::size_t after_bytes, const ScanLimits& limits,
                              const std::vector<Hint>& hints, ScanError& error) {
    if (!same_uid(pid)) {
        error = {"foreign-pid", "process " + std::to_string(pid) + " is not owned by this user"};
        return std::nullopt;
    }
    const std::size_t window_limit = std::max<std::size_t>(limits.window_limit, 1024);
    before_bytes = std::min(before_bytes, window_limit);
    after_bytes = std::min(after_bytes, window_limit);
    const Deadline deadline{std::chrono::steady_clock::now() + limits.timeout};
    std::size_t scanned = 0;
    bool truncated = false;
    bool denied = false;

    // Regions to try, in order: explicit hints first, then the heuristic scan.
    std::vector<Region> regions;
    for (const auto& hint : hints) {
        if (hint.pid == pid && hint.end > hint.start) {
            regions.push_back(Region{hint.start, hint.end, true});
        }
    }
    const auto heuristic = read_regions(pid, limits.all_mappings);
    regions.insert(regions.end(), heuristic.begin(), heuristic.end());

    for (const auto& region : regions) {
        std::vector<std::byte> tail;
        std::uintptr_t position = region.start;
        while (position < region.end) {
            if (deadline.expired()) {
                error = {"timeout", "scan budget exhausted"};
                return std::nullopt;
            }
            if (scanned >= limits.max_bytes_per_pid) {
                truncated = true;
                break;
            }
            const std::size_t request = std::min(kChunkBytes, region.end - position);
            int read_error = 0;
            auto chunk = read_memory(pid, position, request, &read_error);
            if (chunk.empty()) {
                if (read_error == EPERM || read_error == EACCES) denied = true;
                tail.clear();
                position += request;
                continue;
            }
            scanned += chunk.size();

            std::vector<std::byte> area;
            area.reserve(tail.size() + chunk.size());
            area.insert(area.end(), tail.begin(), tail.end());
            area.insert(area.end(), chunk.begin(), chunk.end());
            const auto area_start = position - tail.size();

            for (std::size_t index = 0; index < needle.patterns.size(); ++index) {
                const auto& pattern = needle.patterns[index];
                const void* found = memmem(area.data(), area.size(), pattern.data(), pattern.size());
                if (found == nullptr) continue;
                const auto offset = static_cast<std::size_t>(
                    static_cast<const std::byte*>(found) - area.data());
                const auto address = area_start + offset;
                const auto encoding = needle.encodings[index];
                const std::size_t unit = unit_size(encoding);

                const std::uintptr_t left_edge = std::max<std::uintptr_t>(region.start,
                                                                          address >= before_bytes
                                                                              ? address - before_bytes
                                                                              : 0);
                const std::uintptr_t right_edge = std::min<std::uintptr_t>(
                    region.end, address + pattern.size() + after_bytes);
                auto before_raw = address > left_edge
                                      ? read_memory(pid, left_edge, address - left_edge)
                                      : std::vector<std::byte>{};
                auto after_raw = right_edge > address + pattern.size()
                                     ? read_memory(pid, address + pattern.size(),
                                                   right_edge - address - pattern.size())
                                     : std::vector<std::byte>{};
                // Keep the windows aligned to the encoding unit so the decoder
                // starts at a code unit boundary.
                if (before_raw.size() % unit != 0) {
                    before_raw.erase(before_raw.begin(),
                                     before_raw.begin() + static_cast<std::ptrdiff_t>(before_raw.size() % unit));
                }

                Match match;
                match.pid = pid;
                match.encoding = encoding;
                match.address = address;
                if (std::getenv("MEMSCAN_DEBUG") != nullptr) {
                    std::fprintf(stderr, "hit pid=%d enc=%s addr=0x%lx before=%zu after=%zu raw=",
                                 static_cast<int>(pid), encoding_name(encoding),
                                 static_cast<unsigned long>(address), before_raw.size(),
                                 after_raw.size());
                    for (const std::byte byte : before_raw) {
                        std::fprintf(stderr, "%02x", static_cast<unsigned>(byte));
                    }
                    std::fprintf(stderr, "\n");
                }
                match.before_bytes = before_raw.size();
                match.after_bytes = after_raw.size();
                match.scanned_bytes = scanned;
                match.truncated = truncated;
                match.before = decode(before_raw, encoding);
                match.after = decode(after_raw, encoding);
                if (!match.before.empty()) {
                    // Drop the replacement character a partially read lead
                    // sequence may produce.
                    const std::string replacement = "\xEF\xBF\xBD";
                    while (match.before.starts_with(replacement)) {
                        match.before.erase(0, replacement.size());
                    }
                }
                return match;
            }

            const std::size_t overlap = needle.max_pattern_bytes > 0 ? needle.max_pattern_bytes - 1 : 0;
            if (chunk.size() > overlap) {
                tail.assign(chunk.end() - static_cast<std::ptrdiff_t>(overlap), chunk.end());
            } else {
                tail = chunk;
            }
            position += request;
        }
        if (truncated) break;
    }
    if (denied) {
        error = {"denied", "process memory read denied; grant CAP_SYS_PTRACE to the helper "
                            "or set kernel.yama.ptrace_scope=0"};
    } else if (truncated) {
        error = {"budget", "token not found within the byte budget"};
    } else {
        error = {"not-found", "token not found"};
    }
    return std::nullopt;
}

}  // namespace llavon::memscan
