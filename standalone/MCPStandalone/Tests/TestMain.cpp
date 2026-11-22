#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> run;
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int& FailureCount() {
    static int failures = 0;
    return failures;
}

void Fail(const char* file, int line, const std::string& message) {
    ++FailureCount();
    std::cerr << file << ':' << line << ": " << message << '\n';
}

}  // namespace test

int main() {
    int ran = 0;
    for (const auto& test : test::Registry()) {
        ++ran;
        try {
            test.run();
        } catch (const std::exception& error) {
            test::Fail(__FILE__, __LINE__,
                       test.name + " threw: " + error.what());
        } catch (...) {
            test::Fail(__FILE__, __LINE__,
                       test.name + " threw an unknown exception");
        }
    }

    if (test::FailureCount() != 0) {
        std::cerr << test::FailureCount() << " failure(s) across " << ran
                  << " test(s)\n";
        return 1;
    }

    std::cout << ran << " test(s) passed\n";
    return 0;
}
