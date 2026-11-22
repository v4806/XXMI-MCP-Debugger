#pragma once

#include <functional>
#include <string>
#include <vector>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> run;
};

std::vector<TestCase>& Registry();
int& FailureCount();
void Fail(const char* file, int line, const std::string& message);

}  // namespace test
