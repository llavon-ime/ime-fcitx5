#pragma once

#include <cstdarg>
#include <cstdio>

// Debug logging is compiled in only with -DLLAVON_IME_DEBUG=1 (the
// LLAVON_IME_DEBUG CMake option). Disabled calls discard their arguments, so
// call sites can format expensive previews inline.
#ifdef LLAVON_IME_DEBUG
#define LLAVON_DEBUG_LOG(tag, ...) ::llavon::ime::debug_log(tag, __VA_ARGS__)
#else
#define LLAVON_DEBUG_LOG(tag, ...) ((void)0)
#endif

namespace llavon::ime {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
inline void debug_log(const char* tag, const char* format, ...) {
    std::fprintf(stderr, "[%s] ", tag);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

}  // namespace llavon::ime
