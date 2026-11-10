#include "library/djlib/djlibsidecar.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

// Verbatim sample from docs/sync-contract.md, "Sidecar".
const QByteArray kConstantSample = QByteArrayLiteral(R"json({
  "schema": 1,
  "track_id": "trk_...",
  "tidal_id": "12345678",
  "isrc": "GBXXX2600001",
  "sidecar_version": 3,
  "package_version": 1,
  "file": "Artist - Title (Extended Mix) [12345678].stem.mp4",
  "sha256": "<64 hex, of the package>",
  "crates": [{"id": "crate_...", "name": "House"}, {"id": "crate_...", "name": "Requests / Halloween 2026"}],
  "tempo": "constant",
  "bpm": 124.0,
  "bpm_source": "tidal",
  "grid": {"first_beat_s": 0.1234, "bpm": 124.0},
  "beats_s": null,
  "first_downbeat_s": 0.5795,
  "key": "Am",
  "replaygain": {"gain_db": -9.0, "peak": 0.944},
  "cues": [
    {"type": "main", "pos_s": 0.5795},
    {"type": "hotcue", "number": 1, "pos_s": 0.5795, "label": "1st downbeat"}
  ],
  "flags": ["bpm_check"]
})json");

// The contract sample, as a variable-tempo track (beat map instead of grid).
const QByteArray kVariableSample = QByteArrayLiteral(R"json({
  "schema": 1,
  "track_id": "trk_var",
  "tidal_id": null,
  "isrc": null,
  "sidecar_version": 7,
  "package_version": 2,
  "file": "Live Band - Jam [87654321].stem.mp4",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "crates": [{"id": "crate_a", "name": "Funk"}],
  "tempo": "variable",
  "bpm": 101.5,
  "bpm_source": "measured",
  "grid": null,
  "beats_s": [0.25, 0.84, 1.43, 2.03, 2.62],
  "first_downbeat_s": 0.25,
  "key": "F#",
  "replaygain": {"gain_db": -6.5, "peak": 1.0},
  "cues": [
    {"type": "main", "pos_s": 0.25},
    {"type": "hotcue", "number": 1, "pos_s": 0.25, "label": "1st downbeat"}
  ],
  "flags": ["variable_tempo"]
})json");

// Unknown tempo: everything nullable is null, no cues.
const QByteArray kUnknownSample = QByteArrayLiteral(R"json({
  "schema": 1,
  "track_id": "trk_unk",
  "tidal_id": "1",
  "isrc": null,
  "sidecar_version": 1,
  "package_version": 1,
  "file": "Ambient - Drift [1].stem.mp4",
  "sha256": "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
  "crates": [],
  "tempo": "unknown",
  "bpm": null,
  "bpm_source": null,
  "grid": null,
  "beats_s": null,
  "first_downbeat_s": null,
  "key": null,
  "replaygain": null,
  "cues": [],
  "flags": []
})json");

QJsonObject sampleObject(const QByteArray& json = kConstantSample) {
    return QJsonDocument::fromJson(json).object();
}

QByteArray toJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<DjlibSidecar> parseObject(const QJsonObject& object, QString* pError = nullptr) {
    return DjlibSidecar::parse(toJson(object), pError);
}

TEST(DjlibSidecarTest, parsesConstantContractSample) {
    QString error;
    const auto sidecar = DjlibSidecar::parse(kConstantSample, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    EXPECT_TRUE(error.isEmpty());

    EXPECT_EQ(1, sidecar->schema);
    EXPECT_EQ(QStringLiteral("trk_..."), sidecar->trackId);
    EXPECT_EQ(QStringLiteral("12345678"), sidecar->tidalId);
    EXPECT_EQ(QStringLiteral("GBXXX2600001"), sidecar->isrc);
    EXPECT_EQ(3, sidecar->sidecarVersion);
    EXPECT_EQ(1, sidecar->packageVersion);
    EXPECT_EQ(QStringLiteral("Artist - Title (Extended Mix) [12345678].stem.mp4"), sidecar->file);
    EXPECT_EQ(QStringLiteral("<64 hex, of the package>"), sidecar->sha256);

    ASSERT_EQ(2, sidecar->crates.size());
    EXPECT_EQ(QStringLiteral("crate_..."), sidecar->crates[0].id);
    EXPECT_EQ(QStringLiteral("House"), sidecar->crates[0].name);
    EXPECT_EQ(QStringLiteral("Requests / Halloween 2026"), sidecar->crates[1].name);

    EXPECT_EQ(DjlibSidecar::Tempo::Constant, sidecar->tempo);
    ASSERT_TRUE(sidecar->bpm);
    EXPECT_DOUBLE_EQ(124.0, *sidecar->bpm);
    EXPECT_EQ(QStringLiteral("tidal"), sidecar->bpmSource);
    ASSERT_TRUE(sidecar->grid);
    EXPECT_DOUBLE_EQ(0.1234, sidecar->grid->firstBeatSeconds);
    EXPECT_DOUBLE_EQ(124.0, sidecar->grid->bpm);
    EXPECT_FALSE(sidecar->beatsSeconds);
    ASSERT_TRUE(sidecar->firstDownbeatSeconds);
    EXPECT_DOUBLE_EQ(0.5795, *sidecar->firstDownbeatSeconds);
    EXPECT_EQ(QStringLiteral("Am"), sidecar->key);
    ASSERT_TRUE(sidecar->replayGain);
    EXPECT_DOUBLE_EQ(-9.0, sidecar->replayGain->gainDb);
    EXPECT_DOUBLE_EQ(0.944, sidecar->replayGain->peak);

    ASSERT_EQ(2, sidecar->cues.size());
    EXPECT_EQ(DjlibSidecar::Cue::Type::Main, sidecar->cues[0].type);
    EXPECT_DOUBLE_EQ(0.5795, sidecar->cues[0].posSeconds);
    EXPECT_TRUE(sidecar->cues[0].label.isEmpty());
    EXPECT_EQ(DjlibSidecar::Cue::Type::HotCue, sidecar->cues[1].type);
    EXPECT_EQ(1, sidecar->cues[1].number);
    EXPECT_DOUBLE_EQ(0.5795, sidecar->cues[1].posSeconds);
    EXPECT_EQ(QStringLiteral("1st downbeat"), sidecar->cues[1].label);

    EXPECT_EQ(QStringList{QStringLiteral("bpm_check")}, sidecar->flags);
}

TEST(DjlibSidecarTest, parsesVariableTempo) {
    QString error;
    const auto sidecar = DjlibSidecar::parse(kVariableSample, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    EXPECT_EQ(DjlibSidecar::Tempo::Variable, sidecar->tempo);
    EXPECT_FALSE(sidecar->tidalId);
    EXPECT_FALSE(sidecar->isrc);
    EXPECT_EQ(7, sidecar->sidecarVersion);
    EXPECT_EQ(2, sidecar->packageVersion);
    EXPECT_FALSE(sidecar->grid);
    ASSERT_TRUE(sidecar->beatsSeconds);
    EXPECT_EQ((QList<double>{0.25, 0.84, 1.43, 2.03, 2.62}), *sidecar->beatsSeconds);
    EXPECT_EQ(QStringLiteral("F#"), sidecar->key);
    ASSERT_TRUE(sidecar->replayGain);
    EXPECT_DOUBLE_EQ(1.0, sidecar->replayGain->peak);
    EXPECT_EQ(QStringList{QStringLiteral("variable_tempo")}, sidecar->flags);
}

TEST(DjlibSidecarTest, parsesUnknownTempoWithNulls) {
    QString error;
    const auto sidecar = DjlibSidecar::parse(kUnknownSample, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    EXPECT_EQ(DjlibSidecar::Tempo::Unknown, sidecar->tempo);
    EXPECT_FALSE(sidecar->isrc);
    EXPECT_FALSE(sidecar->bpm);
    EXPECT_FALSE(sidecar->bpmSource);
    EXPECT_FALSE(sidecar->grid);
    EXPECT_FALSE(sidecar->beatsSeconds);
    EXPECT_FALSE(sidecar->firstDownbeatSeconds);
    EXPECT_FALSE(sidecar->key);
    EXPECT_FALSE(sidecar->replayGain);
    EXPECT_TRUE(sidecar->crates.isEmpty());
    EXPECT_TRUE(sidecar->cues.isEmpty());
    EXPECT_TRUE(sidecar->flags.isEmpty());
}

TEST(DjlibSidecarTest, ignoresUnknownFields) {
    QJsonObject object = sampleObject();
    object.insert(QStringLiteral("future_field"), QJsonObject{{QStringLiteral("x"), 1}});
    object.insert(QStringLiteral("flags"),
            QJsonArray{QStringLiteral("bpm_check"), QStringLiteral("some_future_flag")});
    QJsonObject grid = object.value(QStringLiteral("grid")).toObject();
    grid.insert(QStringLiteral("time_signature"), QStringLiteral("4/4"));
    object.insert(QStringLiteral("grid"), grid);
    QJsonArray crates = object.value(QStringLiteral("crates")).toArray();
    QJsonObject crate = crates[0].toObject();
    crate.insert(QStringLiteral("color"), QStringLiteral("#ff0000"));
    crates[0] = crate;
    object.insert(QStringLiteral("crates"), crates);
    QJsonArray cues = object.value(QStringLiteral("cues")).toArray();
    QJsonObject cue = cues[1].toObject();
    cue.insert(QStringLiteral("color"), QStringLiteral("#00ff00"));
    cues[1] = cue;
    object.insert(QStringLiteral("cues"), cues);

    QString error;
    const auto sidecar = parseObject(object, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    EXPECT_EQ(2, sidecar->crates.size());
    EXPECT_EQ(2, sidecar->cues.size());
    EXPECT_EQ((QStringList{QStringLiteral("bpm_check"), QStringLiteral("some_future_flag")}),
            sidecar->flags);
}

TEST(DjlibSidecarTest, missingNullableFieldsAreNull) {
    QJsonObject object = sampleObject(kUnknownSample);
    for (const auto* key : {"tidal_id",
                 "isrc",
                 "bpm",
                 "bpm_source",
                 "grid",
                 "beats_s",
                 "first_downbeat_s",
                 "key",
                 "replaygain",
                 "flags"}) {
        object.remove(QString::fromLatin1(key));
    }
    QString error;
    const auto sidecar = parseObject(object, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    EXPECT_FALSE(sidecar->tidalId);
    EXPECT_FALSE(sidecar->bpm);
    EXPECT_FALSE(sidecar->key);
    EXPECT_TRUE(sidecar->flags.isEmpty());
}

TEST(DjlibSidecarTest, rejectsBadSchema) {
    for (const QJsonValue& schema : {QJsonValue(2),
                 QJsonValue(0),
                 QJsonValue(1.5),
                 QJsonValue(QStringLiteral("1")),
                 QJsonValue(QJsonValue::Null)}) {
        QJsonObject object = sampleObject();
        object.insert(QStringLiteral("schema"), schema);
        QString error;
        EXPECT_FALSE(parseObject(object, &error)) << toJson(object).toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
    QJsonObject object = sampleObject();
    object.remove(QStringLiteral("schema"));
    QString error;
    EXPECT_FALSE(parseObject(object, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("schema"))) << error.toStdString();
}

TEST(DjlibSidecarTest, rejectsMissingRequiredFields) {
    for (const auto* key : {"track_id",
                 "sidecar_version",
                 "package_version",
                 "file",
                 "sha256",
                 "crates",
                 "tempo",
                 "cues"}) {
        QJsonObject object = sampleObject();
        object.remove(QString::fromLatin1(key));
        QString error;
        EXPECT_FALSE(parseObject(object, &error)) << key;
        EXPECT_TRUE(error.contains(QString::fromLatin1(key)))
                << key << ": " << error.toStdString();
    }
}

TEST(DjlibSidecarTest, rejectsWrongTypes) {
    const QList<QPair<QString, QJsonValue>> cases = {
            {QStringLiteral("track_id"), QJsonValue(42)},
            {QStringLiteral("track_id"), QJsonValue(QString())},
            {QStringLiteral("sidecar_version"), QJsonValue(QStringLiteral("3"))},
            {QStringLiteral("sidecar_version"), QJsonValue(2.5)},
            {QStringLiteral("sidecar_version"), QJsonValue(0)},
            {QStringLiteral("package_version"), QJsonValue(-1)},
            {QStringLiteral("tidal_id"), QJsonValue(12345678)},
            {QStringLiteral("crates"), QJsonValue(QStringLiteral("House"))},
            {QStringLiteral("crates"), QJsonArray{QStringLiteral("crate_x")}},
            {QStringLiteral("crates"), QJsonArray{QJsonObject{{QStringLiteral("name"), "House"}}}},
            {QStringLiteral("tempo"), QJsonValue(QStringLiteral("swing"))},
            {QStringLiteral("bpm"), QJsonValue(QStringLiteral("124"))},
            {QStringLiteral("bpm"), QJsonValue(0)},
            {QStringLiteral("grid"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("grid"), QJsonObject{{QStringLiteral("first_beat_s"), 0.1}}},
            {QStringLiteral("grid"),
                    QJsonObject{{QStringLiteral("first_beat_s"), -0.1},
                            {QStringLiteral("bpm"), 124.0}}},
            {QStringLiteral("grid"),
                    QJsonObject{{QStringLiteral("first_beat_s"), 0.1},
                            {QStringLiteral("bpm"), 0.0}}},
            {QStringLiteral("first_downbeat_s"), QJsonValue(-1.0)},
            {QStringLiteral("key"), QJsonValue(9)},
            {QStringLiteral("replaygain"), QJsonObject{{QStringLiteral("gain_db"), -9.0}}},
            {QStringLiteral("replaygain"),
                    QJsonObject{{QStringLiteral("gain_db"), -9.0},
                            {QStringLiteral("peak"), -0.5}}},
            {QStringLiteral("flags"), QJsonValue(QStringLiteral("bpm_check"))},
    };
    for (const auto& [key, value] : cases) {
        QJsonObject object = sampleObject();
        object.insert(key, value);
        QString error;
        EXPECT_FALSE(parseObject(object, &error)) << toJson(object).toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(DjlibSidecarTest, rejectsNonFiniteNumbers) {
    // JSON has no NaN/Infinity; an overflowing literal is the only way in.
    QByteArray json = kConstantSample;
    json.replace("\"bpm\": 124.0,", "\"bpm\": 1e999,");
    QString error;
    EXPECT_FALSE(DjlibSidecar::parse(json, &error));
    EXPECT_FALSE(error.isEmpty());

    json = kVariableSample;
    json.replace("1.43", "1e999");
    EXPECT_FALSE(DjlibSidecar::parse(json, &error));
}

TEST(DjlibSidecarTest, validatesVariableBeats) {
    const QList<QJsonValue> badBeats = {
            QJsonValue(QJsonValue::Null),
            QJsonArray{},
            QJsonArray{0.5, 0.4, 1.0},         // descending
            QJsonArray{0.5, 0.5, 1.0},         // duplicate
            QJsonArray{-0.1, 0.5},             // negative
            QJsonArray{0.1, QStringLiteral("x")}, // not a number
    };
    for (const auto& beats : badBeats) {
        QJsonObject object = sampleObject(kVariableSample);
        object.insert(QStringLiteral("beats_s"), beats);
        QString error;
        EXPECT_FALSE(parseObject(object, &error)) << toJson(object).toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(DjlibSidecarTest, unknownTempoIgnoresBeatData) {
    // Beat data is only meaningful for its own tempo; for "unknown" it is dropped.
    QJsonObject object = sampleObject(kUnknownSample);
    object.insert(QStringLiteral("grid"),
            QJsonObject{{QStringLiteral("first_beat_s"), 0.1}, {QStringLiteral("bpm"), 120.0}});
    object.insert(QStringLiteral("beats_s"), QJsonArray{0.1, 0.6});
    QString error;
    const auto sidecar = parseObject(object, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    EXPECT_FALSE(sidecar->grid);
    EXPECT_FALSE(sidecar->beatsSeconds);
}

TEST(DjlibSidecarTest, validatesCues) {
    const QList<QJsonValue> badCues = {
            QJsonValue(QJsonValue::Null),
            QJsonArray{QStringLiteral("main")},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "loop"}, {QStringLiteral("pos_s"), 1.0}}},
            QJsonArray{QJsonObject{{QStringLiteral("pos_s"), 1.0}}},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "main"}}},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "main"}, {QStringLiteral("pos_s"), -1.0}}},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "hotcue"}, {QStringLiteral("pos_s"), 1.0}}},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "hotcue"},
                    {QStringLiteral("number"), 0},
                    {QStringLiteral("pos_s"), 1.0}}},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "hotcue"},
                    {QStringLiteral("number"), 1.5},
                    {QStringLiteral("pos_s"), 1.0}}},
            QJsonArray{QJsonObject{{QStringLiteral("type"), "hotcue"},
                    {QStringLiteral("number"), 1},
                    {QStringLiteral("pos_s"), 1.0},
                    {QStringLiteral("label"), 5}}},
    };
    for (const auto& cues : badCues) {
        QJsonObject object = sampleObject();
        object.insert(QStringLiteral("cues"), cues);
        QString error;
        EXPECT_FALSE(parseObject(object, &error)) << toJson(object).toStdString();
        EXPECT_FALSE(error.isEmpty());
    }

    // A hot cue without a label is fine.
    QJsonObject object = sampleObject();
    object.insert(QStringLiteral("cues"),
            QJsonArray{QJsonObject{{QStringLiteral("type"), "hotcue"},
                    {QStringLiteral("number"), 2},
                    {QStringLiteral("pos_s"), 3.0}}});
    QString error;
    const auto sidecar = parseObject(object, &error);
    ASSERT_TRUE(sidecar) << error.toStdString();
    ASSERT_EQ(1, sidecar->cues.size());
    EXPECT_EQ(2, sidecar->cues[0].number);
    EXPECT_TRUE(sidecar->cues[0].label.isEmpty());
}

TEST(DjlibSidecarTest, rejectsGarbage) {
    for (const QByteArray& json : {QByteArray(),
                 QByteArrayLiteral("not json"),
                 QByteArrayLiteral("{\"schema\": 1,"),
                 QByteArrayLiteral("[]"),
                 QByteArrayLiteral("null"),
                 QByteArrayLiteral("42"),
                 QByteArrayLiteral("\xff\xfe\x00garbage")}) {
        QString error;
        EXPECT_FALSE(DjlibSidecar::parse(json, &error)) << json.toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
    // A null error pointer is allowed.
    EXPECT_FALSE(DjlibSidecar::parse(QByteArrayLiteral("garbage")));
}

TEST(DjlibCratesJsonTest, parsesCrateList) {
    QString error;
    const auto crates = djlib::parseCratesJson(QByteArrayLiteral(
                                                       R"json({"schema": 1, "crates": [
          {"id": "crate_1", "name": "House", "future": true},
          {"id": "crate_2", "name": "Requests / Halloween 2026"}],
          "generated_at": "2026-11-10T00:00:00Z"})json"),
            &error);
    ASSERT_TRUE(crates) << error.toStdString();
    ASSERT_EQ(2, crates->size());
    EXPECT_EQ((djlib::CrateRef{QStringLiteral("crate_1"), QStringLiteral("House")}), (*crates)[0]);
    EXPECT_EQ((djlib::CrateRef{QStringLiteral("crate_2"),
                      QStringLiteral("Requests / Halloween 2026")}),
            (*crates)[1]);
}

TEST(DjlibCratesJsonTest, parsesEmptyList) {
    const auto crates = djlib::parseCratesJson(QByteArrayLiteral(R"({"schema": 1, "crates": []})"));
    ASSERT_TRUE(crates);
    EXPECT_TRUE(crates->isEmpty());
}

TEST(DjlibCratesJsonTest, rejectsInvalid) {
    for (const QByteArray& json : {
                 QByteArray(),
                 QByteArrayLiteral("garbage"),
                 QByteArrayLiteral("[]"),
                 QByteArrayLiteral(R"({"crates": []})"),
                 QByteArrayLiteral(R"({"schema": 2, "crates": []})"),
                 QByteArrayLiteral(R"({"schema": 1})"),
                 QByteArrayLiteral(R"({"schema": 1, "crates": {}})"),
                 QByteArrayLiteral(R"({"schema": 1, "crates": ["crate_1"]})"),
                 QByteArrayLiteral(R"({"schema": 1, "crates": [{"name": "House"}]})"),
                 QByteArrayLiteral(R"({"schema": 1, "crates": [{"id": "", "name": "House"}]})"),
                 QByteArrayLiteral(R"({"schema": 1, "crates": [{"id": "crate_1"}]})"),
                 QByteArrayLiteral(R"({"schema": 1, "crates": [{"id": "c", "name": 3}]})"),
         }) {
        QString error;
        EXPECT_FALSE(djlib::parseCratesJson(json, &error)) << json.toStdString();
        EXPECT_FALSE(error.isEmpty()) << json.toStdString();
    }
}

} // namespace
