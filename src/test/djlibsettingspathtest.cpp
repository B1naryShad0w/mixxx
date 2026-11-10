#include <gtest/gtest.h>

#include <QDir>
#include <QStandardPaths>

#include "util/cmdlineargs.h"

namespace {

// The djlib fork must default to its own settings dir so it runs side by side
// with stock Mixxx without touching its settings, library DB or mappings.
TEST(DjlibSettingsPathTest, defaultIsSideBySideWithStockMixxx) {
    const CmdlineArgs args;
    const QString path = args.getSettingsPath();
    EXPECT_FALSE(args.getSettingsPathSet());
#if defined(__LINUX__)
    EXPECT_EQ(QDir::homePath() + QStringLiteral("/.mixxx-djlib/"), path);
#elif defined(__WINDOWS__) || defined(__APPLE__)
    // %LOCALAPPDATA%\Mixxx-djlib\ on Windows,
    // ~/Library/Application Support/Mixxx-djlib/ on macOS.
    EXPECT_EQ(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                    QStringLiteral("/Mixxx-djlib/"),
            path);
#endif
    EXPECT_NE(QDir::homePath() + QStringLiteral("/.mixxx/"), path);
    EXPECT_FALSE(path.endsWith(QStringLiteral("/Mixxx/")));
}

} // namespace
