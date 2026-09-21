#pragma once

#include <cstdint>

namespace llavon::ime {

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

// Modifiers that turn a key into a shortcut: the input rules hand the key back
// to the application instead of composing. Meta is deliberately absent because
// the fcitx5 adapter merges the raw Meta bit into the effective states.
constexpr std::uint32_t kShortcutModifierMask =
    input_key_state(InputKeyState::Shift) | input_key_state(InputKeyState::Ctrl) |
    input_key_state(InputKeyState::Alt) | input_key_state(InputKeyState::Hyper) |
    input_key_state(InputKeyState::Super) | input_key_state(InputKeyState::Super2);

// Keysyms fcitx5 folds Shift out of because their shifted form is already in
// the keysym: printable keys (Key::keySymToUnicode maps them) plus BackSpace,
// Clear, Escape, Delete and the keypad keys. Arrows, Home/End/Page, Insert and
// Space/Return/Tab keep the state, which the marking and Shift+space rules
// read.
constexpr bool shift_folds_into_keysym(char32_t symbol) {
    if (symbol >= U'!' && symbol <= U'~') return true;
    if (symbol >= 0xa0 && symbol <= 0xff) return true;
    switch (symbol) {
        case 0xff08:  // BackSpace
        case 0xff0b:  // Clear
        case 0xff1b:  // Escape
        case 0xffff:  // Delete
        case 0xff8d:  // KP_Enter
        case 0xffbd:  // KP_Equal
            return true;
        default:
            break;
    }
    return symbol >= 0xffaa && symbol <= 0xffb9;  // keypad operators and digits
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
    // Modifier bits of the frontend's own key event (fcitx5's normalized key,
    // or AppKit's event flags), kept for hosts and diagnostics. The rules use
    // the effective states above.
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

    constexpr bool raw_has(InputKeyState state) const {
        return (raw_states & static_cast<std::uint32_t>(state)) != 0;
    }

    // Modifiers that hand the key back to the application. Checked on the
    // effective states, so a Shift that normalize_key() folded into the keysym
    // no longer counts; the raw Meta bit the fcitx5 adapter merges into the
    // states is ignored on purpose.
    constexpr bool has_shortcut_modifier() const {
        return (states & kShortcutModifierMask) != 0;
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

// Frontends report a Shift-modified key in one of two shapes: fcitx5 folds
// Shift into the keysym and drops the state (Key::normalize), while a host that
// only reports modifier bits leaves the keysym unshifted. Normalize both to the
// folded shape so the input rules stay frontend-agnostic: a plain Shift+ASCII
// letter or symbol becomes the shifted keysym with the Shift bit removed from
// the effective states. Shift combined with Ctrl/Alt/Super stays a shortcut,
// keys the shifted form already covers (printables, BackSpace, Escape, Delete,
// keypad) drop the state, and keys the rules still need it for (arrows, Space,
// Return, Tab) keep it.
//
// CapsLock is the caller's responsibility. X11 applies CapsLock to letter
// keysyms (Shift and CapsLock cancel each other); AppKit's
// charactersIgnoringModifiers already applies Shift but never CapsLock, so the
// macOS adapter folds CapsLock in before sending the key.
constexpr InputKey normalize_key(const InputKey& key) {
    constexpr std::uint32_t kCommandMask =
        input_key_state(InputKeyState::Ctrl) | input_key_state(InputKeyState::Alt) |
        input_key_state(InputKeyState::Hyper) | input_key_state(InputKeyState::Super) |
        input_key_state(InputKeyState::Super2) | input_key_state(InputKeyState::Meta);

    InputKey normalized = key;
    if ((normalized.states & input_key_state(InputKeyState::Shift)) == 0) return normalized;
    if ((normalized.states & kCommandMask) != 0) return normalized;

    const char32_t sym = normalized.sym;
    if (sym >= U'a' && sym <= U'z') {
        normalized.sym = sym - (U'a' - U'A');
    } else if (const char32_t shifted = shifted_ascii_symbol(sym); shifted != sym) {
        normalized.sym = shifted;
    } else if (!(sym >= U'A' && sym <= U'Z') && !is_shifted_ascii_symbol(sym) &&
               !shift_folds_into_keysym(sym)) {
        // No shifted form and no fcitx5 folding rule: keep the state for the
        // rules that need it (arrows, Space, Return, Tab, Home/End/Page, ...).
        return normalized;
    }
    normalized.states &= ~input_key_state(InputKeyState::Shift);
    return normalized;
}

}  // namespace llavon::ime
