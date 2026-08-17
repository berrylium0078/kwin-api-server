// SPDX-License-Identifier: MIT
// Shared harness macros for the unit tests (see test_main.cpp for main()).

#pragma once

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace kas_test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

} // namespace kas_test

#define TEST(name)                                                                             \
    static void test_##name();                                                                 \
    static ::kas_test::Registrar reg_##name(#name, test_##name);                               \
    static void test_##name()

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);     \
            throw std::runtime_error(std::string("CHECK failed: ") + #cond);                   \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        auto va = (a);                                                                         \
        auto vb = (b);                                                                         \
        if (!(va == vb)) {                                                                     \
            std::fprintf(stderr, "CHECK_EQ failed: %s == %s (%s:%d)\n", #a, #b, __FILE__,      \
                         __LINE__);                                                            \
            throw std::runtime_error(std::string("CHECK_EQ failed: ") + #a + " == " + #b);     \
        }                                                                                      \
    } while (0)
