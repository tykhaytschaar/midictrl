#pragma once

// Tiny assertion / runner macros for host-side StateMachine tests. Kept in
// the repo rather than pulling in Unity / Catch / etc. — we have at most
// a few dozen tests and they all live in one binary, so the surface needed
// is small.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

extern int g_tests_total;
extern int g_tests_passed;
extern int g_tests_failed;
extern bool g_current_test_failed;

#define TEST_FAIL(fmt, ...) do { \
    fprintf(stderr, "  FAIL at %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    g_current_test_failed = true; \
} while (0)

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { TEST_FAIL("expected: %s", #cond); return; } \
} while (0)

#define TEST_ASSERT_TRUE(cond)  TEST_ASSERT(cond)
#define TEST_ASSERT_FALSE(cond) TEST_ASSERT(!(cond))

#define TEST_ASSERT_EQ_INT(actual, expected) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a != _e) { TEST_FAIL("%s: expected %lld, got %lld", #actual, _e, _a); return; } \
} while (0)

#define RUN_TEST(fn) do { \
    g_current_test_failed = false; \
    fn(); \
    g_tests_total++; \
    if (g_current_test_failed) { printf("FAIL %s\n", #fn); g_tests_failed++; } \
    else { printf("PASS %s\n", #fn); g_tests_passed++; } \
} while (0)

#define TEST_SUMMARY_AND_EXIT() do { \
    printf("\n%d total, %d passed, %d failed\n", g_tests_total, g_tests_passed, g_tests_failed); \
    return g_tests_failed == 0 ? 0 : 1; \
} while (0)
