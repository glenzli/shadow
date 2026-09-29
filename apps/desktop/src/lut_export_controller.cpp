#include "lut_export_controller.hpp"

#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QtConcurrentRun>

#include <exception>
#include <utility>

LutExportController::LutExportController(Prepare prepare, QObject* parent)
    : QObject(parent), prepare_(std::move(prepare)) {
    connect(&watcher_, &QFutureWatcher<BackendLutExportResult>::finished, this, [this] {
        busy_ = false;
        if (pending_prepare_) {
            const bool selected_only = *pending_prepare_;
            pending_prepare_.reset();
            this->prepare(selected_only);
            return;
        }
        if (cancelled_) {
            result_ = {};
            error_ = tr("LUT export cancelled.");
        } else {
            result_ = watcher_.result();
            if (!result_.error.isEmpty()) {
                error_ = tr("The LUT could not be generated: %1").arg(result_.error);
            }
        }
        emit stateChanged();
    });
}

LutExportController::~LutExportController() {
    cancel();
    watcher_.waitForFinished();
}

void LutExportController::prepare(bool selected_only) {
    if (busy()) {
        cancel();
        pending_prepare_ = selected_only;
        return;
    }
    snapshot_ = {};
    result_ = {};
    error_.clear();
    saved_path_.clear();
    cancelled_ = false;
    try {
        snapshot_ = prepare_(selected_only);
    } catch (const std::exception& error) {
        error_ = tr("The grading snapshot could not be prepared: %1").arg(QString::fromUtf8(error.what()));
    }
    emit stateChanged();
}

void LutExportController::bake(int size) {
    if (!canBake() || !snapshot_.bake) { return; }
    if (size != 17 && size != 33 && size != 65) {
        error_ = tr("Choose a 17, 33, or 65 point LUT.");
        emit stateChanged();
        return;
    }
    result_ = {};
    error_.clear();
    saved_path_.clear();
    busy_ = true;
    watcher_.setFuture(QtConcurrent::run([run = snapshot_.bake, size] {
        try {
            return run(static_cast<std::uint16_t>(size));
        } catch (const std::exception& error) {
            return BackendLutExportResult{.error = QString::fromUtf8(error.what())};
        }
    }));
    emit stateChanged();
}

void LutExportController::cancel() {
    pending_prepare_.reset();
    cancelled_ = true;
    if (snapshot_.cancel) { snapshot_.cancel(); }
    result_ = {};
    emit stateChanged();
}

bool LutExportController::save(const QUrl& destination) {
    if (!ready() || busy()) { return false; }
    QString path = destination.toLocalFile();
    if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) { path += QStringLiteral(".cube"); }
    if (path.isEmpty() || QFileInfo(path).suffix().compare(QStringLiteral("cube"), Qt::CaseInsensitive) != 0) {
        error_ = tr("Choose a local .cube destination.");
        emit stateChanged();
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(result_.document) != result_.document.size() || !file.commit()) {
        file.cancelWriting();
        error_ = tr("The complete LUT could not be saved atomically.");
        emit stateChanged();
        return false;
    }
    error_.clear();
    saved_path_ = path;
    emit stateChanged();
    return true;
}

QString LutExportController::measurementText() const {
    if (!ready()) { return {}; }
    const QLocale locale;
    return tr("%1 independent RGB probes · measured maximum error %2 · RMS error %3")
        .arg(result_.probe_count)
        .arg(locale.toString(result_.maximum_absolute_error, 'g', 4))
        .arg(locale.toString(result_.root_mean_square_error, 'g', 4));
}

QString LutExportController::omissionReason(const QString& reason) const {
    if (reason == QStringLiteral("maskedNode")) { return tr("Complete masked node omitted"); }
    if (reason == QStringLiteral("regionalTone")) { return tr("Guided highlights, shadows, whites, blacks, and highlight channel repair omitted"); }
    if (reason == QStringLiteral("technicalDetail")) { return tr("Texture, clarity, local contrast, dehaze, sharpening, denoise, and defringe omitted"); }
    if (reason == QStringLiteral("finishingEffects")) { return tr("Grain and vignette omitted"); }
    if (reason == QStringLiteral("bypassed")) { return tr("Disabled or zero-strength node skipped"); }
    if (reason == QStringLiteral("foundation")) { return tr("Source development and Foundation omitted"); }
    if (reason == QStringLiteral("rawDenoise")) { return tr("RAW denoise omitted"); }
    if (reason == QStringLiteral("repair")) { return tr("Repair omitted"); }
    if (reason == QStringLiteral("completion")) { return tr("AI completion omitted"); }
    if (reason == QStringLiteral("liquify")) { return tr("Liquify omitted"); }
    if (reason == QStringLiteral("canvas")) { return tr("Crop and geometry omitted"); }
    return tr("Unsupported adjustment omitted");
}

QVariantList LutExportController::omissions() const {
    QVariantList result;
    for (const auto& item : snapshot_.omissions) {
        result.append(QVariantMap{{QStringLiteral("nodeLabel"), item.node_label},
            {QStringLiteral("reason"), omissionReason(item.reason)}});
    }
    return result;
}
