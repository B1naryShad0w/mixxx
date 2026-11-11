#include "library/djlib/djlibsidecar.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <cmath>

namespace {

constexpr int kSchemaVersion = 1;
// Largest integer a double represents exactly (JSON numbers are doubles in Qt).
constexpr double kMaxExactInteger = 9007199254740992.0;

bool fail(QString* pError, const QString& message) {
    if (pError) {
        *pError = message;
    }
    return false;
}

/// Parses `json` into a root object.
bool parseRootObject(const QByteArray& json, QJsonObject* pObject, QString* pError) {
    if (json.size() > djlib::kMaxJsonBytes) {
        return fail(pError,
                QStringLiteral("file is too large (%1 bytes)").arg(json.size()));
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(pError,
                QStringLiteral("invalid JSON at offset %1: %2")
                        .arg(parseError.offset)
                        .arg(parseError.errorString()));
    }
    if (!doc.isObject()) {
        return fail(pError, QStringLiteral("JSON root is not an object"));
    }
    *pObject = doc.object();
    return true;
}

bool readFiniteNumber(const QJsonValue& value,
        const QString& name,
        double* pResult,
        QString* pError) {
    if (!value.isDouble()) {
        return fail(pError, QStringLiteral("%1 must be a number").arg(name));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number)) {
        return fail(pError, QStringLiteral("%1 must be finite").arg(name));
    }
    *pResult = number;
    return true;
}

bool readNonNegative(const QJsonValue& value,
        const QString& name,
        double* pResult,
        QString* pError) {
    if (!readFiniteNumber(value, name, pResult, pError)) {
        return false;
    }
    if (*pResult < 0) {
        return fail(pError, QStringLiteral("%1 must be >= 0").arg(name));
    }
    return true;
}

bool readPositive(const QJsonValue& value,
        const QString& name,
        double* pResult,
        QString* pError) {
    if (!readFiniteNumber(value, name, pResult, pError)) {
        return false;
    }
    if (*pResult <= 0) {
        return fail(pError, QStringLiteral("%1 must be > 0").arg(name));
    }
    return true;
}

/// A position or time in seconds: 0..24 h.
bool readSeconds(const QJsonValue& value,
        const QString& name,
        double* pResult,
        QString* pError) {
    if (!readNonNegative(value, name, pResult, pError)) {
        return false;
    }
    if (*pResult > djlib::kMaxSeconds) {
        return fail(pError, QStringLiteral("%1 is out of range").arg(name));
    }
    return true;
}

bool readBpm(const QJsonValue& value,
        const QString& name,
        double* pResult,
        QString* pError) {
    if (!readPositive(value, name, pResult, pError)) {
        return false;
    }
    if (*pResult < djlib::kMinBpm || *pResult > djlib::kMaxBpm) {
        return fail(pError, QStringLiteral("%1 is out of range").arg(name));
    }
    return true;
}

bool readInteger(const QJsonValue& value,
        const QString& name,
        qint64 minimum,
        qint64* pResult,
        QString* pError) {
    double number;
    if (!readFiniteNumber(value, name, &number, pError)) {
        return false;
    }
    if (std::floor(number) != number || std::fabs(number) > kMaxExactInteger) {
        return fail(pError, QStringLiteral("%1 must be an integer").arg(name));
    }
    const auto integer = static_cast<qint64>(number);
    if (integer < minimum) {
        return fail(pError, QStringLiteral("%1 must be >= %2").arg(name).arg(minimum));
    }
    *pResult = integer;
    return true;
}

bool checkSchema(const QJsonObject& object, QString* pError) {
    const QJsonValue value = object.value(QStringLiteral("schema"));
    if (value.isUndefined()) {
        return fail(pError, QStringLiteral("missing field schema"));
    }
    if (!value.isDouble() || value.toDouble() != kSchemaVersion) {
        return fail(pError,
                QStringLiteral("unsupported schema (expected %1)").arg(kSchemaVersion));
    }
    return true;
}

/// Returns the value of a required field, or an undefined value after setting
/// the error.
QJsonValue required(const QJsonObject& object, const QString& name, QString* pError) {
    const QJsonValue value = object.value(name);
    if (value.isUndefined()) {
        fail(pError, QStringLiteral("missing field %1").arg(name));
    }
    return value;
}

bool readRequiredString(const QJsonObject& object,
        const QString& name,
        bool allowEmpty,
        QString* pResult,
        QString* pError) {
    const QJsonValue value = required(object, name, pError);
    if (value.isUndefined()) {
        return false;
    }
    if (!value.isString()) {
        return fail(pError, QStringLiteral("%1 must be a string").arg(name));
    }
    if (!allowEmpty && value.toString().isEmpty()) {
        return fail(pError, QStringLiteral("%1 must not be empty").arg(name));
    }
    *pResult = value.toString();
    return true;
}

/// Nullable string: absent or null -> nullopt.
bool readOptionalString(const QJsonObject& object,
        const QString& name,
        std::optional<QString>* pResult,
        QString* pError) {
    const QJsonValue value = object.value(name);
    if (value.isUndefined() || value.isNull()) {
        pResult->reset();
        return true;
    }
    if (!value.isString()) {
        return fail(pError, QStringLiteral("%1 must be a string or null").arg(name));
    }
    *pResult = value.toString();
    return true;
}

bool readCrateRef(const QJsonValue& value,
        const QString& context,
        djlib::CrateRef* pCrate,
        QString* pError) {
    if (!value.isObject()) {
        return fail(pError, QStringLiteral("%1 must be an object").arg(context));
    }
    const QJsonObject object = value.toObject();
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!id.isString() || id.toString().isEmpty()) {
        return fail(pError, QStringLiteral("%1.id must be a non-empty string").arg(context));
    }
    const QJsonValue name = object.value(QStringLiteral("name"));
    if (!name.isString()) {
        return fail(pError, QStringLiteral("%1.name must be a string").arg(context));
    }
    pCrate->id = id.toString();
    pCrate->name = name.toString();
    return true;
}

bool readCrateList(const QJsonObject& object,
        QList<djlib::CrateRef>* pCrates,
        QString* pError) {
    const QString name = QStringLiteral("crates");
    const QJsonValue value = required(object, name, pError);
    if (value.isUndefined()) {
        return false;
    }
    if (!value.isArray()) {
        return fail(pError, QStringLiteral("crates must be an array"));
    }
    const QJsonArray array = value.toArray();
    if (array.size() > djlib::kMaxCrates) {
        return fail(pError, QStringLiteral("crates has too many entries"));
    }
    QList<djlib::CrateRef> crates;
    crates.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        djlib::CrateRef crate;
        if (!readCrateRef(array.at(i), QStringLiteral("crates[%1]").arg(i), &crate, pError)) {
            return false;
        }
        crates.append(std::move(crate));
    }
    *pCrates = std::move(crates);
    return true;
}

bool readTempo(const QJsonObject& object, DjlibSidecar::Tempo* pTempo, QString* pError) {
    QString tempo;
    if (!readRequiredString(object, QStringLiteral("tempo"), true, &tempo, pError)) {
        return false;
    }
    if (tempo == QLatin1String("constant")) {
        *pTempo = DjlibSidecar::Tempo::Constant;
    } else if (tempo == QLatin1String("variable")) {
        *pTempo = DjlibSidecar::Tempo::Variable;
    } else if (tempo == QLatin1String("unknown")) {
        *pTempo = DjlibSidecar::Tempo::Unknown;
    } else {
        // Forward compatibility within schema 1: a tempo kind we don't know
        // means "don't touch beats", like "unknown".
        *pTempo = DjlibSidecar::Tempo::Unknown;
    }
    return true;
}

bool readGrid(const QJsonObject& object, DjlibSidecar::Grid* pGrid, QString* pError) {
    const QJsonValue value = object.value(QStringLiteral("grid"));
    if (!value.isObject()) {
        return fail(pError, QStringLiteral("grid must be an object for constant tempo"));
    }
    const QJsonObject grid = value.toObject();
    return readSeconds(grid.value(QStringLiteral("first_beat_s")),
                   QStringLiteral("grid.first_beat_s"),
                   &pGrid->firstBeatSeconds,
                   pError) &&
            readBpm(grid.value(QStringLiteral("bpm")),
                    QStringLiteral("grid.bpm"),
                    &pGrid->bpm,
                    pError);
}

bool readBeats(const QJsonObject& object, QList<double>* pBeats, QString* pError) {
    const QJsonValue value = object.value(QStringLiteral("beats_s"));
    if (!value.isArray()) {
        return fail(pError, QStringLiteral("beats_s must be an array for variable tempo"));
    }
    const QJsonArray array = value.toArray();
    // Mixxx needs two beats for a tempo.
    if (array.size() < 2) {
        return fail(pError, QStringLiteral("beats_s must have at least two beats"));
    }
    if (array.size() > djlib::kMaxBeats) {
        return fail(pError, QStringLiteral("beats_s has too many beats"));
    }
    QList<double> beats;
    beats.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        double beat;
        if (!readSeconds(array.at(i), QStringLiteral("beats_s[%1]").arg(i), &beat, pError)) {
            return false;
        }
        // Beats closer than this would fall onto the same frame.
        if (!beats.isEmpty() && beat < beats.last() + djlib::kMinBeatSeconds) {
            return fail(pError,
                    QStringLiteral("beats_s must be strictly ascending (index %1)").arg(i));
        }
        beats.append(beat);
    }
    *pBeats = std::move(beats);
    return true;
}

bool readReplayGain(const QJsonObject& object,
        std::optional<DjlibSidecar::ReplayGain>* pResult,
        QString* pError) {
    const QJsonValue value = object.value(QStringLiteral("replaygain"));
    if (value.isUndefined() || value.isNull()) {
        pResult->reset();
        return true;
    }
    if (!value.isObject()) {
        return fail(pError, QStringLiteral("replaygain must be an object or null"));
    }
    const QJsonObject rg = value.toObject();
    DjlibSidecar::ReplayGain replayGain;
    if (!readFiniteNumber(rg.value(QStringLiteral("gain_db")),
                QStringLiteral("replaygain.gain_db"),
                &replayGain.gainDb,
                pError) ||
            !readNonNegative(rg.value(QStringLiteral("peak")),
                    QStringLiteral("replaygain.peak"),
                    &replayGain.peak,
                    pError)) {
        return false;
    }
    if (std::fabs(replayGain.gainDb) > djlib::kMaxGainDb) {
        return fail(pError, QStringLiteral("replaygain.gain_db is out of range"));
    }
    if (replayGain.peak > djlib::kMaxPeak) {
        return fail(pError, QStringLiteral("replaygain.peak is out of range"));
    }
    *pResult = replayGain;
    return true;
}

/// Reads one cue. Sets *pSkip (and returns true) for a cue type this client
/// doesn't know: those are ignored for forward compatibility within schema 1.
bool readCue(const QJsonValue& value,
        qsizetype index,
        DjlibSidecar::Cue* pCue,
        bool* pSkip,
        QString* pError) {
    *pSkip = false;
    const QString context = QStringLiteral("cues[%1]").arg(index);
    if (!value.isObject()) {
        return fail(pError, QStringLiteral("%1 must be an object").arg(context));
    }
    const QJsonObject object = value.toObject();
    const QJsonValue type = object.value(QStringLiteral("type"));
    if (!type.isString()) {
        return fail(pError, QStringLiteral("%1.type must be a string").arg(context));
    }
    if (type.toString() == QLatin1String("main")) {
        pCue->type = DjlibSidecar::Cue::Type::Main;
        pCue->number = 0;
    } else if (type.toString() == QLatin1String("hotcue")) {
        pCue->type = DjlibSidecar::Cue::Type::HotCue;
        qint64 number;
        if (!readInteger(object.value(QStringLiteral("number")),
                    context + QStringLiteral(".number"),
                    1,
                    &number,
                    pError)) {
            return false;
        }
        if (number > 1000) {
            return fail(pError, QStringLiteral("%1.number is out of range").arg(context));
        }
        pCue->number = static_cast<int>(number);
    } else {
        *pSkip = true;
        return true;
    }
    if (!readSeconds(object.value(QStringLiteral("pos_s")),
                context + QStringLiteral(".pos_s"),
                &pCue->posSeconds,
                pError)) {
        return false;
    }
    const QJsonValue label = object.value(QStringLiteral("label"));
    if (label.isUndefined() || label.isNull()) {
        pCue->label.clear();
    } else if (label.isString()) {
        pCue->label = label.toString().left(djlib::kMaxLabelChars);
    } else {
        return fail(pError, QStringLiteral("%1.label must be a string or null").arg(context));
    }
    return true;
}

bool readCues(const QJsonObject& object, QList<DjlibSidecar::Cue>* pCues, QString* pError) {
    const QJsonValue value = required(object, QStringLiteral("cues"), pError);
    if (value.isUndefined()) {
        return false;
    }
    if (!value.isArray()) {
        return fail(pError, QStringLiteral("cues must be an array"));
    }
    const QJsonArray array = value.toArray();
    if (array.size() > djlib::kMaxCues) {
        return fail(pError, QStringLiteral("cues has too many entries"));
    }
    QList<DjlibSidecar::Cue> cues;
    cues.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        DjlibSidecar::Cue cue;
        bool skip;
        if (!readCue(array.at(i), i, &cue, &skip, pError)) {
            return false;
        }
        if (!skip) {
            cues.append(std::move(cue));
        }
    }
    *pCues = std::move(cues);
    return true;
}

bool readFlags(const QJsonObject& object, QStringList* pFlags, QString* pError) {
    const QJsonValue value = object.value(QStringLiteral("flags"));
    pFlags->clear();
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isArray()) {
        return fail(pError, QStringLiteral("flags must be an array"));
    }
    const QJsonArray array = value.toArray();
    for (const QJsonValue& flag : array) {
        // Open set: ignore entries we can't interpret.
        if (flag.isString()) {
            pFlags->append(flag.toString());
        }
    }
    return true;
}

bool parseSidecarObject(const QJsonObject& object, DjlibSidecar* pSidecar, QString* pError) {
    DjlibSidecar& s = *pSidecar;
    if (!checkSchema(object, pError)) {
        return false;
    }
    s.schema = kSchemaVersion;

    if (!readRequiredString(object, QStringLiteral("track_id"), false, &s.trackId, pError) ||
            !readOptionalString(object, QStringLiteral("tidal_id"), &s.tidalId, pError) ||
            !readOptionalString(object, QStringLiteral("isrc"), &s.isrc, pError)) {
        return false;
    }

    const QString sidecarVersion = QStringLiteral("sidecar_version");
    QJsonValue value = required(object, sidecarVersion, pError);
    if (value.isUndefined() ||
            !readInteger(value, sidecarVersion, 1, &s.sidecarVersion, pError)) {
        return false;
    }
    const QString packageVersion = QStringLiteral("package_version");
    value = required(object, packageVersion, pError);
    if (value.isUndefined() ||
            !readInteger(value, packageVersion, 0, &s.packageVersion, pError)) {
        return false;
    }

    if (!readRequiredString(object, QStringLiteral("file"), false, &s.file, pError) ||
            !readRequiredString(object, QStringLiteral("sha256"), true, &s.sha256, pError) ||
            !readCrateList(object, &s.crates, pError) ||
            !readTempo(object, &s.tempo, pError)) {
        return false;
    }

    value = object.value(QStringLiteral("bpm"));
    if (value.isUndefined() || value.isNull()) {
        s.bpm.reset();
    } else {
        double bpm;
        if (!readBpm(value, QStringLiteral("bpm"), &bpm, pError)) {
            return false;
        }
        s.bpm = bpm;
    }
    if (!readOptionalString(object, QStringLiteral("bpm_source"), &s.bpmSource, pError)) {
        return false;
    }

    s.grid.reset();
    s.beatsSeconds.reset();
    switch (s.tempo) {
    case DjlibSidecar::Tempo::Constant: {
        DjlibSidecar::Grid grid;
        if (!readGrid(object, &grid, pError)) {
            return false;
        }
        s.grid = grid;
        break;
    }
    case DjlibSidecar::Tempo::Variable: {
        QList<double> beats;
        if (!readBeats(object, &beats, pError)) {
            return false;
        }
        s.beatsSeconds = std::move(beats);
        break;
    }
    case DjlibSidecar::Tempo::Unknown:
        break;
    }

    value = object.value(QStringLiteral("first_downbeat_s"));
    if (value.isUndefined() || value.isNull()) {
        s.firstDownbeatSeconds.reset();
    } else {
        double downbeat;
        if (!readSeconds(value, QStringLiteral("first_downbeat_s"), &downbeat, pError)) {
            return false;
        }
        s.firstDownbeatSeconds = downbeat;
    }

    if (!readOptionalString(object, QStringLiteral("key"), &s.key, pError)) {
        return false;
    }
    if (s.key && s.key->trimmed().isEmpty()) {
        s.key.reset();
    }

    return readReplayGain(object, &s.replayGain, pError) &&
            readCues(object, &s.cues, pError) &&
            readFlags(object, &s.flags, pError);
}

} // namespace

// static
std::optional<DjlibSidecar> DjlibSidecar::parse(const QByteArray& json, QString* pError) {
    QJsonObject object;
    if (!parseRootObject(json, &object, pError)) {
        return std::nullopt;
    }
    DjlibSidecar sidecar;
    if (!parseSidecarObject(object, &sidecar, pError)) {
        return std::nullopt;
    }
    if (pError) {
        pError->clear();
    }
    return sidecar;
}

namespace djlib {

std::optional<QList<CrateRef>> parseCratesJson(const QByteArray& json, QString* pError) {
    QJsonObject object;
    if (!parseRootObject(json, &object, pError) || !checkSchema(object, pError)) {
        return std::nullopt;
    }
    QList<CrateRef> crates;
    if (!readCrateList(object, &crates, pError)) {
        return std::nullopt;
    }
    if (pError) {
        pError->clear();
    }
    return crates;
}

} // namespace djlib
