#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "rtc/global.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
	timeBeginPeriod(1);
#endif

    // Should be sufficient for unit tests.
    rtc::SetThreadPoolSize(1);

    ::testing::InitGoogleTest(&argc, argv);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    int result = RUN_ALL_TESTS();

#ifdef _WIN32
	timeEndPeriod(1);
#endif

    return result;
}
