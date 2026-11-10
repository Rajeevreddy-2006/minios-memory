// Tiny assertion harness so the project has zero external dependencies.
#pragma once
#include <cstdio>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failed++;                                                      \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        g_checks++;                                                          \
        auto _a = (a); auto _b = (b);                                        \
        if (!(_a == _b)) {                                                   \
            g_failed++;                                                      \
            std::printf("  FAIL %s:%d  %s == %s  (got %lld vs %lld)\n",      \
                        __FILE__, __LINE__, #a, #b,                          \
                        (long long)_a, (long long)_b);                       \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        int _before = g_failed;                                              \
        fn();                                                                \
        std::printf("[%s] %s\n", g_failed == _before ? " ok " : "FAIL", #fn);\
    } while (0)

#define TEST_SUMMARY()                                                       \
    (std::printf("%d checks, %d failed\n", g_checks, g_failed),              \
     g_failed ? 1 : 0)
