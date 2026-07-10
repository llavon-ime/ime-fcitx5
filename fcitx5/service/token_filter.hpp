#pragma once

#include <cstddef>
#include <vector>

namespace ime::fcitx5 {

inline std::size_t remove_leading_unknown_context_tokens(std::vector<int>& context_tokens, int unknown_token) {
    std::size_t first_kept = 0;
    while (first_kept < context_tokens.size() && context_tokens[first_kept] == unknown_token) ++first_kept;
    if (first_kept == 0) return 0;

    context_tokens.erase(context_tokens.begin(), context_tokens.begin() + static_cast<std::ptrdiff_t>(first_kept));
    return first_kept;
}

}  // namespace ime::fcitx5
