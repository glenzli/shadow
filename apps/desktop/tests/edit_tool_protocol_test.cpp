#include "edit_tool_protocol.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <cstdlib>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        qCritical() << message;
        std::exit(1);
    }
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QJsonObject identity{
        {"sessionId", "session"},
        {"snapshotId", "snapshot"},
        {"photoId", "photo"},
        {"representationId", "representation"},
        {"baseCommitId", ""},
        {"workingCommitId", ""},
        {"activeVariantId", "variant"},
        {"draftRevision", "18446744073709551615"}
    };
    QJsonObject request{
        {"schema", EditToolProtocol::schema},
        {"id", "request"},
        {"op", "preview"},
        {"params",
         QJsonObject{
             {"expected", identity},
             {"operation",
              QJsonObject{{"type", "set_exposure"}, {"nodeId", "node"}, {"stops", 1.25}}},
             {"outputPath", "/tmp/preview.jpg"}
         }}
    };
    const auto parse = [&] { return EditToolProtocol::parse(QJsonDocument(request).toJson()); };
    require(parse().request.has_value(), "typed exposure and exact uint64 revision accepted");
    require(parse().request->exposure_stops == 1.25, "exposure preserved without clamping");
    request.insert("surprise", true);
    require(!parse().request, "unknown envelope rejected");
    request.remove("surprise");
    auto params = request.value("params").toObject();
    auto edit = params.value("operation").toObject();
    edit.insert("stops", 16.001);
    params.insert("operation", edit);
    request.insert("params", params);
    require(!parse().request, "out of range exposure rejected");
    edit.insert("stops", "1.0");
    params.insert("operation", edit);
    request.insert("params", params);
    require(!parse().request, "numeric strings rejected");
    edit.insert("stops", -16);
    params.insert("operation", edit);
    request.insert("params", params);
    require(parse().request.has_value(), "exact lower bound accepted");
    auto invalid = identity;
    invalid.insert("draftRevision", 1);
    params.insert("expected", invalid);
    request.insert("params", params);
    require(!parse().request, "lossy numeric revision rejected");
    invalid = identity;
    invalid.remove("activeVariantId");
    params.insert("expected", invalid);
    request.insert("params", params);
    require(!parse().request, "missing Variant CAS rejected");
    invalid = identity;
    invalid.insert("workingCommitId", QJsonValue::Null);
    params.insert("expected", invalid);
    request.insert("params", params);
    require(!parse().request, "null head cannot mean missing head");
    require(!EditToolProtocol::parse("[]").request, "nonobject rejected");
    require(!EditToolProtocol::parse(QByteArray(65537, ' ')).request, "oversize request rejected");
    require(
        EditToolProtocol::discovery().value("export").toObject().value("cancel") == "unsupported",
        "export cancellation accurately declared"
    );
    return 0;
}
