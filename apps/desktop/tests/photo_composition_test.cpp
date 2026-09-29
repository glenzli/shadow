#include "../src/composition_controller.hpp"
#include "composition_engine.hpp"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <shadow/image/linear_raster.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/warm_edit_preview.hpp>
using namespace shadow::composition;
using namespace shadow::image;
static std::vector<Input> panoramaInputs() {
    cv::theRNG().state = 12345;
    cv::Mat scene(900, 1800, CV_32FC3, cv::Scalar(.1, .1, .1));
    cv::RNG rng(334);
    for (int i = 0; i < 1200; ++i) {
        cv::Point p(rng.uniform(10, 1790), rng.uniform(10, 890));
        cv::Scalar color(rng.uniform(.05, .9), rng.uniform(.05, .9), rng.uniform(.05, .9));
        cv::circle(scene, p, rng.uniform(2, 10), color, -1);
    }
    std::vector<Input> in;
    for (double yaw : {-.20, .20}) {
        cv::Mat mapx(480, 640, CV_32F), mapy(480, 640, CV_32F);
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 640; ++x) {
                double rx = (x - 320) / 650., ry = (y - 240) / 650.;
                double xx = std::cos(yaw) * rx + std::sin(yaw),
                       zz = -std::sin(yaw) * rx + std::cos(yaw);
                mapx.at<float>(y, x) = float(900 + xx / zz * 900);
                mapy.at<float>(y, x) = float(450 + ry / zz * 900);
            }
        cv::Mat view;
        cv::remap(scene, view, mapx, mapy, cv::INTER_LINEAR);
        in.push_back({view, {}, 1});
    }
    return in;
}
class PhotoCompositionTest final : public QObject {
    Q_OBJECT
  private slots:
    void hdr_recovers_superwhite_without_channel_selection() {
        std::vector<Input> in;
        for (double exposure : {.25, 1., 4.}) {
            Input f;
            f.exposure = exposure;
            f.rgb = cv::Mat(40, 64, CV_32FC3);
            f.confidence = cv::Mat(40, 64, CV_32F);
            for (int y = 0; y < 40; ++y)
                for (int x = 0; x < 64; ++x) {
                    float s = .02f + float(x) / 63 * 3.3f;
                    cv::Vec3f color(s, s * .7f, s * .3f);
                    auto v = color * exposure;
                    bool clipped = v[0] >= 1;
                    for (int c = 0; c < 3; ++c)
                        v[c] = std::min(v[c], 1.f);
                    f.rgb.at<cv::Vec3f>(y, x) = v;
                    f.confidence.at<float>(y, x) = clipped ? 0 : 1;
                }
            in.push_back(f);
        }
        auto out = merge_hdr(in, false, false).rgb;
        for (int x = 0; x < 64; ++x) {
            float s = .02f + float(x) / 63 * 3.3f;
            auto v = out.at<cv::Vec3f>(20, x);
            QVERIFY(std::abs(v[0] - s) < 1e-5);
            QVERIFY(std::abs(v[1] - s * .7f) < 1e-5);
            QVERIFY(std::abs(v[2] - s * .3f) < 1e-5);
        }
        QVERIFY(out.at<cv::Vec3f>(20, 63)[0] > 3);
    }
    void deghost_preserves_reference_moving_object() {
        std::vector<Input> in;
        for (double e : {.25, 1., 4.})
            in.push_back(
                {cv::Mat(32, 32, CV_32FC3, cv::Scalar(.1 * e, .1 * e, .1 * e)),
                 cv::Mat(32, 32, CV_32F, cv::Scalar(1)),
                 e}
            );
        in[1].rgb.at<cv::Vec3f>(10, 10) = {.7, .3, .2};
        auto out = merge_hdr(in, false, true).rgb.at<cv::Vec3f>(10, 10);
        QVERIFY(cv::norm(out - cv::Vec3f(.7, .3, .2)) < 1e-6);
    }
    void invalid_brackets_fail_explicitly() {
        Input f{cv::Mat(32, 32, CV_32FC3, cv::Scalar(.1, .1, .1)), {}, 1};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, merge_hdr({f}, false, false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, merge_hdr({f, f}, false, false));
        auto second = f;
        second.exposure = 2;
        second.rgb = cv::Mat(30, 30, CV_32FC3, cv::Scalar(.2, .2, .2));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, merge_hdr({f, second}, false, false));
    }
    void float_tiff_survives_routing_and_rebinding() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto path = std::filesystem::path(dir.path().toStdString()) / "composite.tif";
        SceneLinearRgbFrame f{{64, 48}, 64 * 12, std::vector<float>(64 * 48 * 3)};
        for (size_t i = 0; i < f.samples.size(); i += 3) {
            f.samples[i] = 4.25;
            f.samples[i + 1] = .4;
            f.samples[i + 2] = -.01;
        }
        write_linear_tiff(path, f, "{}");
        auto exact = read_linear_tiff(path);
        QVERIFY(exact.samples == f.samples);
        auto small = read_linear_tiff(path, 16);
        QCOMPARE(small.dimensions.width, 16U);
        QVERIFY(std::abs(small.samples[0] - 4.25) < 1e-5);
        auto provider = make_photo_decoder_provider();
        auto session = provider->open(path);
        auto developed = develop_source_reference(*session, {});
        QVERIFY(std::holds_alternative<SceneLinearRgbFrame>(developed.source));
        QCOMPARE(std::get<SceneLinearRgbFrame>(developed.source).samples[0], 4.25f);
        auto warm = prepare_warm_edit_preview(*session, 32);
        auto jpg = warm.render_jpeg({});
        QVERIFY(!jpg.bytes.empty());
    }
    void panorama_joins_rotated_overlapping_views() {
        auto in = panoramaInputs();
        auto out = merge_panorama(in, true).rgb;
        QVERIFY(out.cols > in[0].rgb.cols);
        QVERIFY(out.rows > 300);
        QVERIFY(cv::checkRange(out));
    }
    void hdr_alignment_recovers_known_translation() {
        cv::Mat base(480, 640, CV_32FC3);
        cv::RNG rng(75);
        rng.fill(base, cv::RNG::UNIFORM, .04, .8);
        cv::GaussianBlur(base, base, cv::Size(9, 9), 0);
        cv::Mat shifted;
        cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, 13, 0, 1, -7);
        cv::warpAffine(base, shifted, transform, base.size());
        shifted *= .25;
        auto out = merge_hdr({{shifted, {}, .25}, {base, {}, 1}}, true, false);
        QCOMPARE(out.offsets[0].x, -13);
        QCOMPARE(out.offsets[0].y, 7);
        QCOMPARE(out.rgb.cols, 627);
        QCOMPARE(out.rgb.rows, 473);
        QVERIFY(cv::norm(out.rgb, base(cv::Rect(0, 7, 627, 473)), cv::NORM_INF) < 1e-5);
    }
    void worker_controller_saves_without_overwriting_and_cancels() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVariantList targets;
        auto inputs = panoramaInputs();
        for (size_t i = 0; i < inputs.size(); ++i) {
            QString name = dir.filePath(QString("view-%1.tif").arg(i));
            auto& m = inputs[i].rgb;
            SceneLinearRgbFrame f{{uint32_t(m.cols), uint32_t(m.rows)}, size_t(m.cols) * 12, {}};
            f.samples.assign(m.ptr<float>(), m.ptr<float>() + m.total() * 3);
            write_linear_tiff(std::filesystem::path(name.toStdString()), f, "{}");
            targets.append(QVariantMap{{"sourcePath", name}, {"photoId", QString::number(i)}});
        }
        CompositionController controller;
        controller.prepare(targets, "panorama");
        controller.start(2048, true, true, true);
        QVERIFY(controller.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 30000);
        QVERIFY2(controller.ready(), qPrintable(controller.error()));
        QFile original(targets[0].toMap().value("sourcePath").toString());
        QVERIFY(original.open(QIODevice::ReadOnly));
        auto before = original.readAll();
        original.close();
        controller.save(QUrl::fromLocalFile(original.fileName()));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(controller.ready());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), before);
        original.close();
        QSignalSpy saved(&controller, &CompositionController::saved);
        QString destination = dir.filePath("joined.tif");
        controller.save(QUrl::fromLocalFile(destination));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(controller.savedPath(), destination);
        QCOMPARE(saved.at(0).at(0).toUrl(), QUrl::fromLocalFile(destination));
        auto read = read_linear_tiff(std::filesystem::path(destination.toStdString()));
        QVERIFY(read.dimensions.width > 640);
        controller.prepare(targets, "hdr");
        controller.start(2048, true, true, true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.ready());
        QVERIFY(controller.error().contains("RAW"));
        controller.prepare(targets, "panorama");
        controller.start(2048, true, true, true);
        controller.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.ready());
        QVERIFY(controller.preview().isEmpty());
        QCOMPARE(saved.size(), 1);
    }
    void unrelated_panorama_is_rejected() {
        Input a{cv::Mat(240, 320, CV_32FC3, cv::Scalar(.1, .1, .1)), {}, 1};
        Input b{cv::Mat(240, 320, CV_32FC3, cv::Scalar(.8, .8, .8)), {}, 1};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, merge_panorama({a, b}, true));
    }
};
QTEST_GUILESS_MAIN(PhotoCompositionTest)
#include "photo_composition_test.moc"
