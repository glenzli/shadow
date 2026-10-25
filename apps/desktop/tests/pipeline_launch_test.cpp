#include "pipeline_launch.hpp"
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdlib>
#include <stdexcept>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool require(bool condition, const char* message) {
    if (!condition)
        qCritical() << message;
    return condition;
}
} // namespace
int main() {
    QTemporaryDir directory;
    if (!directory.isValid())
        return EXIT_FAILURE;
    const QDir canonical(QDir(directory.path()).canonicalPath());
    const QString input = canonical.filePath("input.jpg");
    const QString second = canonical.filePath("second.jpg");
    const QString request = canonical.filePath("request.json");
    const QString result = canonical.filePath("result.json");
    const QString output = canonical.filePath("output.jpg");
    if (!writeFile(input, "parser fixture") || !writeFile(second, "parser fixture"))
        return EXIT_FAILURE;
    QJsonObject object{
        {"schema", "shadow-pipeline-edit-20260814.1"},
        {"requestId", "contract"},
        {"input", input},
        {"output", output},
        {"export", QJsonObject{{"format", "jpeg"}}}
    };
    const QStringList args{"Shadow", "--pipeline-edit", "--request", request, "--result", result};
    QString error;
    const auto parse = [&] {
        writeFile(request, QJsonDocument(object).toJson());
        return parsePipelineLaunch(args, &error);
    };
    const auto legacy = parse();
    if (!require(
            legacy && legacy->legacy_single && legacy->photos.size() == 1
                && legacy->photos[0].input_path == input && legacy->photos[0].output_path == output
                && legacy->result_path == result && error.isEmpty(),
            "legacy contract changed"
        ))
        return EXIT_FAILURE;
    auto normal = parsePipelineLaunch({"Shadow"}, &error);
    if (!require(!normal && error.isEmpty(), "normal launch captured"))
        return EXIT_FAILURE;
    auto direct = parsePipelineLaunch({"Shadow", "--isolate", input, second, input}, &error);
    if (!require(
            direct && direct->interactive && direct->photos.size() == 2,
            "direct deduplication"
        ))
        return EXIT_FAILURE;
    if (!require(
            parsePipelineLaunch({"Shadow", "--isolate"}, &error).has_value(),
            "empty interactive launch"
        ))
        return EXIT_FAILURE;
    if (!require(
            !parsePipelineLaunch({"Shadow", "--isolate", "--bad"}, &error),
            "unknown flag accepted"
        ))
        return EXIT_FAILURE;
    if (!require(
            !parsePipelineLaunch({"Shadow", "--isolate", "--pipeline-edit"}, &error),
            "mixed modes accepted"
        ))
        return EXIT_FAILURE;
    if (!require(
            !parsePipelineLaunch({"Shadow", "--pipeline-edit", "--request", request}, &error),
            "missing result accepted"
        ))
        return EXIT_FAILURE;
    object["operations"] = QJsonArray{QJsonObject{{"exposure", 1.0}}};
    if (!require(!parse() && error.contains("unsupported"), "unsupported edit intent ignored"))
        return EXIT_FAILURE;
    object.remove("operations");
    object["export"] = QJsonObject{{"format", "png"}, {"width", 1024}};
    if (!require(!parse() && error.contains("unsupported"), "unsupported export option ignored"))
        return EXIT_FAILURE;
    object["export"] = QJsonObject{{"format", "png"}, {"filenameSuffix", "-adjusted"}};
    if (!require(!parse() && error.contains("unsupported"), "unused filename suffix accepted"))
        return EXIT_FAILURE;
    object["export"] = QJsonObject{{"format", "jpeg"}};
    object["output"] = result;
    if (!require(!parse(), "result/output collision accepted"))
        return EXIT_FAILURE;
    object["output"] = input;
    if (!require(!parse(), "original overwrite accepted"))
        return EXIT_FAILURE;
    object["output"] = output;
    object["schema"] = "shadow-pipeline-edit-20260920.1";
    const QJsonObject first{{"input", input}, {"output", output}};
    const QJsonObject next{{"input", second}, {"output", canonical.filePath("second-out.jpg")}};
    object["photos"] = QJsonArray{first, next};
    QJsonObject with_recipe = first;
    with_recipe["recipe"] = "unimplemented.shadowrecipe";
    object["photos"] = QJsonArray{with_recipe, next};
    if (!require(!parse() && error.contains("unsupported"), "unsupported per-photo recipe ignored"))
        return EXIT_FAILURE;
    object["photos"] = QJsonArray{first, next};
    const auto batch = parse();
    if (!require(batch && batch->photos.size() == 2 && !batch->legacy_single, "batch admission"))
        return EXIT_FAILURE;
    if (!require(writePipelineResult(*batch, "cancelled", {}, {output}), "result publication"))
        return EXIT_FAILURE;
    QFile receiptFile(result);
    if (!receiptFile.open(QIODevice::ReadOnly))
        return EXIT_FAILURE;
    const QByteArray originalReceipt = receiptFile.readAll();
    receiptFile.close();
    const auto receipt = QJsonDocument::fromJson(originalReceipt).object();
    if (!require(
            receipt["photos"].toArray()[0].toObject()["outcome"] == "completed"
                && receipt["photos"].toArray()[1].toObject()["outcome"] == "cancelled",
            "partial completion receipt"
        ))
        return EXIT_FAILURE;
    if (!require(
            !writePipelineResult(*batch, "failed", {"late result"}),
            "result overwrite accepted"
        ))
        return EXIT_FAILURE;
    if (!receiptFile.open(QIODevice::ReadOnly) || receiptFile.readAll() != originalReceipt)
        return EXIT_FAILURE;
    receiptFile.close();
    if (!QFile::remove(result))
        return EXIT_FAILURE;
    object["photos"] = QJsonArray{first, first};
    if (!require(!parse(), "duplicate batch paths"))
        return EXIT_FAILURE;
    QJsonObject alias = next;
    alias["output"] = result;
    object["photos"] = QJsonArray{first, alias};
    if (!require(!parse(), "batch result collision"))
        return EXIT_FAILURE;
    object["photos"] = QJsonArray{};
    if (!require(!parse(), "empty batch accepted"))
        return EXIT_FAILURE;
    QJsonArray too_many;
    for (int i = 0; i < 257; ++i)
        too_many.append(first);
    object["photos"] = too_many;
    if (!require(!parse(), "oversized batch accepted"))
        return EXIT_FAILURE;
    object["photos"] = QJsonArray{first};
    object["export"] = 42;
    if (!require(!parse(), "non-object settings accepted"))
        return EXIT_FAILURE;
    object["export"] = QJsonObject{{"format", "dng"}};
    if (!require(!parse(), "unedited DNG output admitted as edited export"))
        return EXIT_FAILURE;
    object["export"] = QJsonObject{{"format", "jpeg"}};
    writeFile(output, "do not replace");
    if (!require(!parse(), "existing output accepted"))
        return EXIT_FAILURE;
#if defined(Q_OS_UNIX)
    const auto tool = parsePipelineLaunch({"Shadow", "--agent-stdio", "--isolate", input}, &error);
    if (!require(
            tool && tool->agent_stdio && tool->photos.size() == 1,
            "formal tool entry missing"
        ))
        return EXIT_FAILURE;
#endif
    if (!require(
            !parsePipelineLaunch({"Shadow", "--agent-stdio", input}, &error),
            "implicit Library tool scope accepted"
        )
        || !require(
            !parsePipelineLaunch({"Shadow", "--agent-stdio", "--isolate"}, &error),
            "empty tool scope accepted"
        )
        || !require(
            !parsePipelineLaunch({"Shadow", "--agent-stdio", "--isolate", input, second}, &error),
            "multi-photo tool scope accepted"
        )
        || !require(
            !parsePipelineLaunch(
                {"Shadow", "--agent-stdio", "--agent-stdio", "--isolate", input},
                &error
            ),
            "duplicate tool option accepted"
        ))
        return EXIT_FAILURE;
    const auto png = canonical.filePath("material.png");
    const auto derived = canonical.filePath("derived.png");
    const QByteArray source_bytes("bounded source snapshot fixture");
    writeFile(png, source_bytes);
    const auto hash = QString::fromLatin1(
        QCryptographicHash::hash(source_bytes, QCryptographicHash::Sha256).toHex()
    );
    object = {
        {"schema", "shadow.external-edit/1"},
        {"requestId", "shape-session"},
        {"input", QJsonObject{{"path", png}, {"revision", "version-7"}, {"sha256", hash}}},
        {"output", QJsonObject{{"path", derived}}}
    };
    auto external = parse();
    if (!require(
            external && external->external && !external->interactive,
            "external stage request missing"
        ))
        return EXIT_FAILURE;
#if defined(Q_OS_UNIX)
    if (!require(
            parsePipelineLaunch(args + QStringList{"--agent-stdio"}, &error).has_value(),
            "external stage tools rejected"
        ))
        return EXIT_FAILURE;
#endif
    auto binding = *external->external;
    binding.private_source_path = canonical.filePath("private.png");
    try {
        ExternalEditContract::snapshotSource(binding, std::atomic_bool(false));
        writeFile(png, "changed original");
        bool rejected = false;
        try {
            ExternalEditContract::verifyOriginal(binding);
        } catch (const std::exception&) {
            rejected = true;
        }
        QFile snapshot(binding.private_source_path);
        if (!require(
                rejected && snapshot.open(QIODevice::ReadOnly)
                    && snapshot.readAll() == source_bytes,
                "input change altered the pinned source or passed digest verification"
            ))
            return EXIT_FAILURE;
    } catch (const std::exception& exception) {
        qCritical() << exception.what();
        return EXIT_FAILURE;
    }
    object["unknown"] = true;
    if (!require(!parse(), "external unknown field accepted"))
        return EXIT_FAILURE;
    object.remove("unknown");
    auto bad_input = object.value("input").toObject();
    bad_input["sha256"] = "bad";
    object["input"] = bad_input;
    if (!require(!parse(), "malformed digest accepted"))
        return EXIT_FAILURE;
    if (!require(
            writePipelineResult(*external, "cancelled", {}),
            "cancelled external receipt missing"
        ))
        return EXIT_FAILURE;
    QFile returned(result);
    if (!returned.open(QIODevice::ReadOnly))
        return EXIT_FAILURE;
    const auto envelope = QJsonDocument::fromJson(returned.readAll()).object();
    if (!require(
            envelope.value("schema") == "shadow.external-edit-result/1"
                && envelope.value("input").toObject().value("revision") == "version-7"
                && !envelope.contains("artifact"),
            "cancelled external receipt fabricated an artifact"
        ))
        return EXIT_FAILURE;
    {
        QTemporaryDir documents;
        if (!documents.isValid())
            return EXIT_FAILURE;
        const QDir root(QDir(documents.path()).canonicalPath());
        const auto image = root.filePath("input.png");
        const auto document = root.filePath("input.shdw");
        const auto instruction = root.filePath("request.json");
        const auto receipt = root.filePath("result.json");
        writeFile(image, "parser fixture");
        writeFile(document, "ZIP fixture");
        QJsonObject request{
            {"schema", "shadow.document-edit/1"},
            {"requestId", "document"},
            {"input", image},
            {"inputSha256", QString(64, 'a')},
            {"output", root.filePath("new.shdw")}
        };
        const auto parse_document = [&] {
            writeFile(instruction, QJsonDocument(request).toJson());
            return parsePipelineLaunch(
                {"Shadow", "--pipeline-edit", "--request", instruction, "--result", receipt},
                &error
            );
        };
        auto parsed = parse_document();
        if (!require(
                parsed && parsed->document_protocol && parsed->photos.size() == 1,
                "document image admission failed"
            ))
            return EXIT_FAILURE;
        request["input"] = document;
        parsed = parse_document();
        if (!require(
                parsed && parsed->document_input == document && parsed->photos.isEmpty(),
                "document continuation admission failed"
            ))
            return EXIT_FAILURE;
        request["operations"] = QJsonArray{};
        if (!require(!parse_document(), "document unsupported operation intent accepted"))
            return EXIT_FAILURE;
        request.remove("operations");
        request["inputSha256"] = "invalid";
        if (!require(!parse_document(), "document invalid input digest accepted"))
            return EXIT_FAILURE;
        parsed = parsePipelineLaunch({"Shadow", document}, &error);
        if (!require(
                parsed && parsed->interactive && parsed->document_input == document,
                "document positional launch failed"
            ))
            return EXIT_FAILURE;
        if (!require(
                !parsePipelineLaunch({"Shadow", "--isolate", document, image}, &error),
                "mixed document/photo session admitted"
            ))
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
