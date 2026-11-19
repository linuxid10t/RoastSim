// Minimal test harness (no external framework, so it builds anywhere).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rstest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline std::string& dataDir() {
    static std::string d = "data";
    return d;
}

struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

}  // namespace rstest

#define RS_CAT2(a, b) a##b
#define RS_CAT(a, b) RS_CAT2(a, b)
#define TEST(name)                                                 \
    static void name();                                            \
    static rstest::Reg RS_CAT(reg_, name)(#name, name);            \
    static void name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++rstest::failures();                                                         \
        }                                                                                 \
    } while (0)

#define CHECK_RANGE(v, lo, hi)                                                                            \
    do {                                                                                                  \
        double _v = (v);                                                                                  \
        if (!(_v >= (lo) && _v <= (hi))) {                                                                \
            std::printf("    FAIL %s:%d: %s = %.3f not in [%g, %g]\n", __FILE__, __LINE__, #v, _v,        \
                        static_cast<double>(lo), static_cast<double>(hi));                                \
            ++rstest::failures();                                                                         \
        }                                                                                                 \
    } while (0)
