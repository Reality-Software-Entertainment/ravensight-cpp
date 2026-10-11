// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Minimal test runner: RS_TEST registers a case, CHECK / CHECK_EQ record
// failures without aborting the case, REQUIRE bails out of the case.

#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace rstest {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

/// Run after every test case (for example to delete temporary directories).
inline std::vector<void (*)()>& after_each_hooks() {
    static std::vector<void (*)()> hooks;
    return hooks;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline int& assertion_count() {
    static int count = 0;
    return count;
}

inline bool check(bool condition, const char* expression, const char* file, int line) {
    ++assertion_count();
    if (!condition) {
        ++failure_count();
        std::printf("  FAILED %s:%d  CHECK(%s)\n", file, line, expression);
    }
    return condition;
}

template <typename T>
std::string stringify(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

template <typename A, typename B>
bool check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++assertion_count();
    if (!(a == b)) {
        ++failure_count();
        std::printf("  FAILED %s:%d  CHECK_EQ(%s, %s)\n    left:  %s\n    right: %s\n", file,
                    line, ea, eb, stringify(a).c_str(), stringify(b).c_str());
        return false;
    }
    return true;
}

inline int run_all() {
    int ran = 0;
    for (const TestCase& test : registry()) {
        const int failures_before = failure_count();
        std::printf("[ RUN  ] %s\n", test.name);
        test.fn();
        for (void (*hook)() : after_each_hooks()) {
            hook();
        }
        ++ran;
        if (failure_count() == failures_before) {
            std::printf("[  OK  ] %s\n", test.name);
        } else {
            std::printf("[ FAIL ] %s\n", test.name);
        }
    }
    std::printf("\n%d test(s), %d assertion(s), %d failure(s)\n", ran, assertion_count(),
                failure_count());
    return failure_count() == 0 ? 0 : 1;
}

} // namespace rstest

#define RS_TEST(name)                                                    \
    static void rs_test_##name();                                        \
    static ::rstest::Registrar rs_registrar_##name(#name, &rs_test_##name); \
    static void rs_test_##name()

#define CHECK(condition) ::rstest::check((condition), #condition, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::rstest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define REQUIRE(condition)                                                          \
    do {                                                                            \
        if (!::rstest::check((condition), #condition, __FILE__, __LINE__)) return;  \
    } while (0)
