#include "bopomofo/keymap.hpp"

#include <unordered_map>

namespace ime::fcitx5 {

namespace {

char32_t normalize_ascii_letter(char32_t key) {
    if (key >= U'A' && key <= U'Z') return key + (U'a' - U'A');
    return key;
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

std::optional<char32_t> lookup_microsoft_punctuation_key(char32_t key) {
    static const std::unordered_map<char32_t, char32_t> map{
        {U'!', U'！'}, {U'"', U'；'}, {U'\'', U'、'}, {U':', U'：'}, {U'<', U'，'}, {U'>', U'。'}, {U'?', U'？'},
        {U'[', U'「'},  {U']', U'」'},  {U'\\', U'＼'}, {U'`', U'‘'},  {U'(', U'（'}, {U')', U'）'},
        {U'*', U'＊'},  {U'+', U'＋'}, {U'=', U'＝'}, {U'#', U'＃'}, {U'$', U'＄'}, {U'%', U'％'},
        {U'&', U'＆'},  {U'@', U'＠'}, {U'^', U'︿'}, {U'_', U'—'},
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

}  // namespace ime::fcitx5
