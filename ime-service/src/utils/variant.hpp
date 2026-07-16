#pragma once
#include <variant>

namespace imesvc {
// template <typename... Ts>
// struct variant : Ts... {
//     std::variant<Ts...> var;
//     template <typename T>
//     variant(T&& value) : Ts(std::forward<T>(value))..., var(std::forward<T>(value)) {}
// };
}  // namespace imesvc