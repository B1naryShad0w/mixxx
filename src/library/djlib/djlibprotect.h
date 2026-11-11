#pragma once

#include <QString>
#include <QStringList>

namespace djlib {

/// The djlib library folder is written by the agent only: packages are
/// verified against their checksums and must never be modified by Mixxx.
/// Files below these folders are excluded from the export of metadata into
/// file tags, whatever the library preferences say.
///
/// `roots`: absolute folder paths (the configured path and, when it differs,
/// its canonical form); empty = nothing is protected. Thread-safe.
void setProtectedRoots(const QStringList& roots);
QStringList protectedRoots();

/// True if `location` (an absolute file path) is below a protected folder.
/// Names are compared without case on Windows and macOS. Thread-safe.
bool isProtectedFile(const QString& location);

} // namespace djlib
