// SPDX-License-Identifier: MIT
// Minimal test harness: TEST(name) registers a case, CHECK(cond) fails it.
// The binary exits non-zero if any case fails and prints
// "ALL TESTS PASSED (N)" on success (xmake's add_tests matches on that).

#include "test_main.hpp"

#include <cstdio>

namespace kas_test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

} // namespace kas_test

int main() {
    int failed = 0;
    for (const auto& test : kas_test::registry()) {
        try {
            test.fn();
            std::printf("[PASS] %s\n", test.name);
        } catch (const std::exception& e) {
            std::printf("[FAIL] %s: %s\n", test.name, e.what());
            ++failed;
        }
    }
    if (failed > 0) {
        std::printf("FAILED: %d of %zu test(s)\n", failed, kas_test::registry().size());
        return 1;
    }
    std::printf("ALL TESTS PASSED (%zu)\n", kas_test::registry().size());
    return 0;
}
