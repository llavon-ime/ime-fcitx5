#include "symbol/symbol_menu.hpp"

namespace ime::fcitx5 {

namespace {

// Ordering adapted from McBopomofo's MIT-licensed _punctuation_list data.
// See packaging/licenses/MCBOPOMOFO_NOTICE.txt for attribution.
constexpr std::u32string_view kMcBopomofoSymbols =
    U"　，、。．；：？！︰?⋯‥…｜—︴﹏（）︵︶《》︽︾〈〉︿﹀【】︻︼｛｝︷︸〔〕︹︺「」﹁﹂『』﹃﹄＃＆＊※§〃○●◎㊣△▽▲▼∴∵☆★◇◆□■♀♂→←↑↓↗↖↙↘㏕㎜㎝㎞㏎㎡㎎㎏㏄℃℉°±×÷≒≠≦≧∼∠⊥∟≡⊿∞√┌┬┐├┼┤└┴┘─│═╞╪╡╱╲╳╭╮╰╯▁▂▃▄▅▆▇█▏▎▍▌▋▊▉▔〡〢〣〤〥〦〧〨〩十卄卅ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩαβγδεζηθικλμνξοπρστυφχψω";

// Category layout adapted from libchewing's symbols.dat. A category with an
// empty symbol list is a direct symbol entry on the first menu level.
struct SymbolCategory {
    std::u32string_view label;
    std::u32string_view symbols;
};

constexpr SymbolCategory kCategories[] = {
    {U"…", U""},
    {U"※", U""},
    {U"常用符號", U"，、。．？！；：‧‥﹐﹒˙·‘’“”〝〞‵′〃～＄％＠＆＃＊"},
    {U"左右括號", U"（）「」〔〕｛｝〈〉『』《》【】﹙﹚﹝﹞﹛﹜"},
    {U"上下括號", U"︵︶﹁﹂︹︺︷︸︿﹀﹃﹄︽︾︻︼"},
    {U"希臘字母", U"αβγδεζηθικλμνξοπρστυφχψωΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ"},
    {U"數學符號", U"＋－×÷＝≠≒∞±√＜＞﹤﹥≦≧∩∪ˇ⊥∠∟⊿㏒㏑∫∮∵∴╳﹢"},
    {U"特殊圖形", U"↑↓←→↖↗↙↘㊣◎○●⊕⊙○●△▲☆★◇◆□■▽▼§￥〒￠￡※♀♂"},
    {U"Unicode", U"♨☀☁☂☃♠♥♣♦♩♪♫♬☺☻"},
    {U"單線框", U"├─┼┴┬┤┌┐╞═╪╡│▕└┘╭╮╰╯"},
    {U"雙線框", U"╔╦╗╠═╬╣╓╥╖╒╤╕║╚╩╝╟╫╢╙╨╜╞╪╡╘╧╛"},
    {U"填色方塊", U"＿ˍ▁▂▃▄▅▆▇█▏▎▍▌▋▊▉◢◣◥◤"},
    {U"線段", U"﹣﹦≡｜∣∥–︱—︳╴¯￣﹉﹊﹍﹎﹋﹌﹏︴∕﹨╱╲／＼"},
};

}  // namespace

std::u32string_view mcbopomofo_symbols() noexcept {
    return kMcBopomofoSymbols;
}

bool SymbolMenuState::active() const noexcept {
    return active_;
}

std::uint64_t SymbolMenuState::epoch() const noexcept {
    return epoch_;
}

bool SymbolMenuState::matches(std::uint64_t epoch) const noexcept {
    return active_ && epoch_ == epoch;
}

void SymbolMenuState::open() noexcept {
    active_ = true;
    ++epoch_;
    category_ = static_cast<std::size_t>(-1);
    rebuild_menu();
}

void SymbolMenuState::close() noexcept {
    if (!active_) return;
    active_ = false;
    ++epoch_;
    category_ = static_cast<std::size_t>(-1);
    menu_.clear();
}

void SymbolMenuState::rebuild_menu() noexcept {
    menu_.clear();
    if (category_ == static_cast<std::size_t>(-1)) {
        for (const auto& category : kCategories) {
            menu_.emplace_back(category.label);
        }
        return;
    }
    for (const char32_t symbol : kCategories[category_].symbols) {
        menu_.emplace_back(1, symbol);
    }
}

bool SymbolMenuState::in_category() const noexcept {
    return category_ != static_cast<std::size_t>(-1);
}

void SymbolMenuState::back() noexcept {
    if (category_ == static_cast<std::size_t>(-1)) return;
    category_ = static_cast<std::size_t>(-1);
    rebuild_menu();
}

bool SymbolMenuState::select(std::size_t n, char32_t& symbol) noexcept {
    if (!active_ || n >= menu_.size()) return false;

    if (category_ == static_cast<std::size_t>(-1)) {
        if (n >= std::size(kCategories)) return false;
        if (!kCategories[n].symbols.empty()) {
            category_ = n;
            rebuild_menu();
            return false;
        }
        symbol = kCategories[n].label.front();
        return true;
    }

    symbol = menu_[n].front();
    return true;
}

}  // namespace ime::fcitx5
