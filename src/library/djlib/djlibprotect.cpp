#include "library/djlib/djlibprotect.h"

#include <QDir>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QWriteLocker>

namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kCaseSensitivity = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kCaseSensitivity = Qt::CaseSensitive;
#endif

Q_GLOBAL_STATIC(QReadWriteLock, s_lock)
Q_GLOBAL_STATIC(QStringList, s_prefixes) // cleaned, with a trailing '/'

} // namespace

void djlib::setProtectedRoots(const QStringList& roots) {
    QStringList prefixes;
    for (const auto& root : roots) {
        if (root.isEmpty()) {
            continue;
        }
        QString prefix = QDir::cleanPath(root);
        if (!prefix.endsWith(QLatin1Char('/'))) {
            prefix += QLatin1Char('/');
        }
        if (!prefixes.contains(prefix)) {
            prefixes.append(prefix);
        }
    }
    const QWriteLocker locker(s_lock());
    *s_prefixes() = prefixes;
}

QStringList djlib::protectedRoots() {
    const QReadLocker locker(s_lock());
    return *s_prefixes();
}

bool djlib::isProtectedFile(const QString& location) {
    if (location.isEmpty()) {
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
