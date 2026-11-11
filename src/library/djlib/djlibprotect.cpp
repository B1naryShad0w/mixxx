#include "library/djlib/djlibprotect.h"

#include <QDir>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QWriteLocker>
#include <atomic>

namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kCaseSensitivity = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kCaseSensitivity = Qt::CaseSensitive;
#endif

Q_GLOBAL_STATIC(QReadWriteLock, s_lock)
Q_GLOBAL_STATIC(QStringList, s_prefixes) // cleaned, with a trailing '/'
std::atomic<int> s_count{0};             // size of s_prefixes

} // namespace

void djlib::addProtectedRoot(const QString& root) {
    if (root.isEmpty()) {
        return;
    }
    QString prefix = QDir::cleanPath(root);
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }
    const QWriteLocker locker(s_lock());
    if (!s_prefixes()->contains(prefix, kCaseSensitivity)) {
        s_prefixes()->append(prefix);
        s_count.store(static_cast<int>(s_prefixes()->size()));
    }
}

void djlib::resetProtectedRootsForTesting() {
    const QWriteLocker locker(s_lock());
    s_prefixes()->clear();
    s_count.store(0);
}

bool djlib::hasProtectedRoots() {
    return s_count.load() > 0;
}

QStringList djlib::protectedRoots() {
    const QReadLocker locker(s_lock());
    return *s_prefixes();
}

bool djlib::isProtectedFile(const QString& location) {
    if (location.isEmpty() || !hasProtectedRoots()) {
        return false;
    }
    const QString path = QDir::cleanPath(location);
    const QReadLocker locker(s_lock());
    for (const auto& prefix : std::as_const(*s_prefixes())) {
        if (path.startsWith(prefix, kCaseSensitivity)) {
            return true;
        }
    }
    return false;
}
