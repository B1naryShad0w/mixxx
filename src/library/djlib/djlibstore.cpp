#include "library/djlib/djlibstore.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "util/assert.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("DjlibStore");

const QString kCreateCrates = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS djlib_crates ("
        "djlib_id TEXT PRIMARY KEY, "
        "crate_id INTEGER UNIQUE NOT NULL, "
        "name TEXT NOT NULL)");

const QString kCreateTracks = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS djlib_tracks ("
        "djlib_id TEXT PRIMARY KEY, "
        "track_id INTEGER UNIQUE, "
        "file TEXT NOT NULL, "
        "sidecar_version INTEGER NOT NULL, "
        "package_version INTEGER, "
        "applied_beats TEXT, "
        "applied_cues TEXT, "
        "applied_key INTEGER, "
        "pending_json TEXT, "
        "updated_at INTEGER)");

const QString kTrackColumns = QStringLiteral(
        "djlib_id, track_id, file, sidecar_version, package_version, "
        "applied_beats, applied_cues, applied_key, pending_json, updated_at");

bool execOrLog(QSqlQuery& query) {
    if (query.exec()) {
        return true;
    }
    kLogger.warning() << "SQL failed:" << query.lastQuery() << query.lastError();
    return false;
}

bool prepareOrLog(QSqlQuery& query, const QString& sql) {
    if (query.prepare(sql)) {
        return true;
    }
    kLogger.warning() << "SQL prepare failed:" << sql << query.lastError();
    return false;
}

QVariant nullableText(const QString& text) {
    return text.isNull() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(text);
}

template<typename T>
QVariant nullable(const std::optional<T>& value, QMetaType::Type type) {
    return value ? QVariant::fromValue(*value) : QVariant(QMetaType(type));
}

DjlibStore::CrateMapping crateMappingFromQuery(const QSqlQuery& query) {
    return DjlibStore::CrateMapping{
            query.value(0).toString(),
            CrateId(query.value(1)),
            query.value(2).toString()};
}

} // namespace

DjlibStore::DjlibStore(const QSqlDatabase& database)
        : m_database(database) {
}

bool DjlibStore::initialize() {
    for (const auto& sql : {kCreateCrates, kCreateTracks}) {
        QSqlQuery query(m_database);
        if (!prepareOrLog(query, sql) || !execOrLog(query)) {
            return false;
        }
    }
    return true;
}

QList<DjlibStore::CrateMapping> DjlibStore::crateMappings() const {
    QList<CrateMapping> result;
    QSqlQuery query(m_database);
    if (!prepareOrLog(query,
                QStringLiteral("SELECT djlib_id, crate_id, name FROM djlib_crates "
                               "ORDER BY djlib_id")) ||
            !execOrLog(query)) {
        return result;
    }
    while (query.next()) {
        result.append(crateMappingFromQuery(query));
    }
    return result;
}

std::optional<DjlibStore::CrateMapping> DjlibStore::crateMapping(const QString& djlibId) const {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query,
                QStringLiteral("SELECT djlib_id, crate_id, name FROM djlib_crates "
                               "WHERE djlib_id = :id"))) {
        return std::nullopt;
    }
    query.bindValue(QStringLiteral(":id"), djlibId);
    if (!execOrLog(query) || !query.next()) {
        return std::nullopt;
    }
    return crateMappingFromQuery(query);
}

std::optional<DjlibStore::CrateMapping> DjlibStore::crateMappingByCrateId(CrateId crateId) const {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query,
                QStringLiteral("SELECT djlib_id, crate_id, name FROM djlib_crates "
                               "WHERE crate_id = :id"))) {
        return std::nullopt;
    }
    query.bindValue(QStringLiteral(":id"), crateId.toVariant());
    if (!execOrLog(query) || !query.next()) {
        return std::nullopt;
    }
    return crateMappingFromQuery(query);
}

bool DjlibStore::upsertCrateMapping(const CrateMapping& mapping) {
    VERIFY_OR_DEBUG_ASSERT(!mapping.djlibId.isEmpty() && mapping.crateId.isValid()) {
        return false;
    }
    QSqlQuery query(m_database);
    if (!prepareOrLog(query,
                QStringLiteral("INSERT OR REPLACE INTO djlib_crates "
                               "(djlib_id, crate_id, name) VALUES (:djlib_id, :crate_id, :name)"))) {
        return false;
    }
    query.bindValue(QStringLiteral(":djlib_id"), mapping.djlibId);
    query.bindValue(QStringLiteral(":crate_id"), mapping.crateId.toVariant());
    query.bindValue(QStringLiteral(":name"), mapping.name);
    return execOrLog(query);
}

bool DjlibStore::removeCrateMapping(const QString& djlibId) {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query, QStringLiteral("DELETE FROM djlib_crates WHERE djlib_id = :id"))) {
        return false;
    }
    query.bindValue(QStringLiteral(":id"), djlibId);
    return execOrLog(query);
}

bool DjlibStore::removeCrateMappingByCrateId(CrateId crateId) {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query, QStringLiteral("DELETE FROM djlib_crates WHERE crate_id = :id"))) {
        return false;
    }
    query.bindValue(QStringLiteral(":id"), crateId.toVariant());
    return execOrLog(query);
}

QList<DjlibStore::TrackState> DjlibStore::selectTrackStates(
        const QString& where, const QVariant& value) const {
    QList<TrackState> result;
    QString sql = QStringLiteral("SELECT %1 FROM djlib_tracks").arg(kTrackColumns);
    if (!where.isEmpty()) {
        sql += QStringLiteral(" WHERE %1 = :value").arg(where);
    }
    sql += QStringLiteral(" ORDER BY djlib_id");
    QSqlQuery query(m_database);
    if (!prepareOrLog(query, sql)) {
        return result;
    }
    if (!where.isEmpty()) {
        query.bindValue(QStringLiteral(":value"), value);
    }
    if (!execOrLog(query)) {
        return result;
    }
    while (query.next()) {
        TrackState state;
        state.djlibId = query.value(0).toString();
        if (!query.value(1).isNull()) {
            state.trackId = TrackId(query.value(1));
        }
        state.file = query.value(2).toString();
        state.sidecarVersion = query.value(3).toLongLong();
        if (!query.value(4).isNull()) {
            state.packageVersion = query.value(4).toLongLong();
        }
        state.appliedBeats = query.value(5).toString();
        state.appliedCues = query.value(6).toString();
        if (!query.value(7).isNull()) {
            state.appliedKey = query.value(7).toInt();
        }
        if (!query.value(8).isNull()) {
            state.pendingJson = query.value(8).toString().toUtf8();
        }
        state.updatedAt = query.value(9).toLongLong();
        result.append(std::move(state));
    }
    return result;
}

QList<DjlibStore::TrackState> DjlibStore::trackStates() const {
    return selectTrackStates(QString(), QVariant());
}

std::optional<DjlibStore::TrackState> DjlibStore::trackState(const QString& djlibId) const {
    const auto states = selectTrackStates(QStringLiteral("djlib_id"), djlibId);
    if (states.isEmpty()) {
        return std::nullopt;
    }
    return states.first();
}

std::optional<DjlibStore::TrackState> DjlibStore::trackStateByTrackId(TrackId trackId) const {
    if (!trackId.isValid()) {
        return std::nullopt;
    }
    const auto states = selectTrackStates(QStringLiteral("track_id"), trackId.toVariant());
    if (states.isEmpty()) {
        return std::nullopt;
    }
    return states.first();
}

bool DjlibStore::upsertTrackState(const TrackState& state) {
    VERIFY_OR_DEBUG_ASSERT(!state.djlibId.isEmpty()) {
        return false;
    }
    QSqlQuery query(m_database);
    if (!prepareOrLog(query,
                QStringLiteral("INSERT OR REPLACE INTO djlib_tracks (%1) VALUES "
                               "(:djlib_id, :track_id, :file, :sidecar_version, "
                               ":package_version, :applied_beats, :applied_cues, "
                               ":applied_key, :pending_json, :updated_at)")
                        .arg(kTrackColumns))) {
        return false;
    }
    query.bindValue(QStringLiteral(":djlib_id"), state.djlibId);
    query.bindValue(QStringLiteral(":track_id"),
            state.trackId.isValid() ? state.trackId.toVariant()
                                    : QVariant(QMetaType(QMetaType::Int)));
    query.bindValue(QStringLiteral(":file"), state.file.isNull() ? QString("") : state.file);
    query.bindValue(QStringLiteral(":sidecar_version"), state.sidecarVersion);
    query.bindValue(QStringLiteral(":package_version"),
            nullable(state.packageVersion, QMetaType::LongLong));
    query.bindValue(QStringLiteral(":applied_beats"), nullableText(state.appliedBeats));
    query.bindValue(QStringLiteral(":applied_cues"), nullableText(state.appliedCues));
    query.bindValue(QStringLiteral(":applied_key"), nullable(state.appliedKey, QMetaType::Int));
    query.bindValue(QStringLiteral(":pending_json"),
            state.pendingJson ? QVariant(QString::fromUtf8(*state.pendingJson))
                              : QVariant(QMetaType(QMetaType::QString)));
    query.bindValue(QStringLiteral(":updated_at"), QDateTime::currentMSecsSinceEpoch());
    return execOrLog(query);
}

bool DjlibStore::setPendingJson(
        const QString& djlibId, const std::optional<QByteArray>& json) {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query,
                QStringLiteral("UPDATE djlib_tracks SET pending_json = :json, "
                               "updated_at = :now WHERE djlib_id = :id"))) {
        return false;
    }
    query.bindValue(QStringLiteral(":json"),
            json ? QVariant(QString::fromUtf8(*json))
                 : QVariant(QMetaType(QMetaType::QString)));
    query.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    query.bindValue(QStringLiteral(":id"), djlibId);
    if (!execOrLog(query)) {
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool DjlibStore::removeTrackState(const QString& djlibId) {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query, QStringLiteral("DELETE FROM djlib_tracks WHERE djlib_id = :id"))) {
        return false;
    }
    query.bindValue(QStringLiteral(":id"), djlibId);
    return execOrLog(query);
}

bool DjlibStore::removeTrackStatesByTrackIds(const QList<TrackId>& trackIds) {
    QSqlQuery query(m_database);
    if (!prepareOrLog(query, QStringLiteral("DELETE FROM djlib_tracks WHERE track_id = :id"))) {
        return false;
    }
    for (const auto& trackId : trackIds) {
        query.bindValue(QStringLiteral(":id"), trackId.toVariant());
        if (!execOrLog(query)) {
            return false;
        }
    }
    return true;
}
