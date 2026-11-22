#pragma once

#include <iostream>
#include <string>

#include "TestMain.h"

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static const bool name##_registered = [] {                                 \
        test::Registry().push_back({#name, &name});                            \
        return true;                                                           \
    }();                                                                       \
    static void name()

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            test::Fail(__FILE__, __LINE__, "REQUIRE failed: " #condition);     \
            return;                                                            \
        }                                                                      \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                           \
    do {                                                                       \
        const auto& actual_value = (actual);                                   \
        const auto& expected_value = (expected);                               \
        if (!(actual_value == expected_value)) {                               \
            test::Fail(__FILE__, __LINE__, "REQUIRE_EQ failed: " #actual);     \
            return;                                                            \
        }                                                                      \
    } while (false)
