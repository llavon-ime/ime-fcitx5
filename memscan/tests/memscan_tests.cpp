#include "needle.hpp"
#include "scan.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// A private-use-area probe token, the shape the input method uses.
std::string pua_token(std::mt19937_64& rng, std::size_t count = 16) {
    std::uniform_int_distribution<int> distribution(0xE000, 0xF8FF);
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
        const char32_t codepoint = static_cast<char32_t>(distribution(rng));
        text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return text;
}

// Tests only use ASCII plus BMP private-use code points, so this is enough.
std::u16string to_utf16(std::string_view input) {
    std::u16string output;
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first < 0x80) {
            output.push_back(static_cast<char16_t>(first));
            ++index;
        } else {
            const char32_t codepoint = static_cast<char32_t>(
                ((first & 0x0F) << 12) |
                ((static_cast<unsigned char>(input[index + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(input[index + 2]) & 0x3F));
            output.push_back(static_cast<char16_t>(codepoint));
            index += 3;
        }
    }
    return output;
}

struct Holder {
    pid_t pid = -1;
    std::uintptr_t address = 0;
};

// The child keeps the probe text in a heap buffer and reports its address so
// the scanner can be pointed at exactly that buffer (the parent also holds a
// copy of the probe token in its own heap, which is what a real engine avoids
// by never scanning itself).
Holder spawn_holder(const std::string& text, bool utf16) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        std::uintptr_t address = 0;
        if (utf16) {
            [[maybe_unused]] auto* buffer = new std::u16string(to_utf16(text));
            address = reinterpret_cast<std::uintptr_t>(buffer->data());
        } else {
            [[maybe_unused]] auto* buffer = new std::string(text);
            address = reinterpret_cast<std::uintptr_t>(buffer->data());
        }
        [[maybe_unused]] const auto written = ::write(ready[1], &address, sizeof(address));
        ::pause();
        ::_exit(0);
    }
    ::close(ready[1]);
    std::uintptr_t address = 0;
    [[maybe_unused]] const auto got = ::read(ready[0], &address, sizeof(address));
    ::close(ready[0]);
    return {pid, address};
}

void stop_holder(pid_t pid) {
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

}  // namespace

int main() {
    using namespace llavon::memscan;

    std::mt19937_64 rng(0x5eed);
    const std::string token = pua_token(rng);
    NeedleError needle_error;
    const auto needle = parse_needle(token, needle_error);
    check(needle.has_value(), "PUA probe token accepted");

    check(!parse_needle("short token", needle_error).has_value(), "short token rejected");
    check(!parse_needle("aaaaaaaaaaaaaaaaaaaaaaaa", needle_error).has_value(),
          "plain ASCII token rejected");
    check(parse_needle("LVPabcdefghijklmnop1234", needle_error).has_value(),
          "LVP-prefixed ASCII token accepted");

    ScanLimits limits;
    limits.timeout = std::chrono::milliseconds(3000);
    const std::string text = "hello magic " + token + " seed line\n";

    for (const bool utf16 : {false, true}) {
        const Holder holder = spawn_holder(text, utf16);
        check(holder.pid > 0, std::string("holder spawned (") + (utf16 ? "utf16" : "utf8") + ")");
        if (holder.pid <= 0) continue;
        ScanError error;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 256}};
        const auto match = scan_pid(holder.pid, *needle, 64, 32, limits, hints, std::string{}, error);
        check(match.has_value(),
              std::string("scan finds token (") + (utf16 ? "utf16" : "utf8") + "): " + error.code);
        if (match) {
            check(match->encoding == (utf16 ? Encoding::Utf16Le : Encoding::Utf8),
                  std::string("encoding reported: ") + llavon::memscan::encoding_name(match->encoding));
            check(match->before.ends_with("hello magic "), std::string("text before: ") + match->before);
            check(match->after.starts_with(" seed line"), std::string("text after: ") + match->after);
        }
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_holder(text, false);
        ScanError error;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 256}};
        const auto match = scan_pid(holder.pid, *needle, 64, 32, limits, hints, "hello magic ", error);
        check(match.has_value() && match->before.ends_with("hello magic "),
              "expect-suffix match is accepted");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_holder(text, false);
        ScanError error;
        const std::vector<Hint> hints{{holder.pid, 0x1000, 0x2000}};
        const auto match = scan_pid(holder.pid, *needle, 64, 32, limits, hints, std::string{}, error);
        check(match.has_value(), "bogus hint does not break the scan");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_holder(text, false);
        ScanLimits tight = limits;
        tight.timeout = std::chrono::milliseconds(0);
        ScanError error;
        const auto match = scan_pid(holder.pid, *needle, 64, 32, tight, {}, std::string{}, error);
        check(!match.has_value() && error.code == "timeout", "timeout budget enforced");
        stop_holder(holder.pid);
    }

    if (::getuid() != 0) {
        ScanError error;
        const auto match = scan_pid(1, *needle, 64, 32, limits, {}, std::string{}, error);
        check(!match.has_value() && error.code == "foreign-pid", "root process refused");
        check(!same_uid(1), "same_uid rejects root");
    }

    if (failures == 0) {
        std::printf("memscan tests passed\n");
        return 0;
    }
    std::printf("memscan tests failed: %d\n", failures);
    return 1;
}
