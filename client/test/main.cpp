#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifdef USE_QT
#include <QCoreApplication>
#include <QTimer>

int main(int argc, char* argv[])
{
    int qt_argc = 0;
    QCoreApplication app(qt_argc, nullptr);
    QTimer::singleShot(0, [&]() {
        ::testing::InitGoogleTest(&argc, argv);
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        auto testResult = RUN_ALL_TESTS();
        app.exit(testResult);
    });
    return app.exec();
}
#endif
