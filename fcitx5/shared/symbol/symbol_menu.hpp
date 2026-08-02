#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ime::fcitx5 {

std::u32string_view mcbopomofo_symbols() noexcept;

class SymbolMenuState {
public:
    bool active() const noexcept;
    std::uint64_t epoch() const noexcept;
    bool matches(std::uint64_t epoch) const noexcept;
    void open() noexcept;
    void close() noexcept;

    // Current level's candidate items. The first level lists category labels and
    // direct symbols; the second level lists the symbols of the chosen category.
    const std::vector<std::u32string>& menu() const noexcept {
        return menu_;
    }
    // Selects the n-th item. Returns true and stores the chosen symbol when the
    // item is a symbol; returns false when the item is a category (the menu
    // descends to the category's symbol list).
    bool select(std::size_t n, char32_t& symbol) noexcept;
    // Returns true while the menu is showing a category's symbol list.
    bool in_category() const noexcept;
    // Returns to the first menu level; no-op when already there.
    void back() noexcept;

private:
    void rebuild_menu() noexcept;

    bool active_ = false;
    std::uint64_t epoch_ = 0;
    std::size_t category_ = static_cast<std::size_t>(-1);
    std::vector<std::u32string> menu_;
};

}  // namespace ime::fcitx5
