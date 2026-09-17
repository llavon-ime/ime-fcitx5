#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ime::fcitx5 {

struct PhraseOverrideRecord {
    std::u16string phrase;
    std::vector<std::u16string> readings;
};

class PhraseOverrideStore {
public:
    explicit PhraseOverrideStore(std::filesystem::path path);

    bool load();
    bool add(std::u16string phrase, std::span<const std::u16string> readings);
    bool replace(std::span<const PhraseOverrideRecord> records);
    std::optional<std::u16string> lookup(std::span<const std::u16string> readings) const;
    std::vector<PhraseOverrideRecord> entries() const;
    static std::string format_readings(std::span<const std::u16string> readings);
    static std::string format_line(std::u16string_view phrase, std::string_view reading_key);
    static std::optional<PhraseOverrideRecord> parse_line(std::string_view line);

    // The phrase must have exactly one character per reading and stay within
    // the supported syllable count, which is what makes the exact-match
    // lookup meaningful.
    static bool valid_entry(std::u16string_view phrase, size_t reading_count);

    const std::filesystem::path& path() const noexcept;

private:
    static std::optional<std::string> reading_key(std::span<const std::u16string> readings);
    bool save() const;

    std::filesystem::path path_;
    std::map<std::string, std::u16string> phrases_;
};

}  // namespace ime::fcitx5
