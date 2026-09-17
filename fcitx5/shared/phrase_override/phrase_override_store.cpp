#include "phrase_override/phrase_override_store.hpp"

#include <fstream>
#include <string_view>

#include "text/utf.hpp"

namespace ime::fcitx5 {

namespace {

constexpr size_t kMinPhraseLength = 2;
constexpr size_t kMaxPhraseLength = 8;
constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80";

// One reading separator: the file uses '-', but the editor also accepts
// whitespace because a first tone is entered with the space key, so users type
// readings the same way they type bopomofo ("ㄌㄧˇ ㄐㄧˋ ㄧㄝˋ").
size_t separator_length(std::string_view text, size_t index) {
    if (index >= text.size()) return 0;
    switch (text[index]) {
        case '-':
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            return 1;
        default:
            break;
    }
    return text.compare(index, kIdeographicSpace.size(), kIdeographicSpace) == 0 ? kIdeographicSpace.size() : 0;
}

std::optional<std::vector<std::u16string>> split_readings(std::string_view text) {
    std::vector<std::u16string> readings;
    size_t index = 0;
    try {
        while (index < text.size()) {
            if (const size_t separator = separator_length(text, index); separator != 0) {
                index += separator;
                continue;
            }
            size_t end = index;
            while (end < text.size() && separator_length(text, end) == 0) ++end;
            readings.push_back(utf8_to_u16(text.substr(index, end - index)));
            index = end;
        }
    } catch (...) {
        return std::nullopt;
    }
    if (readings.empty()) return std::nullopt;
    return readings;
}

}  // namespace

bool PhraseOverrideStore::valid_entry(std::u16string_view phrase, size_t reading_count) {
    try {
        const auto utf8 = u16_to_utf8(phrase);
        if (utf8.find_first_of(" \t\r\n") != std::string::npos) return false;
        return reading_count >= kMinPhraseLength && reading_count <= kMaxPhraseLength &&
               utf8_to_u32(utf8).size() == reading_count;
    } catch (...) {
        return false;
    }
}

PhraseOverrideStore::PhraseOverrideStore(std::filesystem::path path) : path_(std::move(path)) {}

bool PhraseOverrideStore::load() {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        if (!ec) phrases_.clear();
        return !ec;
    }

    std::ifstream input(path_);
    if (!input) return false;

    std::map<std::string, std::u16string> loaded;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;

        const auto record = parse_line(line);
        if (!record || !valid_entry(record->phrase, record->readings.size())) continue;
        const auto key = reading_key(record->readings);
        if (!key) continue;
        loaded[*key] = record->phrase;
    }
    if (input.bad()) return false;
    phrases_ = std::move(loaded);
    return true;
}

bool PhraseOverrideStore::add(std::u16string phrase, std::span<const std::u16string> readings) {
    const auto key = reading_key(readings);
    if (!key || !valid_entry(phrase, readings.size())) return false;

    const auto previous = phrases_.find(*key);
    const std::optional<std::u16string> old_value =
        previous == phrases_.end() ? std::nullopt : std::optional(previous->second);
    phrases_[*key] = std::move(phrase);
    if (save()) return true;

    if (old_value) {
        phrases_[*key] = *old_value;
    } else {
        phrases_.erase(*key);
    }
    return false;
}

bool PhraseOverrideStore::replace(std::span<const PhraseOverrideRecord> records) {
    std::map<std::string, std::u16string> replacement;
    for (const auto& record : records) {
        const auto key = reading_key(record.readings);
        if (!key || !valid_entry(record.phrase, record.readings.size())) return false;
        replacement[*key] = record.phrase;
    }

    auto previous = std::move(phrases_);
    phrases_ = std::move(replacement);
    if (save()) return true;
    phrases_ = std::move(previous);
    return false;
}

std::string PhraseOverrideStore::format_line(std::u16string_view phrase, std::string_view reading_key) {
    return u16_to_utf8(phrase) + " " + std::string(reading_key);
}

std::optional<PhraseOverrideRecord> PhraseOverrideStore::parse_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    // Only ASCII whitespace separates the phrase from its readings: the phrase
    // itself is allowed to contain any other character.
    size_t separator = 0;
    while (separator < line.size() && line[separator] != ' ' && line[separator] != '\t') ++separator;
    if (separator >= line.size()) return std::nullopt;
    size_t reading_begin = separator;
    while (reading_begin < line.size() && (line[reading_begin] == ' ' || line[reading_begin] == '\t')) ++reading_begin;
    if (reading_begin >= line.size()) return std::nullopt;

    auto readings = split_readings(line.substr(reading_begin));
    if (!readings) return std::nullopt;

    PhraseOverrideRecord record;
    try {
        record.phrase = utf8_to_u16(line.substr(0, separator));
    } catch (...) {
        return std::nullopt;
    }
    record.readings = std::move(*readings);
    if (!reading_key(record.readings)) return std::nullopt;
    return record;
}

std::string PhraseOverrideStore::format_readings(std::span<const std::u16string> readings) {
    std::string result;
    for (const auto& reading : readings) {
        if (!result.empty()) result.push_back('-');
        result += u16_to_utf8(reading);
    }
    return result;
}

std::optional<std::u16string> PhraseOverrideStore::lookup(std::span<const std::u16string> readings) const {
    const auto key = reading_key(readings);
    if (!key) return std::nullopt;
    const auto found = phrases_.find(*key);
    if (found == phrases_.end()) return std::nullopt;
    return found->second;
}

std::vector<PhraseOverrideRecord> PhraseOverrideStore::entries() const {
    std::vector<PhraseOverrideRecord> result;
    result.reserve(phrases_.size());
    for (const auto& [key, phrase] : phrases_) {
        const auto readings = split_readings(key);
        if (readings) result.push_back({phrase, *readings});
    }
    return result;
}

const std::filesystem::path& PhraseOverrideStore::path() const noexcept {
    return path_;
}

std::optional<std::string> PhraseOverrideStore::reading_key(std::span<const std::u16string> readings) {
    if (readings.size() < kMinPhraseLength || readings.size() > kMaxPhraseLength) return std::nullopt;

    std::string key;
    try {
        for (size_t i = 0; i < readings.size(); ++i) {
            auto reading = readings[i];
            while (!reading.empty() && reading.front() == u' ') reading.erase(reading.begin());
            while (!reading.empty() && reading.back() == u' ') reading.pop_back();
            if (reading.empty()) return std::nullopt;
            const auto utf8 = u16_to_utf8(reading);
            if (utf8.find_first_of("- \t\r\n") != std::string::npos) return std::nullopt;
            if (i != 0) key.push_back('-');
            key += utf8;
        }
    } catch (...) {
        return std::nullopt;
    }
    return key;
}

bool PhraseOverrideStore::save() const {
    std::error_code ec;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
        if (ec) return false;
    }

    auto temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        for (const auto& [key, phrase] : phrases_) output << format_line(phrase, key) << '\n';
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::filesystem::rename(temporary, path_, ec);
    if (!ec) return true;
    std::filesystem::remove(temporary, ec);
    return false;
}

}  // namespace ime::fcitx5
