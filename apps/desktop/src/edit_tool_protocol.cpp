#include "edit_tool_protocol.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace EditToolProtocol {
namespace {
bool fields(const QJsonObject& object, const QSet<QString>& required) {
    if (object.size() != required.size())
        return false;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!required.contains(it.key()))
            return false;
    return true;
}
bool text(const QJsonValue& value, const bool empty = false, const qsizetype maximum = 256) {
    return value.isString() && (empty || !value.toString().isEmpty())
           && value.toString().size() <= maximum && !value.toString().contains(QChar::Null);
}
bool identity(const QJsonValue& value) {
    if (!value.isObject())
        return false;
    const auto object = value.toObject();
    if (!fields(
            object,
            {"sessionId",
             "snapshotId",
             "photoId",
             "representationId",
             "baseCommitId",
             "workingCommitId",
             "activeVariantId",
             "draftRevision"}
        ))
        return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!text(it.value(), it.key() == "baseCommitId" || it.key() == "workingCommitId"))
            return false;
    }
    const auto revision = object.value("draftRevision").toString();
    bool valid = false;
    const auto number = revision.toULongLong(&valid);
    return valid && QString::number(number) == revision;
}
} // namespace

ParseResult parse(const QByteArray& line) {
    ParseResult result;
    const auto reject = [&result](const char* error) {
        result.error = QString::fromLatin1(error);
        return result;
    };
    if (line.size() > maximumLineBytes)
        return reject("request exceeds 64 KiB");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return reject("request must be one JSON object");
    const auto object = document.object();
    if (text(object.value("id")))
        result.id = object.value("id").toString();
    if (object.value("schema").toString() != QLatin1String(schema) || result.id.isEmpty()
        || !text(object.value("op")) || !object.value("params").isObject()
        || !fields(object, {"schema", "id", "op", "params"}))
        return reject("invalid request envelope");
    Request request;
    request.id = result.id;
    const auto params = object.value("params").toObject();
    const auto operation = object.value("op").toString();
    if (operation == "discover" || operation == "snapshot" || operation == "shutdown") {
        if (!params.isEmpty())
            return reject("operation takes no parameters");
        request.command = operation == "discover"   ? Command::Discover
                          : operation == "snapshot" ? Command::Snapshot
                                                    : Command::Shutdown;
    } else if (operation == "cancel") {
        if (!fields(params, {"requestId"}) || !text(params.value("requestId")))
            return reject("cancel requires a requestId");
        request.command = Command::Cancel;
        request.request_id = params.value("requestId").toString();
    } else if (operation == "preview" || operation == "apply" || operation == "export") {
        // The legacy singular form remains valid. Exactly one form is allowed;
        // the bounded array represents one atomic proposal, not sequential commits.
        const auto edit_field = params.contains("operations") ? QStringLiteral("operations")
                                                              : QStringLiteral("operation");
        const QSet<QString> required =
            operation == "preview" ? QSet<QString>{"expected", edit_field, "outputPath"}
            : operation == "apply" ? QSet<QString>{"expected", "proposalId"}
                                   : QSet<QString>{"expected", "outputPath"};
        if (!fields(params, required) || !identity(params.value("expected")))
            return reject("operation requires an exact snapshot identity");
        request.expected = params.value("expected").toObject();
        if (operation == "apply") {
            if (!text(params.value("proposalId")))
                return reject("apply requires a proposalId");
            request.command = Command::Apply;
            request.proposal_id = params.value("proposalId").toString();
        } else {
            if (!text(params.value("outputPath"), false, 4096))
                return reject("operation requires a new outputPath");
            request.output_path = params.value("outputPath").toString();
            request.command = operation == "preview" ? Command::Preview : Command::Export;
        }
        if (operation == "preview") {
            QJsonArray edits;
            if (edit_field == "operations") {
                if (!params.value(edit_field).isArray())
                    return reject("operations must be a nonempty array");
                edits = params.value(edit_field).toArray();
            } else {
                if (!params.value(edit_field).isObject())
                    return reject("operation must be a typed object");
                edits.append(params.value(edit_field));
            }
            if (edits.isEmpty() || edits.size() > maximumEdits)
                return reject("a proposal requires between 1 and 16 adjustments");
            for (const auto& value : edits) {
                if (!value.isObject())
                    return reject("each adjustment must be a typed object");
                const auto edit = value.toObject();
                const auto spec =
                    std::find_if(editSpecs.begin(), editSpecs.end(), [&](const auto& item) {
                        return edit.value("type").toString() == QLatin1String(item.type);
                    });
                if (spec == editSpecs.end()
                    || !fields(edit, {"type", "nodeId", QString::fromLatin1(spec->field)})
                    || !text(edit.value("nodeId")) || !edit.value(spec->field).isDouble())
                    return reject("unsupported or malformed typed adjustment");
                const Edit parsed{
                    spec->kind,
                    edit.value("nodeId").toString(),
                    edit.value(spec->field).toDouble()
                };
                if (!std::isfinite(parsed.value) || parsed.value < spec->minimum
                    || parsed.value > spec->maximum)
                    return reject("adjustment must be finite and within its discovered range");
                if (std::any_of(
                        request.edits.begin(),
                        request.edits.end(),
                        [&](const auto& previous) {
                            return previous.kind == parsed.kind
                                   && previous.node_id == parsed.node_id;
                        }
                    ))
                    return reject("a proposal cannot set the same node parameter twice");
                request.edits.append(parsed);
            }
        }
    } else {
        return reject("unsupported operation");
    }
    result.request = std::move(request);
    return result;
}
QJsonObject success(const QString& id, const QJsonObject& result) {
    return {{"schema", schema}, {"id", id}, {"ok", true}, {"result", result}};
}
QJsonObject failure(const QString& id, const QString& code, const QString& message) {
    return {
        {"schema", schema},
        {"id", id},
        {"ok", false},
        {"error", QJsonObject{{"code", code}, {"message", message}}}
    };
}
QJsonObject discovery() {
    QJsonArray edits;
    for (const auto& spec : editSpecs)
        edits.append(
            QJsonObject{
                {"type", spec.type},
                {"valueField", spec.field},
                {"minimum", spec.minimum},
                {"maximum", spec.maximum},
                {"units", spec.units}
            }
        );
    return {
        {"scope", "one_explicit_isolated_photo"},
        {"schema", schema},
        {"operations",
         QJsonArray{"discover", "snapshot", "preview", "apply", "export", "cancel", "shutdown"}},
        {"edits", edits},
        {"preview",
         QJsonObject{
             {"format", "jpeg"},
             {"maxEdge", 1024},
             {"maximumOperations", static_cast<int>(maximumEdits)},
             {"operationForms", QJsonArray{"operation", "operations"}},
             {"composition", "atomic_absolute_values_one_undo"},
             {"cancel", "supported"},
             {"cancellationBoundary", "before_artifact_publication"},
             {"publication", "new_file_only"}
         }},
        {"apply",
         QJsonObject{
             {"cancel", "unsupported"},
             {"draftPolicy", "reject_unsaved_or_busy"},
             {"cas", QJsonArray{"workingCommitId", "activeVariantId"}}
         }},
        {"export",
         QJsonObject{
             {"format", "png"},
             {"bitDepth", 8},
             {"colorSpace", "sRGB"},
             {"cancel", "unsupported"},
             {"source", "explicit_committed_recipe"},
             {"publication", "new_file_only"}
         }},
        {"persistence", "temporary_session_export_before_shutdown"},
        {"restart", "new_session_old_tokens_rejected"},
        {"requestIds", "correlation_only_no_durable_replay"},
        {"maximumRequestBytes", static_cast<int>(maximumLineBytes)}
    };
}
} // namespace EditToolProtocol
