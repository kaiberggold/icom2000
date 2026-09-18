#pragma once

// Deliberately not a third-party framework (Catch2/doctest/GTest): pulling
// one in via FetchContent means every test run needs network access on
// first configure, which this sketch avoids. Swapping this out for a real
// framework later is a one-file change -- everything else here just calls
// CHECK().
#include <iostream>

namespace icom::testing {

inline int& failure_count() {
    static int n = 0;
    return n;
}

inline void check(bool condition, const char* expr, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
        ++failure_count();
    }
}

} // namespace icom::testing

#define CHECK(expr) ::icom::testing::check((expr), #expr, __FILE__, __LINE__)
