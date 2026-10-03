#include "edit_tool_protocol.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
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
    require(
        parse().request->edits.size() == 1 && parse().request->edits.front().value == 1.25,
        "exposure preserved without clamping"
    );
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
    const auto composition = [&](const QJsonArray& edits) {
        request.insert(
            "params",
            QJsonObject{
                {"expected", identity},
                {"operations", edits},
                {"outputPath", "/tmp/composed.jpg"}
            }
        );
    };
    const QJsonObject exposure{{"type", "set_exposure"}, {"nodeId", "node"}, {"stops", 1.0}};
    const QJsonObject contrast{{"type", "set_contrast"}, {"nodeId", "node"}, {"factor", 1.2}};
    const QJsonObject saturation{{"type", "set_saturation"}, {"nodeId", "node"}, {"factor", 0.75}};
    composition({exposure, contrast, saturation});
    require(
        parse().request && parse().request->edits.size() == 3,
        "distinct parameters form one proposal"
    );
    require(
        parse().request->edits.at(1).kind == EditToolProtocol::EditKind::Contrast
            && parse().request->edits.at(2).value == 0.75,
        "typed composition values preserved"
    );
    auto mixed = request.value("params").toObject();
    mixed.insert("operation", exposure);
    request.insert("params", mixed);
    require(!parse().request, "singular and array forms cannot be mixed");
    composition({});
    require(!parse().request, "empty composition rejected");
    composition({exposure, exposure});
    require(!parse().request, "duplicate node parameter rejected");
    auto malformed = contrast;
    malformed.insert("factor", -0.01);
    composition({exposure, malformed});
    require(!parse().request, "invalid trailing adjustment rejects whole proposal");
    malformed = saturation;
    malformed.insert("factor", 8.01);
    composition({malformed});
    require(!parse().request, "saturation upper bound enforced");
    malformed.insert("factor", 8.0);
    composition({malformed});
    require(parse().request.has_value(), "saturation exact upper bound accepted");
    malformed = contrast;
    malformed.insert("factor", false);
    composition({malformed});
    require(!parse().request, "boolean factor rejected");
    malformed = contrast;
    malformed.insert("stops", 1.0);
    composition({malformed});
    require(!parse().request, "unit confusion rejected");
    composition({exposure, QJsonValue::Null});
    require(!parse().request, "null adjustment rejected");
    QJsonArray bounded;
    for (int index = 0; index < EditToolProtocol::maximumEdits; ++index) {
        auto item = exposure;
        item.insert("nodeId", QString::number(index));
        bounded.append(item);
    }
    composition(bounded);
    require(parse().request.has_value(), "bounded cross-node composition accepted");
    bounded.append(exposure);
    composition(bounded);
    require(!parse().request, "oversize composition rejected");
    require(
        EditToolProtocol::discovery().value("edits").toArray().size() == 3,
        "supported adjustments discoverable"
    );
    require(
        EditToolProtocol::discovery().value("proposalLifetime").toObject().value("snapshot")
            == "reuse_while_current",
        "snapshot observation lifetime is discoverable"
    );
    require(!EditToolProtocol::parse("[]").request, "nonobject rejected");
    require(!EditToolProtocol::parse(QByteArray(65537, ' ')).request, "oversize request rejected");
    require(
        EditToolProtocol::discovery().value("export").toObject().value("cancel") == "unsupported",
        "export cancellation accurately declared"
    );
    return 0;
}
