#include "needle.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

namespace llavon::memscan {

namespace {

constexpr char32_t kPuaStart = 0xE000;
constexpr char32_t kPuaEnd = 0xF8FF;
constexpr std::size_t kMinPuaCodepoints = 12;
constexpr std::size_t kMinAsciiCharacters = 16;
constexpr std::string_view kAsciiPrefix = "LVP";

bool decode_utf8(std::string_view input, std::vector<char32_t>& output) {
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
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
            return false;
        }
        if (index + length > input.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(input[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
        output.push_back(codepoint);
        index += length;
    }
    return true;
}

void append_utf8(std::string& output, char32_t codepoint) {
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

std::vector<std::byte> to_utf8_bytes(const std::vector<char32_t>& codepoints) {
    std::string text;
    for (const char32_t codepoint : codepoints) append_utf8(text, codepoint);
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::vector<std::byte> to_utf16_bytes(const std::vector<char32_t>& codepoints) {
    std::vector<std::byte> bytes;
    for (const char32_t codepoint : codepoints) {
        auto push = [&bytes](std::uint16_t unit) {
            bytes.push_back(static_cast<std::byte>(unit & 0xFF));
            bytes.push_back(static_cast<std::byte>((unit >> 8) & 0xFF));
        };
        if (codepoint <= 0xFFFF) {
            push(static_cast<std::uint16_t>(codepoint));
        } else {
            const char32_t adjusted = codepoint - 0x10000;
            push(static_cast<std::uint16_t>(0xD800 | (adjusted >> 10)));
            push(static_cast<std::uint16_t>(0xDC00 | (adjusted & 0x3FF)));
        }
    }
    return bytes;
}

std::vector<std::byte> to_utf32_bytes(const std::vector<char32_t>& codepoints) {
    std::vector<std::byte> bytes;
    for (const char32_t codepoint : codepoints) {
        bytes.push_back(static_cast<std::byte>(codepoint & 0xFF));
        bytes.push_back(static_cast<std::byte>((codepoint >> 8) & 0xFF));
        bytes.push_back(static_cast<std::byte>((codepoint >> 16) & 0xFF));
        bytes.push_back(static_cast<std::byte>((codepoint >> 24) & 0xFF));
    }
    return bytes;
}

}  // namespace

const char* encoding_name(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return "utf8";
        case Encoding::Utf16Le: return "utf16le";
        case Encoding::Utf32Le: return "utf32le";
    }
    return "unknown";
}

std::optional<Needle> parse_needle(const std::string& utf8, NeedleError& error) {
    std::vector<char32_t> codepoints;
    if (!decode_utf8(utf8, codepoints)) {
        error = {"needle-invalid", "probe token is not valid UTF-8"};
        return std::nullopt;
    }
    if (codepoints.size() < kMinPuaCodepoints || codepoints.size() > 64) {
        error = {"needle-invalid", "probe token must have 12..64 code points"};
        return std::nullopt;
    }

    const bool all_pua = std::ranges::all_of(codepoints, [](char32_t codepoint) {
        return codepoint >= kPuaStart && codepoint <= kPuaEnd;
    });
    const bool ascii_token = [&]() {
        if (codepoints.size() < kMinAsciiCharacters) return false;
        std::string text;
        for (const char32_t codepoint : codepoints) {
            if (codepoint > 0x7F) return false;
            text.push_back(static_cast<char>(codepoint));
        }
        if (!text.starts_with(kAsciiPrefix)) return false;
        return std::ranges::all_of(text.substr(kAsciiPrefix.size()), [](unsigned char value) {
            return std::isalnum(value) != 0;
        });
    }();
    if (!all_pua && !ascii_token) {
        error = {"needle-invalid",
                 "probe token must be all private-use code points or LVP-prefixed ASCII"};
        return std::nullopt;
    }

    Needle needle;
    needle.text = utf8;
    needle.encodings = {Encoding::Utf8, Encoding::Utf16Le, Encoding::Utf32Le};
    needle.patterns = {to_utf8_bytes(codepoints), to_utf16_bytes(codepoints),
                       to_utf32_bytes(codepoints)};
    for (const auto& pattern : needle.patterns) {
        needle.max_pattern_bytes = std::max(needle.max_pattern_bytes, pattern.size());
    }
    return needle;
}

}  // namespace llavon::memscan
