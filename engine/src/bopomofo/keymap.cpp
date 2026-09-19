#include "bopomofo/keymap.hpp"

#include <unordered_map>

namespace ime::fcitx5 {

namespace {

char32_t normalize_ascii_letter(char32_t key) {
    if (key >= U'A' && key <= U'Z') return key + (U'a' - U'A');
    return key;
}

bool is_hsu_end_key(char32_t key, const Syllable& syllable) {
    if (syllable.empty()) return false;
    return key == U'd' || key == U'f' || key == U'j' || key == U's' || key == U' ';
}

// Hsu alternative readings, mirroring libchewing's Hsu::ALT_TABLE. They only
// apply to first-tone readings, which end with a trailing ASCII space.
const std::vector<std::u16string>& hsu_alternative_readings(const Syllable& syllable) {
    static const std::unordered_map<std::u16string, std::vector<std::u16string>> table{
        {u"ㄘ ", {u"ㄟ "}},   {u"ㄧ ", {u"ㄝ "}},   {u"ㄙ ", {u"˙"}},   {u"ㄉ ", {u"ˊ"}},
        {u"ㄈ ", {u"ˇ"}},     {u"ㄜ ", {u"ㄍ "}},   {u"ㄛ ", {u"ㄏ "}}, {u"ㄓ ", {u"ㄐ ", u"ˋ"}},
        {u"ㄤ ", {u"ㄎ "}},   {u"ㄦ ", {u"ㄌ ", u"ㄥ "}}, {u"ㄕ ", {u"ㄒ "}},
        {u"ㄔ ", {u"ㄑ "}},   {u"ㄣ ", {u"ㄋ "}},   {u"ㄢ ", {u"ㄇ "}},
    };
    static const std::vector<std::u16string> empty;
    const auto it = table.find(syllable.text());
    if (it == table.end()) return empty;
    return it->second;
}

// Ordered Hsu conversion rules from libchewing's src/editor/zhuyin_layout/hsu.rs.
BopomofoKeyResult apply_hsu_key(Syllable& syllable, char32_t key) {
    if (is_hsu_end_key(key, syllable)) {
        // Step 1: normalize a singleton initial into its final counterpart.
        if (!syllable.has_medial() && !syllable.has_final()) {
            switch (syllable.initial()) {
                case U'ㄐ':
                    (void)syllable.overwrite(U'ㄓ');
                    break;
                case U'ㄑ':
                    (void)syllable.overwrite(U'ㄔ');
                    break;
                case U'ㄒ':
                    (void)syllable.overwrite(U'ㄕ');
                    break;
                case U'ㄏ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄛ');
                    break;
                case U'ㄍ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄜ');
                    break;
                case U'ㄇ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄢ');
                    break;
                case U'ㄋ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄣ');
                    break;
                case U'ㄎ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄤ');
                    break;
                case U'ㄌ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄦ');
                    break;
                default:
                    break;
            }
        }

        // Step 2: apply the delayed ㄍㄧ/ㄍㄩ to ㄐㄧ/ㄐㄩ conversion.
        if (syllable.initial() == U'ㄍ' && (syllable.medial() == U'ㄧ' || syllable.medial() == U'ㄩ')) {
            (void)syllable.overwrite(U'ㄐ');
        }

        // Step 3: apply the tone.
        switch (key) {
            case U'd':
                (void)syllable.overwrite(U'ˊ');
                break;
            case U'f':
                (void)syllable.overwrite(U'ˇ');
                break;
            case U'j':
                (void)syllable.overwrite(U'ˋ');
                break;
            case U's':
                (void)syllable.overwrite(U'˙');
                break;
            default:
                (void)syllable.overwrite(U' ');
                break;
        }

        BopomofoKeyResult result;
        if (syllable.complete()) {
            result.status = BopomofoKeyStatus::Completed;
            result.alternative_readings = hsu_alternative_readings(syllable);
        } else {
            result.status = BopomofoKeyStatus::Composing;
        }
        return result;
    }

    // Map the physical key using the syllable state before this key.
    char32_t symbol = 0;
    switch (key) {
        case U'a':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄟ' : U'ㄘ';
            break;
        case U'b':
            symbol = U'ㄅ';
            break;
        case U'c':
            symbol = U'ㄕ';
            break;
        case U'd':
            symbol = U'ㄉ';
            break;
        case U'e':
            symbol = syllable.has_medial() ? U'ㄝ' : U'ㄧ';
            break;
        case U'f':
            symbol = U'ㄈ';
            break;
        case U'g':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄜ' : U'ㄍ';
            break;
        case U'h':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄛ' : U'ㄏ';
            break;
        case U'i':
            symbol = U'ㄞ';
            break;
        case U'j':
            symbol = U'ㄓ';
            break;
        case U'k':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄤ' : U'ㄎ';
            break;
        case U'l':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄥ' : U'ㄌ';
            break;
        case U'm':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄢ' : U'ㄇ';
            break;
        case U'n':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄣ' : U'ㄋ';
            break;
        case U'o':
            symbol = U'ㄡ';
            break;
        case U'p':
            symbol = U'ㄆ';
            break;
        case U'r':
            symbol = U'ㄖ';
            break;
        case U's':
            symbol = U'ㄙ';
            break;
        case U't':
            symbol = U'ㄊ';
            break;
        case U'u':
            symbol = U'ㄩ';
            break;
        case U'v':
            symbol = U'ㄔ';
            break;
        case U'w':
            symbol = U'ㄠ';
            break;
        case U'x':
            symbol = U'ㄨ';
            break;
        case U'y':
            symbol = U'ㄚ';
            break;
        case U'z':
            symbol = U'ㄗ';
            break;
        default:
            return {};
    }

    // Convert existing ㄍㄧ or ㄍㄩ to ㄐㄧ or ㄐㄩ before inserting the new symbol.
    if (syllable.initial() == U'ㄍ' && (syllable.medial() == U'ㄧ' || syllable.medial() == U'ㄩ')) {
        (void)syllable.overwrite(U'ㄐ');
    }

    // ㄐ/ㄑ/ㄒ must be followed by ㄧ or ㄩ; convert them to ㄓ/ㄔ/ㄕ when the
    // incoming symbol is ㄨ or a final without a medial.
    if (symbol == U'ㄨ' || (is_bopomofo_final(symbol) && !syllable.has_medial())) {
        switch (syllable.initial()) {
            case U'ㄐ':
                (void)syllable.overwrite(U'ㄓ');
                break;
            case U'ㄑ':
                (void)syllable.overwrite(U'ㄔ');
                break;
            case U'ㄒ':
                (void)syllable.overwrite(U'ㄕ');
                break;
            default:
                break;
        }
    }

    // Similarly, when ㄓ/ㄔ/ㄕ is followed by ㄧ or ㄩ, convert them to ㄐ/ㄑ/ㄒ.
    if (symbol == U'ㄧ' || symbol == U'ㄩ') {
        switch (syllable.initial()) {
            case U'ㄓ':
                (void)syllable.overwrite(U'ㄐ');
                break;
            case U'ㄔ':
                (void)syllable.overwrite(U'ㄑ');
                break;
            case U'ㄕ':
                (void)syllable.overwrite(U'ㄒ');
                break;
            default:
                break;
        }
    }

    (void)syllable.overwrite(symbol);
    BopomofoKeyResult result;
    if (syllable.complete()) {
        result.status = BopomofoKeyStatus::Completed;
        result.alternative_readings = hsu_alternative_readings(syllable);
    } else {
        result.status = BopomofoKeyStatus::Composing;
    }
    return result;
}

}  // namespace

std::optional<char32_t> lookup_bopomofo_key(char32_t key, bool accept_uppercase) {
    if (accept_uppercase) key = normalize_ascii_letter(key);
    static const std::unordered_map<char32_t, char32_t> map{
        {U'1', U'ㄅ'}, {U'2', U'ㄉ'}, {U'5', U'ㄓ'}, {U'8', U'ㄚ'}, {U'9', U'ㄞ'}, {U'0', U'ㄢ'}, {U'-', U'ㄦ'},
        {U'q', U'ㄆ'}, {U'w', U'ㄊ'}, {U'e', U'ㄍ'}, {U'r', U'ㄐ'}, {U't', U'ㄔ'}, {U'y', U'ㄗ'}, {U'u', U'ㄧ'}, {U'i', U'ㄛ'}, {U'o', U'ㄟ'}, {U'p', U'ㄣ'},
        {U'a', U'ㄇ'}, {U's', U'ㄋ'}, {U'd', U'ㄎ'}, {U'f', U'ㄑ'}, {U'g', U'ㄕ'}, {U'h', U'ㄘ'}, {U'j', U'ㄨ'}, {U'k', U'ㄜ'}, {U'l', U'ㄠ'}, {U';', U'ㄤ'},
        {U'z', U'ㄈ'}, {U'x', U'ㄌ'}, {U'c', U'ㄏ'}, {U'v', U'ㄒ'}, {U'b', U'ㄖ'}, {U'n', U'ㄙ'}, {U'm', U'ㄩ'}, {U',', U'ㄝ'}, {U'.', U'ㄡ'}, {U'/', U'ㄥ'},
        {U' ', U' '}, {U'6', U'ˊ'}, {U'3', U'ˇ'}, {U'4', U'ˋ'}, {U'7', U'˙'},
    };

    const auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

std::optional<char32_t> lookup_chewing_punctuation_key(char32_t key) {
    static const std::unordered_map<char32_t, char32_t> map{
        {U'[', U'「'}, {U']', U'」'}, {U'{', U'『'}, {U'}', U'』'}, {U'\'', U'、'}, {U'<', U'，'},
        {U':', U'：'}, {U'"', U'；'}, {U'>', U'。'}, {U'~', U'～'}, {U'!', U'！'}, {U'@', U'＠'},
        {U'#', U'＃'}, {U'$', U'＄'}, {U'%', U'％'}, {U'^', U'︿'}, {U'&', U'＆'}, {U'*', U'＊'},
        {U'(', U'（'}, {U')', U'）'}, {U'_', U'—'}, {U'+', U'＋'}, {U'=', U'＝'}, {U'\\', U'＼'},
        {U'|', U'｜'}, {U'?', U'？'}, {U',', U'，'}, {U'.', U'。'}, {U';', U'；'},
    };

    const auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

std::optional<char32_t> lookup_microsoft_ctrl_punctuation_key(char32_t key) {
    static const std::unordered_map<char32_t, char32_t> map{
        {U'!', U'！'}, {U'\'', U'、'}, {U',', U'，'}, {U'.', U'。'}, {U'/', U'？'}, {U';', U'；'},
    };

    const auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

BopomofoKeyResult apply_bopomofo_key(Syllable& syllable, BopomofoKeyboardLayout layout, char32_t key,
                                     bool accept_uppercase) {
    if (accept_uppercase) {
        key = normalize_ascii_letter(key);
    } else if (key >= U'A' && key <= U'Z') {
        return {};
    }

    Syllable candidate = syllable;
    BopomofoKeyResult result;
    if (layout == BopomofoKeyboardLayout::Hsu) {
        result = apply_hsu_key(candidate, key);
    } else {
        const auto symbol = lookup_bopomofo_key(key, accept_uppercase);
        if (!symbol) return {};
        if (!(candidate.accept(*symbol) || candidate.overwrite(*symbol))) return {};
        result.status = is_bopomofo_tone(*symbol) && candidate.complete()
                            ? BopomofoKeyStatus::Completed
                            : BopomofoKeyStatus::Composing;
    }

    if (result.status == BopomofoKeyStatus::Rejected) return result;
    syllable = std::move(candidate);
    return result;
}

}  // namespace ime::fcitx5
