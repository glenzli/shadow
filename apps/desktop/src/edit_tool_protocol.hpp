#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <array>
#include <optional>

namespace EditToolProtocol {
inline constexpr auto schema = "shadow.edit-tools/1";
inline constexpr qsizetype maximumLineBytes = 64 * 1024;
enum class Command { Discover, Snapshot, Preview, Apply, Export, Cancel, Shutdown };
inline constexpr qsizetype maximumEdits = 16;
enum class EditKind { Exposure, Contrast, Saturation };
struct EditSpec final {
    EditKind kind;
    const char* type;
    const char* field;
    double minimum, maximum;
    const char* units;
};
inline constexpr std::array editSpecs{
    EditSpec{EditKind::Exposure, "set_exposure", "stops", -16, 16, "stops"},
    EditSpec{EditKind::Contrast, "set_contrast", "factor", 0, 8, "factor"},
    EditSpec{EditKind::Saturation, "set_saturation", "factor", 0, 8, "factor"}
};
struct Edit final {
    EditKind kind = EditKind::Exposure;
    QString node_id;
    double value = 0;
};
struct Request final {
    QString id;
    Command command = Command::Discover;
    QJsonObject expected;
    QString proposal_id, output_path, request_id;
    QVector<Edit> edits;
};
struct ParseResult final {
    std::optional<Request> request;
    QString id, error;
};
[[nodiscard]] ParseResult parse(const QByteArray& line);
[[nodiscard]] QJsonObject success(const QString& id, const QJsonObject& result);
[[nodiscard]] QJsonObject failure(const QString& id, const QString& code, const QString& message);
[[nodiscard]] QJsonObject discovery();
} // namespace EditToolProtocol
