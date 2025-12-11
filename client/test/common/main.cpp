#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "rtc/global.hpp"

int main(int argc, char* argv[])
{
    // Should be sufficient for unit tests.
    rtc::SetThreadPoolSize(1);

    ::testing::InitGoogleTest(&argc, argv);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    return RUN_ALL_TESTS();
}
