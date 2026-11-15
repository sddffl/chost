// A small check helper for chost's C++ tests: CHECK(condition, printf-style message) prints PASS or FAIL,
// evaluating the condition once, and failures() is what main returns.

#pragma once

#include <cstdio>

namespace check {
inline int& count()
{
    static int failures = 0;
    return failures;
}
inline int failures()
{
    std::printf("%s (%d failure%s)\n", count() ? "FAILED" : "ALL PASSED", count(), count() == 1 ? "" : "s");
    return count() ? 1 : 0;
}
} // namespace check

#define CHECK(cond, ...)                                       \
    do                                                         \
    {                                                          \
        const bool ok_ = (cond);                               \
        std::printf("  %s  ", ok_ ? "PASS" : "FAIL");          \
        std::printf(__VA_ARGS__);                              \
        std::printf("\n");                                     \
        if (!ok_)                                              \
            ++check::count();                                  \
    } while (0)
