#pragma once

#include <cstdint>

namespace ime::fcitx5 {

// X11 keysyms the input rules route on.
namespace keysym {
inline constexpr char32_t BackSpace = 0xff08;
inline constexpr char32_t Tab = 0xff09;
inline constexpr char32_t Return = 0xff0d;
inline constexpr char32_t Escape = 0xff1b;
inline constexpr char32_t Home = 0xff50;
inline constexpr char32_t Left = 0xff51;
inline constexpr char32_t Up = 0xff52;
inline constexpr char32_t Right = 0xff53;
inline constexpr char32_t Down = 0xff54;
inline constexpr char32_t Page_Up = 0xff55;
inline constexpr char32_t Page_Down = 0xff56;
inline constexpr char32_t End = 0xff57;
inline constexpr char32_t Insert = 0xff63;
inline constexpr char32_t Delete = 0xffff;
inline constexpr char32_t grave = 0x60;
}  // namespace keysym

// Lowercases an ASCII letter; every other character passes through.
constexpr char32_t ascii_lower(char32_t value) {
    return value >= U'A' && value <= U'Z' ? value + (U'a' - U'A') : value;
}

// Modifier bits with the same numeric values as X11 key states, so a frontend
// adapter can copy its own state bits through without translation.
enum class InputKeyState : std::uint32_t {
    None = 0,
    Shift = 1U << 0,
    CapsLock = 1U << 1,
    Ctrl = 1U << 2,
    Alt = 1U << 3,
    Hyper = 1U << 5,
    Super = 1U << 6,
    Super2 = 1U << 26,
    Meta = 1U << 28,
};

constexpr std::uint32_t operator|(InputKeyState lhs, InputKeyState rhs) {
    return static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs);
}

constexpr std::uint32_t operator|(std::uint32_t lhs, InputKeyState rhs) {
    return lhs | static_cast<std::uint32_t>(rhs);
}

constexpr std::uint32_t operator|(InputKeyState lhs, std::uint32_t rhs) {
    return static_cast<std::uint32_t>(lhs) | rhs;
}

// Storage conversion for a single state bit; combinations can use operator|.
constexpr std::uint32_t input_key_state(InputKeyState state) {
    return static_cast<std::uint32_t>(state);
}

// Maps a US-layout symbol keysym to the character Shift produces for it.
// Frontends may fold Shift into the keysym and clear the state, so keysyms
// that are already shifted symbols pass through unchanged.
constexpr char32_t shifted_ascii_symbol(char32_t symbol) {
    switch (symbol) {
        case U'1':
            return U'!';
        case U'2':
            return U'@';
        case U'3':
            return U'#';
        case U'4':
            return U'$';
        case U'5':
            return U'%';
        case U'6':
            return U'^';
        case U'7':
            return U'&';
        case U'8':
            return U'*';
        case U'9':
            return U'(';
        case U'0':
            return U')';
        case U'-':
            return U'_';
        case U'=':
            return U'+';
        case U'[':
            return U'{';
        case U']':
            return U'}';
        case U'\\':
            return U'|';
        case U';':
            return U':';
        case U'\'':
            return U'"';
        case U',':
            return U'<';
        case U'.':
            return U'>';
        case U'/':
            return U'?';
        case U'`':
            return U'~';
        default:
            return symbol;
    }
}

// True when the keysym is the shifted form of a US symbol key, which means the
// Shift state was folded into the keysym before the key reached the engine.
constexpr bool is_shifted_ascii_symbol(char32_t symbol) {
    switch (symbol) {
        case U'!':
        case U'@':
        case U'#':
        case U'$':
        case U'%':
        case U'^':
        case U'&':
        case U'*':
        case U'(':
        case U')':
        case U'_':
        case U'+':
        case U'{':
        case U'}':
        case U'|':
        case U':':
        case U'"':
        case U'<':
        case U'>':
        case U'?':
        case U'~':
            return true;
        default:
            return false;
    }
}

// One key press as seen by the input rules, independent of the frontend.
struct InputKey {
    // Effective keysym after frontend normalization.
    char32_t sym = 0;
    // Effective modifier bits; see InputKeyState.
    std::uint32_t states = 0;
    // Modifier bits exactly as the frontend reported them, before the frontend
    // adapter merged the raw Meta bit into the effective states.
    std::uint32_t frontend_states = 0;
    // Modifier bits of the raw key event, which still carries CapsLock and any
    // modifier the normalization moved into the keysym.
    std::uint32_t raw_states = 0;
    // CapsLock lives on the raw frontend key and is reported separately
    // because the normalized states drop it.
    bool caps_lock = false;
    bool release = false;

    constexpr bool has(InputKeyState state) const {
        return (states & static_cast<std::uint32_t>(state)) != 0;
    }

    constexpr bool frontend_has(InputKeyState state) const {
        return (frontend_states & static_cast<std::uint32_t>(state)) != 0;
    }

    constexpr bool raw_has(InputKeyState state) const {
        return (raw_states & static_cast<std::uint32_t>(state)) != 0;
    }

    // Blocking modifiers as reported by the frontend key, ignoring the raw
    // Meta bit the effective states carry.
    constexpr bool frontend_blocking_modifier() const {
        return (frontend_states & (InputKeyState::Shift | InputKeyState::Ctrl | InputKeyState::Alt |
                                   InputKeyState::Hyper | InputKeyState::Super | InputKeyState::Super2 |
                                   InputKeyState::Meta)) != 0;
    }

    // Shift is held, or the keysym already carries the folded shifted symbol.
    constexpr bool shifted() const { return has(InputKeyState::Shift) || is_shifted_ascii_symbol(sym); }

    // Modifiers that hand the key back to the application. CapsLock and
    // NumLock are not blocking for text input.
    constexpr bool has_blocking_modifier() const {
        return (states & (InputKeyState::Shift | InputKeyState::Ctrl | InputKeyState::Alt |
                          InputKeyState::Hyper | InputKeyState::Super | InputKeyState::Super2 |
                          InputKeyState::Meta)) != 0;
    }
};

}  // namespace ime::fcitx5
