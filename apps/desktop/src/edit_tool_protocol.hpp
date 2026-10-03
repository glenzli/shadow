#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

namespace EditToolProtocol {
inline constexpr auto schema = "shadow.edit-tools/1";
inline constexpr qsizetype maximumLineBytes = 64 * 1024;
enum class Command { Discover, Snapshot, Preview, Apply, Export, Cancel, Shutdown };
struct Request final {
    QString id;
    Command command = Command::Discover;
    QJsonObject expected;
    QString node_id, proposal_id, output_path, request_id;
    double exposure_stops = 0;
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
