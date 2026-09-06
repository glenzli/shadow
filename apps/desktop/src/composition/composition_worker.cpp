#include "composition_engine.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <cstdio>
#include <set>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/linear_raster.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/warm_edit_preview.hpp>
using namespace shadow::image;
using namespace shadow::composition;
namespace {
std::filesystem::path native(const QString& p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    auto b = p.toUtf8();
    return std::filesystem::path(std::string(b.constData(), size_t(b.size())));
#endif
}
void message(QJsonObject o) {
    auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.data(), 1, size_t(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
QString digest(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("input-unavailable");
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        throw std::runtime_error("input-unavailable");
    return QString::fromLatin1(h.result().toHex());
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    cv::setNumThreads(2);
    try {
        if (argc != 3)
            throw std::runtime_error("request-invalid");
        QFile request(QString::fromLocal8Bit(argv[1]));
        if (!request.open(QIODevice::ReadOnly) || request.size() > 64 * 1024)
            throw std::runtime_error("request-invalid");
        auto doc = QJsonDocument::fromJson(request.readAll());
        auto json = doc.object();
        QString mode = json.value("mode").toString();
        auto paths = json.value("inputs").toArray();
        int edge = json.value("maxEdge").toInt(4096);
        if ((mode != "hdr" && mode != "panorama") || paths.size() < 2 || paths.size() > 12
            || (edge != 2048 && edge != 4096))
            throw std::runtime_error("request-invalid");
        QString root = QString::fromLocal8Bit(argv[2]);
        if (!QFileInfo(root).isDir())
            throw std::runtime_error("output-unavailable");
        std::vector<Input> inputs;
        QJsonArray provenance_inputs;
        std::set<QString> unique, content_hashes;
        std::vector<QFileInfo> facts;
        std::vector<QString> digests;
        auto provider = make_photo_decoder_provider();
        AssetMetadata reference;
        uint64_t total_pixels = 0;
        const CameraProfileCatalog linear_profiles{
            .identity = "shadow-composition-radiometric-no-dcp-v1"
        };
        for (int i = 0; i < paths.size(); ++i) {
            const auto entry = paths[i].toObject();
            QString path = entry.value("path").toString();
            QFileInfo info(path);
            QString canonical = info.canonicalFilePath();
            if (!info.isFile() || !info.isReadable() || canonical.isEmpty())
                throw std::runtime_error("input-unavailable");
            if (!unique.insert(canonical).second)
                throw std::runtime_error("duplicate-input");
            facts.push_back(info);
            digests.push_back(digest(path));
            if (!content_hashes.insert(digests.back()).second)
                throw std::runtime_error("duplicate-input");
            message({{"phase", "decode"}, {"index", i}, {"total", paths.size()}});
            auto session = provider->open(native(path));
            const auto metadata = session->metadata();
            if (i == 0)
                reference = metadata;
            double exposure = 1;
            if (mode == "hdr") {
                if (!session->capabilities().raw_frame
                    || !std::isfinite(metadata.exposure_time_seconds)
                    || !std::isfinite(metadata.iso_speed)
                    || !std::isfinite(metadata.aperture_f_number)
                    || metadata.exposure_time_seconds <= 0 || metadata.iso_speed <= 0
                    || metadata.aperture_f_number <= 0)
                    throw std::runtime_error("hdr-metadata");
                if (metadata.normalized_make != reference.normalized_make
                    || metadata.normalized_model != reference.normalized_model
                    || std::abs(metadata.iso_speed / reference.iso_speed - 1) > .01
                    || std::abs(metadata.aperture_f_number / reference.aperture_f_number - 1) > .01)
                    throw std::runtime_error("hdr-camera");
                for (int c = 0; c < 3; ++c)
                    if (reference.as_shot_neutral[c] > 0
                        && std::abs(metadata.as_shot_neutral[c] / reference.as_shot_neutral[c] - 1)
                               > .01)
                        throw std::runtime_error("hdr-white-balance");
                exposure = metadata.exposure_time_seconds;
            }
            const auto d = metadata.image_dimensions;
            double scale = double(edge) / std::max({1U, d.width, d.height});
            scale = std::min(1., scale);
            total_pixels +=
                uint64_t(std::ceil(d.width * scale)) * uint64_t(std::ceil(d.height * scale));
            if (total_pixels > 64ULL * 1024 * 1024)
                throw std::runtime_error("memory-budget");
            RawDevelopmentPlan plan;
            plan.intent = RawDevelopmentIntent::preview;
            plan.quality = RawDevelopmentQuality::balanced;
            RawPipelinePolicy policy;
            if (session->capabilities().raw_frame)
                policy.mode = RawPipelineMode::require_shadow_raw_frame;
            // The existing CFA/highlight developer is unchanged. HDR requires a radiometric
            // input, so omit optional creative DCP tables/curves; keep the camera matrix/WB.
            auto developed = mode == "hdr"
                                 ? develop_source_reference(
                                       *session,
                                       plan,
                                       uint32_t(edge),
                                       policy,
                                       linear_profiles
                                   )
                                 : develop_source_reference(*session, plan, uint32_t(edge), policy);
            Input input;
            input.exposure = exposure;
            if (auto* f = std::get_if<SceneLinearRgbFrame>(&developed.source)) {
                input.rgb = cv::Mat(
                                int(f->dimensions.height),
                                int(f->dimensions.width),
                                CV_32FC3,
                                f->samples.data()
                )
                                .clone();
            } else {
                auto& pixels = std::get<PixelBuffer>(developed.source);
                if (pixels.channels != 3 || pixels.bits_per_channel != 16
                    || pixels.transfer_function != RgbTransferFunction::linear)
                    throw std::runtime_error("invalid-input");
                cv::Mat(
                    int(pixels.dimensions.height),
                    int(pixels.dimensions.width),
                    CV_16UC3,
                    pixels.samples.data(),
                    pixels.row_stride_bytes
                )
                    .convertTo(input.rgb, CV_32FC3, 1. / 65535.);
            }
            input.confidence = cv::Mat(input.rgb.size(), CV_32F, cv::Scalar(1));
            if (developed.sensor_clipping_mask) {
                const auto& mask = *developed.sensor_clipping_mask;
                if (mask.samples.size() != input.rgb.total())
                    throw std::runtime_error("invalid-input");
                auto* w = input.confidence.ptr<float>();
                for (size_t p = 0; p < mask.samples.size(); ++p)
                    if (mask.samples[p] & sensor_highlight_clipped)
                        w[p] = 0;
            }
            inputs.push_back(std::move(input));
            provenance_inputs.append(
                QJsonObject{
                    {"photoId", entry.value("photoId")},
                    {"representationId", entry.value("representationId")},
                    {"name", info.fileName()},
                    {"sha256", digests.back()},
                    {"exposureSeconds", exposure},
                    {"development",
                     QString::fromStdString(
                         raw_pipeline_receipt_identity(developed.pipeline_receipt)
                     )}
                }
            );
        }
        auto progress = [&](const char* phase, int i) {
            message({{"phase", phase}, {"index", i}, {"total", paths.size()}});
        };
        auto result = mode == "hdr"
                          ? merge_hdr(
                                inputs,
                                json.value("align").toBool(true),
                                json.value("deghost").toBool(true),
                                progress
                            )
                          : merge_panorama(inputs, json.value("compensate").toBool(true), progress);
        message({{"phase", "write"}, {"index", paths.size()}, {"total", paths.size()}});
        for (int i = 0; i < paths.size(); ++i) {
            QFileInfo now(facts[i].filePath());
            if (now.size() != facts[i].size() || now.lastModified() != facts[i].lastModified()
                || digest(now.filePath()) != digests[i])
                throw std::runtime_error("input-changed");
        }
        QJsonArray offsets, gains;
        for (auto p : result.offsets)
            offsets.append(QJsonArray{p.x, p.y});
        for (double g : result.gains)
            gains.append(g);
        QJsonObject receipt{
            {"schema", "shadow-photo-composition-v1"},
            {"mode", mode},
            {"inputs", provenance_inputs},
            {"options", json},
            {"offsets", offsets},
            {"gains", gains},
            {"workingSpace", "linear-sRGB-D65"},
            {"originalEditsApplied", false},
            {"opencv", CV_VERSION}
        };
        // Do not persist machine-local source paths in the embedded provenance.
        receipt.remove("options");
        receipt.insert("maxEdge", edge);
        receipt.insert("align", json.value("align"));
        receipt.insert("deghost", json.value("deghost"));
        receipt.insert("compensate", json.value("compensate"));
        SceneLinearRgbFrame frame{
            {uint32_t(result.rgb.cols), uint32_t(result.rgb.rows)},
            size_t(result.rgb.cols) * 12,
            {}
        };
        frame.samples.assign(
            result.rgb.ptr<float>(),
            result.rgb.ptr<float>() + result.rgb.total() * 3
        );
        write_linear_tiff(
            native(QDir(root).filePath("result.tif")),
            frame,
            QJsonDocument(receipt).toJson(QJsonDocument::Compact).toStdString()
        );
        auto completed = open_linear_tiff(native(QDir(root).filePath("result.tif")));
        auto warm = prepare_warm_edit_preview(*completed, 1400);
        auto preview = warm.render_jpeg({}, 92);
        QFile preview_file(QDir(root).filePath("preview.jpg"));
        if (!preview_file.open(QIODevice::WriteOnly)
            || preview_file.write(
                   reinterpret_cast<const char*>(preview.bytes.data()),
                   qint64(preview.bytes.size())
               ) != qint64(preview.bytes.size()))
            throw std::runtime_error("output-unavailable");
        preview_file.close();
        message({{"phase", "ready"}, {"width", result.rgb.cols}, {"height", result.rgb.rows}});
        return 0;
    } catch (const cv::Exception& e) {
        message(
            {{"phase", "error"},
             {"code", "alignment-failed"},
             {"diagnostic", QString::fromUtf8(e.what()).left(1000)}}
        );
        return 1;
    } catch (const std::exception& e) {
        message({{"phase", "error"}, {"code", QString::fromUtf8(e.what()).left(200)}});
        return 1;
    }
}
