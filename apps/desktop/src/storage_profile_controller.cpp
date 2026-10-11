#include "storage_profile_controller.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QVariantMap>

#include <utility>

StorageProfileController::StorageProfileController(
    StorageProfile& current,
    StorageProfileRegistry& registry,
    QString context,
    SwitchHooks hooks,
    QObject* parent
) :
    QObject(parent), current_(current), registry_(registry), context_(std::move(context)),
    hooks_(std::move(hooks)) {
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, &StorageProfileController::advance);
    connect(
        &restart_,
        &StorageProfileRestart::activated,
        this,
        &StorageProfileController::restartActivated
    );
    connect(
        &restart_,
        &StorageProfileRestart::failed,
        this,
        &StorageProfileController::restartFailed
    );
    QCoreApplication::instance()->installEventFilter(this);
    refresh();
}

QString StorageProfileController::currentName() const {
    return current_.info().name();
}
QString StorageProfileController::currentRoot() const {
    return current_.root();
}
QString StorageProfileController::currentKind() const {
    return current_.kind();
}
bool StorageProfileController::debugLaunch() const {
    return context_ == "debug";
}
bool StorageProfileController::busy() const {
    return state_ != State::Idle;
}
bool StorageProfileController::canCancel() const {
    return state_ == State::WaitingJobs;
}
QString StorageProfileController::errorText() const {
    return error_;
}

QString StorageProfileController::statusText() const {
    switch (state_) {
    case State::WaitingJobs:
        return tr("Pausing background work and waiting for current operations…");
    case State::Saving:
        return tr("Saving the current edits…");
    case State::Starting:
    case State::Leaving:
        return tr("Opening %1…").arg(target_.name());
    case State::Idle:
        return {};
    }
    return {};
}

QVariantList StorageProfileController::recentProfiles() const {
    QVariantList rows;
    for (const auto& stored : recent_) {
        QString diagnostic;
        auto actual = StorageProfile::inspect(stored.root, registry_.dailyRoot(), &diagnostic);
        const bool uninitialized_builtin =
            stored.id.isEmpty() && stored.kind != "custom"
            && !QFileInfo::exists(stored.root + "/storage-profile.json");
        const bool available =
            actual ? (stored.id.isEmpty() || stored.id == actual->id) : uninitialized_builtin;
        auto info = actual && available ? *actual : stored;
        if (info.root == current_.root())
            info = current_.info();
        rows.push_back(
            QVariantMap{
                {"root", info.root},
                {"id", info.id},
                {"name", info.name()},
                {"kind", info.kind},
                {"available", available},
                {"current", info.root == current_.root()}
            }
        );
    }
    return rows;
}

void StorageProfileController::refresh() {
    if (busy())
        return;
    QString registry_error;
    recent_ = registry_.recent(&registry_error);
    if (!registry_error.isEmpty())
        error_ = registry_error;
    bool present = false;
    for (auto& info : recent_) {
        if (info.root == current_.root()) {
            info = current_.info();
            present = true;
        }
    }
    if (!present)
        recent_.prepend(current_.info());
    emit changed();
}

void StorageProfileController::renameCurrent(const QString& name) {
    if (busy())
        return;
    error_.clear();
    if (!current_.rename(name, &error_)) {
        emit changed();
        return;
    }
    QString registry_error;
    registry_.remember(current_.info(), context_, &registry_error);
    refresh();
    if (!registry_error.isEmpty())
        error_ = registry_error;
    emit changed();
}

void StorageProfileController::rememberCurrent() {
    refresh();
    registry_.remember(current_.info(), context_, &error_);
    emit changed();
}

void StorageProfileController::createLibrary(const QString& name, const QUrl& parent) {
    if (busy())
        return;
    error_.clear();
    auto created = registry_.create(name, parent.toLocalFile(), &error_);
    if (!created) {
        emit changed();
        return;
    }
    // Keep the new empty library discoverable even if saving the old draft or
    // starting the new process fails. This does not change either launch lane.
    if (!registry_.remember(*created, {}, &error_)) {
        emit changed();
        return;
    }
    refresh();
    begin(*created);
}

void StorageProfileController::openLibrary(const QUrl& directory) {
    switchLibrary(directory.toLocalFile(), {});
}

void StorageProfileController::switchLibrary(const QString& root, const QString& expected_id) {
    if (busy())
        return;
    error_.clear();
    if (StorageProfile::canonicalRoot(root) == current_.root()) {
        refresh();
        return;
    }
    auto target = registry_.prepare(root, expected_id, &error_);
    if (!target) {
        emit changed();
        return;
    }
    begin(*target);
}

void StorageProfileController::begin(const StorageProfileInfo& target) {
    error_.clear();
    if (hooks_.blocker) {
        error_ = hooks_.blocker();
        if (!error_.isEmpty()) {
            emit changed();
            return;
        }
    }
    target_ = target;
    state_ = State::WaitingJobs;
    hooks_.suspend_background(true);
    emit changed();
    poll_.start();
    advance();
}

void StorageProfileController::advance() {
    if (state_ != State::WaitingJobs)
        return;
    if (hooks_.blocker) {
        const auto blocked = hooks_.blocker();
        if (!blocked.isEmpty()) {
            fail(blocked);
            return;
        }
    }
    if (!hooks_.jobs_quiet())
        return;
    poll_.stop();
    state_ = State::Saving;
    emit changed();
    if (hooks_.prepare_editor())
        editorReady();
}

void StorageProfileController::editorReady() {
    if (state_ != State::Saving)
        return;
    state_ = State::Starting;
    emit changed();
    auto arguments = registry_.argumentsFor(target_);
    arguments.append({"--profile-launch-context", context_});
    if (hooks_.restart) {
        hooks_.restart(target_, arguments);
    } else {
        QString error;
        if (!restart_.start(target_, arguments, QCoreApplication::applicationFilePath(), &error))
            restartFailed(error);
    }
}

void StorageProfileController::editorSaveFailed(const QString& error) {
    if (state_ != State::Saving)
        return;
    // The normal window-close recovery sees the same editor signal. Keep this
    // request visibly owned for that whole delivery, then return to editing.
    QTimer::singleShot(0, this, [this, error] {
        if (state_ == State::Saving)
            fail(
                tr("Could not save the current edits. The library was not switched. %1").arg(error)
            );
    });
}

void StorageProfileController::restartActivated() {
    if (state_ != State::Starting)
        return;
    // The activated target records its own launch choice and presents any
    // registry failure. A failed target never changes either launch lane.
    state_ = State::Leaving;
    emit changed();
    emit switched();
}

void StorageProfileController::restartFailed(const QString& error) {
    if (state_ == State::Starting) {
        // A synchronous launch failure can occur inside editor.closeReady.
        // Retain ownership for all normal close listeners in that delivery.
        QTimer::singleShot(0, this, [this, error] {
            if (state_ == State::Starting)
                fail(error);
        });
    }
}

void StorageProfileController::fail(const QString& error) {
    poll_.stop();
    state_ = State::Idle;
    error_ = error;
    hooks_.suspend_background(false);
    emit changed();
}

void StorageProfileController::cancelSwitch() {
    if (canCancel())
        fail({});
}

void StorageProfileController::revealCurrent() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(current_.root()));
}

void StorageProfileController::copyCurrentLocation() {
    QGuiApplication::clipboard()->setText(current_.root());
}

bool StorageProfileController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == QCoreApplication::instance() && event->type() == QEvent::LanguageChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}
