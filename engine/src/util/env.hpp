#pragma once

#include <cstdlib>

namespace llavon::ime {

// Reads the environment variable `name`, falling back to the pre-rename
// `legacy` name so existing environments keep working.
inline const char* env_with_legacy(const char* name, const char* legacy) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') return value;
    if (const char* value = std::getenv(legacy); value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

}  // namespace llavon::ime
