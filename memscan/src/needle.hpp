#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llavon::memscan {

// Byte encodings a probe token can be stored in. UI toolkits use UTF-8 (GTK,
// terminals) or UTF-16 (Qt, Java, Chromium); terminals that store cells as
// code points need UTF-32.
enum class Encoding : std::uint8_t { Utf8, Utf16Le, Utf32Le };

const char* encoding_name(Encoding encoding);

struct Needle {
    std::string text;                              // canonical UTF-8
    std::vector<std::vector<std::byte>> patterns;  // parallel to encodings
    std::vector<Encoding> encodings;
    std::size_t max_pattern_bytes = 0;
};

struct NeedleError {
    std::string code;
    std::string detail;
};

// The helper refuses to search arbitrary content: the token has to be either
// at least 12 private-use-area code points (the mode the input method uses so
// that modal terminals never interpret probe characters as commands) or a
// "LVP"-prefixed ASCII token with at least 16 alphanumeric characters (used by
// tests and manual debugging).
std::optional<Needle> parse_needle(const std::string& utf8, NeedleError& error);

}  // namespace llavon::memscan
