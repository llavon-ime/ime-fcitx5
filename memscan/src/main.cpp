#include "needle.hpp"
#include "scan.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using llavon::memscan::Hint;
using llavon::memscan::Match;
using llavon::memscan::Needle;
using llavon::memscan::NeedleError;
using llavon::memscan::ScanError;
using llavon::memscan::ScanLimits;

struct Options {
    std::string needle;
    std::vector<pid_t> pids;
    std::vector<Hint> hints;
    std::size_t before = 4096;
    std::size_t after = 512;
    std::size_t max_bytes = 512ull * 1024 * 1024;
    long timeout_ms = 3000;
    bool all_mappings = false;
    bool list_mappings = false;
    // Text the input method just committed; the document copy of the token
    // sits right after it.
    std::string expect_suffix;
    // Scan every process owned by the caller instead of an explicit PID list.
    // The caller and its parent are skipped so the input method's own copy of
    // the probe token is never the match.
    bool same_uid_all = false;
};

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const unsigned char value : input) {
        switch (value) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                    output += buffer;
                } else {
                    output.push_back(static_cast<char>(value));
                }
        }
    }
    return output;
}

std::optional<std::uintptr_t> parse_hex(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    std::uintptr_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

void usage() {
    std::fprintf(stderr,
                 "usage: llavon-ime-memscan --needle <text> --pid <pid> [--pid <pid>...]\n"
                 "       [--before <bytes>] [--after <bytes>] [--max-bytes <bytes>]\n"
                 "       [--timeout-ms <ms>] [--hint <pid>:<hexstart>-<hexend>]\n"
                 "       [--all-mappings] [--list-mappings]\n");
}

void fail(const std::string& code, const std::string& detail, int status) {
    std::printf("{\"found\":false,\"error\":\"%s\",\"detail\":\"%s\"}\n", code.c_str(),
                json_escape(detail).c_str());
    std::exit(status);
}

void print_match(const Match& match) {
    char address[32];
    std::snprintf(address, sizeof(address), "0x%lx",
                  static_cast<unsigned long>(match.address));
    std::printf(
        "{\"found\":true,\"pid\":%d,\"encoding\":\"%s\",\"address\":\"%s\","
        "\"before\":\"%s\",\"after\":\"%s\",\"before_bytes\":%zu,\"after_bytes\":%zu,"
        "\"scanned_bytes\":%zu,\"truncated\":%s}\n",
        static_cast<int>(match.pid), llavon::memscan::encoding_name(match.encoding), address,
        json_escape(match.before).c_str(), json_escape(match.after).c_str(), match.before_bytes,
        match.after_bytes, match.scanned_bytes, match.truncated ? "true" : "false");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto next = [&](const char* name) -> std::string_view {
            if (index + 1 >= argc) fail("usage", std::string("missing value for ") + name, 2);
            return argv[++index];
        };
        if (argument == "--needle") {
            options.needle = std::string(next("--needle"));
        } else if (argument == "--pid") {
            const auto value = parse_number<long>(next("--pid"));
            if (!value || *value <= 1 || *value > 4194304) fail("usage", "invalid --pid", 2);
            options.pids.push_back(static_cast<pid_t>(*value));
        } else if (argument == "--before") {
            const auto value = parse_number<std::size_t>(next("--before"));
            if (!value || *value > 65536) fail("usage", "invalid --before", 2);
            options.before = *value;
        } else if (argument == "--after") {
            const auto value = parse_number<std::size_t>(next("--after"));
            if (!value || *value > 65536) fail("usage", "invalid --after", 2);
            options.after = *value;
        } else if (argument == "--max-bytes") {
            const auto value = parse_number<std::size_t>(next("--max-bytes"));
            if (!value || *value > (4ull * 1024 * 1024 * 1024)) fail("usage", "invalid --max-bytes", 2);
            options.max_bytes = *value;
        } else if (argument == "--timeout-ms") {
            const auto value = parse_number<long>(next("--timeout-ms"));
            if (!value || *value < 0 || *value > 60000) fail("usage", "invalid --timeout-ms", 2);
            options.timeout_ms = *value;
        } else if (argument == "--hint") {
            const std::string_view text = next("--hint");
            const auto colon = text.find(':');
            const auto dash = text.find('-', colon == std::string_view::npos ? 0 : colon + 1);
            if (colon == std::string_view::npos || dash == std::string_view::npos)
                fail("usage", "invalid --hint", 2);
            const auto pid = parse_number<long>(text.substr(0, colon));
            const auto start = parse_hex(text.substr(colon + 1, dash - colon - 1));
            const auto end = parse_hex(text.substr(dash + 1));
            if (!pid || !start || !end || *end <= *start) fail("usage", "invalid --hint", 2);
            options.hints.push_back(Hint{static_cast<pid_t>(*pid), *start, *end});
        } else if (argument == "--all-mappings") {
            options.all_mappings = true;
        } else if (argument == "--expect-suffix") {
            options.expect_suffix = std::string(next("--expect-suffix"));
            if (options.expect_suffix.size() > 256) fail("usage", "--expect-suffix is too long", 2);
        } else if (argument == "--same-uid-all") {
            options.same_uid_all = true;
        } else if (argument == "--list-mappings") {
            options.list_mappings = true;
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        } else {
            fail("usage", std::string("unknown argument: ") + std::string(argument), 2);
        }
    }

    if (options.pids.empty() && !options.same_uid_all) fail("usage", "--pid is required", 2);
    if (options.same_uid_all) {
        for (const pid_t pid : llavon::memscan::same_uid_processes(64)) {
            if (std::ranges::find(options.pids, pid) == options.pids.end()) options.pids.push_back(pid);
        }
        if (options.pids.empty()) fail("not-found", "no candidate processes", 1);
    }

    if (options.list_mappings) {
        std::printf("{\"found\":false,\"mappings\":[");
        bool first = true;
        for (const pid_t pid : options.pids) {
            for (const auto& region : llavon::memscan::list_regions(pid, options.all_mappings)) {
                char start[32];
                char end[32];
                std::snprintf(start, sizeof(start), "0x%lx", static_cast<unsigned long>(region.start));
                std::snprintf(end, sizeof(end), "0x%lx", static_cast<unsigned long>(region.end));
                std::printf("%s{\"pid\":%d,\"start\":\"%s\",\"end\":\"%s\",\"writable\":%s}",
                            first ? "" : ",", static_cast<int>(pid), start, end,
                            region.writable ? "true" : "false");
                first = false;
            }
        }
        std::printf("]}\n");
        return 0;
    }

    NeedleError needle_error;
    const auto needle = llavon::memscan::parse_needle(options.needle, needle_error);
    if (!needle) fail(needle_error.code, needle_error.detail, 2);

    ScanLimits limits;
    limits.max_bytes_per_pid = options.max_bytes;
    limits.timeout = std::chrono::milliseconds(options.timeout_ms);
    limits.all_mappings = options.all_mappings;

    ScanError last_error{"not-found", "token not found"};
    for (const pid_t pid : options.pids) {
        if (!llavon::memscan::same_uid(pid)) {
            last_error = {"foreign-pid", "process " + std::to_string(pid) +
                                              " is not owned by this user"};
            continue;
        }
        ScanError error;
        const auto match = llavon::memscan::scan_pid(pid, *needle, options.before, options.after,
                                                     limits, options.hints, options.expect_suffix,
                                                     error);
        if (match) {
            print_match(*match);
            return 0;
        }
        last_error = error;
    }

    int status = 1;
    if (last_error.code == "foreign-pid" || last_error.code == "denied") status = 3;
    else if (last_error.code == "timeout" || last_error.code == "budget") status = 4;
    fail(last_error.code, last_error.detail, status);
    return status;
}
