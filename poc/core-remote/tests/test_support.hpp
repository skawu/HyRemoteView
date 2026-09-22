#pragma once

// POC_ONLY test support: a deliberate minimum, so the harness introduces no test-framework
// dependency of its own.

#include <iostream>
#include <string>

namespace poc_test {

inline int failures = 0;

inline void report(const char *file, int line, const char *text)
{
    ++failures;
    std::cerr << file << ":" << line << ": CHECK failed: " << text << "\n";
}

inline int finish(const char *name)
{
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed: " << name << "\n";
        return 1;
    }
    std::cout << "PASS: " << name << "\n";
    return 0;
}

}  // namespace poc_test

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            poc_test::report(__FILE__, __LINE__, #expr);                                           \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const auto pocActual = (actual);                                                           \
        const auto pocExpected = (expected);                                                       \
        if (!(pocActual == pocExpected))                                                           \
            poc_test::report(__FILE__, __LINE__, #actual " == " #expected);                         \
    } while (false)
