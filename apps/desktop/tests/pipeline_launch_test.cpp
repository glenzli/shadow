#include "pipeline_launch.hpp"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdlib>

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
    return EXIT_SUCCESS;
}
