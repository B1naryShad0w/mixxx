#include "library/djlib/djlibprotect.h"

#include <QDir>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QWriteLocker>
#include <atomic>

namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr bool kCaseInsensitive = true;
#else
constexpr bool kCaseInsensitive = false;
#endif
std::atomic<bool> s_caseInsensitive{kCaseInsensitive};

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
    if (!s_prefixes()->contains(prefix, pathCaseSensitivity())) {
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
        if (path.startsWith(prefix, pathCaseSensitivity())) {
            return true;
        }
    }
    return false;
}

Qt::CaseSensitivity djlib::pathCaseSensitivity() {
    return s_caseInsensitive.load() ? Qt::CaseInsensitive : Qt::CaseSensitive;
}

bool djlib::samePath(const QString& a, const QString& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return a.isEmpty() && b.isEmpty();
    }
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), pathCaseSensitivity()) == 0;
}

void djlib::setPathCaseSensitivityForTesting(
        std::optional<Qt::CaseSensitivity> caseSensitivity) {
    s_caseInsensitive.store(caseSensitivity
                    ? *caseSensitivity == Qt::CaseInsensitive
                    : kCaseInsensitive);
}
