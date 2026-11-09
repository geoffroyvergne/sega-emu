#pragma once

// Minimal dependency-free test framework. Swap for Catch2/GoogleTest once tests grow.

#include <cstdio>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

} // namespace test

#define TEST(name)                                              \
    static void name();                                         \
    static const ::test::Registrar name##_registrar{#name, name}; \
    static void name()

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",               \
                         __FILE__, __LINE__, #expr);                        \
            ++::test::failures();                                           \
        }                                                                   \
    } while (false)
