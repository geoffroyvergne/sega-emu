#include "test.hpp"

int main() {
    // Unbuffered, and each test announced before it runs: if a test crashes,
    // the last line printed names it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (const auto& c : test::registry()) {
        std::printf("[ .. ] %s\r", c.name);
        const int before = test::failures();
        c.fn();
        std::printf("[%s] %s\n", test::failures() == before ? " OK " : "FAIL", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", test::registry().size(), test::failures());
    return test::failures() == 0 ? 0 : 1;
}
