#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace djlib {

/// The djlib library folder is written by the agent only: packages are
/// verified against their checksums and must never be modified by Mixxx.
/// Files below these folders are excluded from the export of metadata into
/// file tags, whatever the library preferences say.
///
/// Folders are only ever added and stay protected until Mixxx exits: tracks
/// are saved (and their tags exported) after the djlib service is gone, and
/// a folder that was the library earlier in the session still holds the
/// agent's files. `root`: an absolute folder path; the caller also adds the
/// canonical form when it differs. Thread-safe.
void addProtectedRoot(const QString& root);
QStringList protectedRoots();

/// False while nothing is protected (no lock, no path handling): the check
/// to make first for every track that is saved.
bool hasProtectedRoots();

/// True if `location` (an absolute file path) is below a protected folder.
/// Names are compared by `pathCaseSensitivity()`. Thread-safe.
bool isProtectedFile(const QString& location);

/// Tests only: nothing is protected any more.
void resetProtectedRootsForTesting();

/// How file names compare here: without case on Windows and macOS.
Qt::CaseSensitivity pathCaseSensitivity();

/// True if both paths spell the same file or folder: compared cleaned (no
/// "..", no doubled or trailing slashes) and by `pathCaseSensitivity()`.
/// The file system is not asked, so links are not seen through.
bool samePath(const QString& a, const QString& b);

/// Tests only: compare names as Windows does (`Qt::CaseInsensitive`), or
/// as this platform does again (`std::nullopt`).
void setPathCaseSensitivityForTesting(std::optional<Qt::CaseSensitivity> caseSensitivity);

} // namespace djlib
