#include "storage_profiles.hpp"

#include "../desktop_backend.hpp"
#include "../edit_controller.hpp"
#include "../storage_profile_controller.hpp"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>

namespace DesktopSmoke {
namespace {
struct Admission {
    DesktopBackend::PipelineInput input;
    QString error;
};
class ProfileSwitchSmoke final : public QObject {
  public:
    ProfileSwitchSmoke(
        QApplication& app,
        QQmlApplicationEngine& engine,
        const std::shared_ptr<DesktopBackend>& backend,
        EditController& editor,
        StorageProfileController& profiles,
        QString source,
        QString target,
        bool verify,
        double exposure
    ) :
        app_(app), editor_(editor), profiles_(profiles), engine_(engine),
        target_(std::move(target)), verify_(verify), exposure_(exposure) {
        lifetime_.start();
        connect(&admission_, &QFutureWatcher<Admission>::finished, this, [this] {
            const auto result = admission_.result();
            if (!result.error.isEmpty()) {
                fail(result.error);
                return;
            }
            const auto& input = result.input;
            if (!editor_.openPhoto(
                    input.photo_id,
                    input.representation_id,
                    input.source_path,
                    input.title
                )) {
                fail("photo admission failed");
                return;
            }
            QMetaObject::invokeMethod(engine_.rootObjects().front(), "showPrecision");
            stage_ = 1;
        });
        connect(&profiles_, &StorageProfileController::switched, this, [this] {
            if (stage_ != 2 || editor_.dirty() || editor_.autosaveFailed()) {
                fail("target activated before working snapshot was durable");
                return;
            }
            qInfo() << "Profile switch smoke: saved working edits and activated target";
            timer_.stop();
        });
        timer_.setInterval(50);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
        admission_.setFuture(QtConcurrent::run([backend, source] {
            Admission result;
            try {
                result.input = backend->admitPipelineInput(source);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
    }
    ~ProfileSwitchSmoke() override {
        admission_.waitForFinished();
    }

  private:
    void fail(const QString& reason) {
        timer_.stop();
        qCritical().noquote() << "Profile switch smoke failed:" << reason;
        app_.exit(EXIT_FAILURE);
    }
    void tick() {
        if (lifetime_.elapsed() > 90'000) {
            fail("timeout");
            return;
        }
        if (stage_ == 2) {
            if (!profiles_.busy() && !profiles_.errorText().isEmpty())
                fail(profiles_.errorText());
            return;
        }
        if (stage_ != 1 || !editor_.active() || editor_.busy() || editor_.gradeNodes().isEmpty())
            return;
        if (verify_) {
            if (std::abs(editor_.exposureStops() - exposure_) > 0.00001) {
                fail("reopened working exposure differs");
                return;
            }
            qInfo() << "Profile switch smoke: reopened durable working exposure" << exposure_;
            timer_.stop();
            app_.quit();
            return;
        }
        const auto nodes = editor_.gradeNodes();
        int enabled = -1;
        for (const auto& value : nodes) {
            const auto node = value.toMap();
            if (node.value("enabled").toBool()) {
                enabled = node.value("index").toInt();
                break;
            }
        }
        if (enabled < 0) {
            fail("no enabled grade node");
            return;
        }
        if (editor_.selectedGradeNodeIndex() != enabled) {
            editor_.selectGradeNode(enabled);
            return;
        }
        editor_.setExposureStops(editor_.exposureStops() + 0.25);
        if (!editor_.dirty()) {
            fail("could not make working edits");
            return;
        }
        auto* dialog = engine_.rootObjects().front()->findChild<QObject*>("storageProfilesDialog");
        if (!dialog || !QMetaObject::invokeMethod(dialog, "open")) {
            fail("packaged profile dialog missing");
            return;
        }
        stage_ = 2;
        profiles_.switchLibrary(target_, {});
    }
    QApplication& app_;
    EditController& editor_;
    StorageProfileController& profiles_;
    QQmlApplicationEngine& engine_;
    QString target_;
    bool verify_;
    double exposure_;
    int stage_ = 0;
    QElapsedTimer lifetime_;
    QTimer timer_;
    QFutureWatcher<Admission> admission_;
};
} // namespace

std::unique_ptr<QObject> installStorageProfileSwitch(
    QApplication& application,
    QQmlApplicationEngine& engine,
    const std::shared_ptr<DesktopBackend>& backend,
    EditController& editor,
    StorageProfileController& profiles
) {
    const QString target = qEnvironmentVariable("SHADOW_DESKTOP_PROFILE_SWITCH_TARGET");
    const bool verify = qEnvironmentVariableIsSet("SHADOW_DESKTOP_PROFILE_VERIFY_EXPOSURE");
    if (target.isEmpty() && !verify)
        return {};
    const QString source = qEnvironmentVariable("SHADOW_DESKTOP_EDIT_SOURCE_PATH");
    if (profiles.currentKind() == "daily" || source.isEmpty()) {
        qCritical() << "Profile switch smoke requires an isolated library and one named source";
        QTimer::singleShot(0, &application, [&application] { application.exit(EXIT_FAILURE); });
        return {};
    }
    bool valid_exposure = true;
    const double exposure = verify ? qEnvironmentVariable("SHADOW_DESKTOP_PROFILE_VERIFY_EXPOSURE")
                                         .toDouble(&valid_exposure)
                                   : 0.0;
    if (!valid_exposure) {
        QTimer::singleShot(0, &application, [&application] { application.exit(EXIT_FAILURE); });
        return {};
    }
    return std::make_unique<ProfileSwitchSmoke>(
        application,
        engine,
        backend,
        editor,
        profiles,
        source,
        target,
        verify,
        exposure
    );
}
} // namespace DesktopSmoke
