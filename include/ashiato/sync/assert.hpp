#pragma once

#include <cstdio>

#if defined(_MSC_VER)
#define ASHIATO_SYNC_BREAKPOINT() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
#define ASHIATO_SYNC_BREAKPOINT() __builtin_trap()
#else
#include <cassert>
#define ASHIATO_SYNC_BREAKPOINT() assert(false)
#endif

namespace ashiato::sync {

inline int default_assert_handler(const char* condition, const char* file_name, int line_number) {
    std::fprintf(stderr, "ASHIATO ASSERTION: %s, %s, line %d\n", condition, file_name, line_number);
    return 1;
}

inline int assert_failed(const char* condition, const char* file_name, int line_number) {
    const int result = default_assert_handler(condition, file_name, line_number);
    if (result != 0) {
        ASHIATO_SYNC_BREAKPOINT();
    }
    return result;
}

}  // namespace ashiato::sync

#if !defined(NDEBUG) || defined(ASHIATO_SYNC_ENABLE_ASSERT)
#define ASHIATO_SYNC_ASSERT(condition) \
    ((void)((!!(condition)) || (::ashiato::sync::assert_failed(#condition, __FILE__, static_cast<int>(__LINE__)), 0)))
#define ASHIATO_SYNC_ASSERT_FAIL(message) \
    ((void)::ashiato::sync::assert_failed((message), __FILE__, static_cast<int>(__LINE__)))
#else
#define ASHIATO_SYNC_ASSERT(...) ((void)0)
#define ASHIATO_SYNC_ASSERT_FAIL(...) ((void)0)
#endif
