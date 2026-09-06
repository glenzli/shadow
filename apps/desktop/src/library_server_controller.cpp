#include "library_server_controller.hpp"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

constexpr auto secret_service = "dev.shadow.photo.library-server";
constexpr auto secret_account = "shared-access-token";
constexpr auto display_name_key = "library-server/display_name";
constexpr auto port_key = "library-server/port";
constexpr auto serves_originals_key = "library-server/serves_originals";
constexpr auto auto_start_key = "library-server/auto_start";
constexpr auto shared_folders_key = "library-server/shared_folders";
constexpr int minimum_port = 1'024;
constexpr int maximum_port = 65'535;
constexpr int progress_poll_interval_ms = 250;

[[nodiscard]] std::unique_ptr<QSettings> makeSettings(const QString& isolated_settings_file) {
    if (isolated_settings_file.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat);
}

[[nodiscard]] QString defaultDisplayName() {
    const QString host_name = QHostInfo::localHostName().trimmed();
    return host_name.isEmpty() ? QObject::tr("This Mac · Shadow Library")
                               : QObject::tr("%1 · Shadow Library").arg(host_name);
}

[[nodiscard]] QString newAccessToken() {
    QString value = QUuid::createUuid().toString(QUuid::WithoutBraces);
    value += QUuid::createUuid().toString(QUuid::WithoutBraces);
    value.remove(QChar('-'));
    return value;
}

[[nodiscard]] QString preferredLanAddress(const QString& listener_address) {
    const qsizetype separator = listener_address.lastIndexOf(QChar(':'));
    const QString port = separator < 0 ? QString{} : listener_address.sliced(separator + 1);
    if (separator > 0) {
        const QString host = listener_address.first(separator);
        if (host != QStringLiteral("0.0.0.0") && host != QStringLiteral("::")) {
            return listener_address;
        }
    }
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
            return port.isEmpty() ? address.toString() : address.toString() + QChar(':') + port;
        }
    }
    return listener_address;
}

[[nodiscard]] LibraryServerTaskResult runTask(
    const LibraryServerControllerOperations& operations,
    const LibraryServerTaskKind kind,
    const BackendLibraryServerConfig& config
) {
    LibraryServerTaskResult result{.kind = kind};
    try {
        switch (kind) {
        case LibraryServerTaskKind::Refresh:
            result.snapshot = operations.snapshot();
            break;
        case LibraryServerTaskKind::Start:
            result.snapshot = operations.start(config);
            break;
        case LibraryServerTaskKind::Stop:
            result.snapshot = operations.stop();
            break;
        case LibraryServerTaskKind::Restart:
            result.snapshot = operations.stop();
            result.snapshot = operations.start(config);
            break;
        case LibraryServerTaskKind::ResetCache:
            result.snapshot = operations.reset_cache();
            break;
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        if ((kind == LibraryServerTaskKind::Stop || kind == LibraryServerTaskKind::Restart)
            && !result.snapshot.has_value() && operations.snapshot) {
            try {
                // A stop can report a listener/index-worker teardown error after ownership of the
                // running service has already been consumed. Refresh the authoritative backend
                // projection so the UI does not keep advertising a server that no longer exists.
                result.snapshot = operations.snapshot();
            } catch (const std::exception&) {
                // Preserve the lifecycle operation's original diagnostic. A failed recovery read
                // must not replace the error that explains why stop/restart did not complete.
            }
        }
    }
    return result;
}

[[nodiscard]] QString statusForSnapshot(const BackendLibraryServerSnapshot& snapshot) {
    if (!snapshot.running) {
        return QStringLiteral("ready");
    }
    if (snapshot.index_state == QStringLiteral("scanning")) {
        return QStringLiteral("indexing");
    }
    if (snapshot.index_state == QStringLiteral("failed")) {
        return QStringLiteral("indexing-failed");
    }
    return QStringLiteral("running");
}

} // namespace

LibraryServerController::LibraryServerController(
    LibraryServerControllerOperations operations,
    const QString& isolated_settings_file,
    std::unique_ptr<SecretStore> secret_store,
    QObject* const parent
) :
    LibraryServerController(
        std::move(operations),
        makeSettings(isolated_settings_file),
        std::move(secret_store),
        parent
    ) {}

LibraryServerController::LibraryServerController(
    LibraryServerControllerOperations operations,
    std::unique_ptr<QSettings> settings,
    std::unique_ptr<SecretStore> secret_store,
    QObject* const parent
) :
    QObject(parent), operations_(std::move(operations)), settings_(std::move(settings)),
    secret_store_(std::move(secret_store)),
    display_name_(settings_->value(QString::fromLatin1(display_name_key), defaultDisplayName())
                      .toString()
                      .trimmed()),
    shared_folders_(settings_->value(QString::fromLatin1(shared_folders_key)).toStringList()),
    port_(
        std::clamp(
            settings_->value(QString::fromLatin1(port_key), 37'641).toInt(),
            minimum_port,
            maximum_port
        )
    ),
    serves_originals_(settings_->value(QString::fromLatin1(serves_originals_key), true).toBool()),
    auto_start_(settings_->value(QString::fromLatin1(auto_start_key), false).toBool()) {
    initialize();
}

LibraryServerController::~LibraryServerController() {
    watcher_.waitForFinished();
    if (snapshot_.running && operations_.stop) {
        try {
            operations_.stop();
        } catch (const std::exception&) {}
    }
}

void LibraryServerController::initialize() {
    connect(
        &watcher_,
        &QFutureWatcher<LibraryServerTaskResult>::finished,
        this,
        &LibraryServerController::finishTask
    );
    progress_timer_.setInterval(progress_poll_interval_ms);
    progress_timer_.setSingleShot(false);
    connect(&progress_timer_, &QTimer::timeout, this, [this] {
        if (!busy() && running() && indexing()) {
            startTask(LibraryServerTaskKind::Refresh);
        }
    });
    loadTokenState();
    QTimer::singleShot(0, this, [this] {
        if (auto_start_ && !shared_folders_.isEmpty()) {
            startServer();
        }
    });
}

bool LibraryServerController::busy() const noexcept {
    return watcher_.isRunning();
}

bool LibraryServerController::running() const noexcept {
    return snapshot_.running;
}

bool LibraryServerController::secureStorageAvailable() const noexcept {
    return secret_store_ != nullptr && secret_store_->available();
}

bool LibraryServerController::accessTokenStored() const noexcept {
    return access_token_stored_;
}

QString LibraryServerController::displayName() const {
    return display_name_;
}

int LibraryServerController::port() const noexcept {
    return port_;
}

bool LibraryServerController::servesOriginals() const noexcept {
    return serves_originals_;
}

bool LibraryServerController::autoStart() const noexcept {
    return auto_start_;
}

QVariantList LibraryServerController::sharedFolders() const {
    QVariantList folders;
    folders.reserve(shared_folders_.size());
    for (const QString& path : shared_folders_) {
        const QFileInfo info(path);
        folders.push_back(
            QVariantMap{
                {QStringLiteral("path"), path},
                {QStringLiteral("name"), info.fileName().isEmpty() ? path : info.fileName()},
                {QStringLiteral("available"), info.exists() && info.isDir()},
            }
        );
    }
    return folders;
}

QString LibraryServerController::localAddress() const {
    return preferredLanAddress(snapshot_.local_address);
}

QString LibraryServerController::providerMode() const {
    return snapshot_.provider_mode;
}

qulonglong LibraryServerController::photoCount() const noexcept {
    return snapshot_.photo_count;
}

qulonglong LibraryServerController::cacheByteLength() const noexcept {
    return snapshot_.cache_byte_len;
}

QString LibraryServerController::indexState() const {
    return snapshot_.index_state;
}

bool LibraryServerController::indexing() const noexcept {
    return snapshot_.running && snapshot_.index_state == QStringLiteral("scanning");
}

qulonglong LibraryServerController::discoveredFileCount() const noexcept {
    return snapshot_.discovered_file_count;
}

qulonglong LibraryServerController::inspectionCompletedCount() const noexcept {
    return snapshot_.inspection_completed_count;
}

qulonglong LibraryServerController::publishedPreviewCount() const noexcept {
    return snapshot_.published_preview_count;
}

QString LibraryServerController::indexDiagnosticText() const {
    return snapshot_.index_diagnostic;
}

QString LibraryServerController::statusCode() const {
    return status_code_;
}

QString LibraryServerController::diagnosticText() const {
    return diagnostic_text_;
}

void LibraryServerController::setDisplayName(const QString& value) {
    const QString normalized = value.trimmed().left(128);
    if (normalized.isEmpty() || normalized == display_name_ || running() || busy()) {
        return;
    }
    display_name_ = normalized;
    settings_->setValue(QString::fromLatin1(display_name_key), display_name_);
    settings_->sync();
    emit settingsChanged();
}

void LibraryServerController::setPort(const int value) {
    const int normalized = std::clamp(value, minimum_port, maximum_port);
    if (normalized == port_ || running() || busy()) {
        return;
    }
    port_ = normalized;
    settings_->setValue(QString::fromLatin1(port_key), port_);
    settings_->sync();
    emit settingsChanged();
}

void LibraryServerController::setServesOriginals(const bool value) {
    if (value == serves_originals_ || running() || busy()) {
        return;
    }
    serves_originals_ = value;
    settings_->setValue(QString::fromLatin1(serves_originals_key), value);
    settings_->sync();
    emit settingsChanged();
}

void LibraryServerController::setAutoStart(const bool value) {
    if (value == auto_start_) {
        return;
    }
    auto_start_ = value;
    settings_->setValue(QString::fromLatin1(auto_start_key), value);
    settings_->sync();
    emit settingsChanged();
}

bool LibraryServerController::addSharedFolder(const QUrl& folder_url) {
    if (running() || busy() || !folder_url.isLocalFile()) {
        return false;
    }
    const QFileInfo folder(folder_url.toLocalFile());
    const QString canonical = folder.canonicalFilePath();
    if (canonical.isEmpty() || !folder.isDir() || shared_folders_.contains(canonical)) {
        setStatus(QStringLiteral("folder-invalid"));
        return false;
    }
    shared_folders_.push_back(canonical);
    persistFolders();
    setStatus(QStringLiteral("folder-added"));
    emit settingsChanged();
    return true;
}

bool LibraryServerController::removeSharedFolder(const int index) {
    if (running() || busy() || index < 0 || index >= shared_folders_.size()) {
        return false;
    }
    shared_folders_.removeAt(index);
    persistFolders();
    setStatus(QStringLiteral("folder-removed"));
    emit settingsChanged();
    return true;
}

void LibraryServerController::refresh() {
    startTask(LibraryServerTaskKind::Refresh);
}

void LibraryServerController::startServer() {
    if (busy() || running()) {
        return;
    }
    if (shared_folders_.isEmpty()) {
        setStatus(QStringLiteral("folder-required"));
        return;
    }
    if (!ensureAccessToken()) {
        return;
    }
    const SecretStoreResult token = readAccessToken();
    if (!token.succeeded()) {
        setStatus(QStringLiteral("secret-store-failed"), token.diagnostic);
        return;
    }
    startTask(LibraryServerTaskKind::Start, currentConfig(token.value));
}

void LibraryServerController::stopServer() {
    if (!busy() && running()) {
        startTask(LibraryServerTaskKind::Stop);
    }
}

void LibraryServerController::rescanAndRestart() {
    if (busy()) {
        return;
    }
    if (!running()) {
        startServer();
        return;
    }
    const SecretStoreResult token = readAccessToken();
    if (!token.succeeded()) {
        setStatus(QStringLiteral("secret-store-failed"), token.diagnostic);
        return;
    }
    startTask(LibraryServerTaskKind::Restart, currentConfig(token.value));
}

void LibraryServerController::clearCache() {
    if (!busy() && !running()) {
        startTask(LibraryServerTaskKind::ResetCache);
    }
}

bool LibraryServerController::copyAccessToken() {
    if (!ensureAccessToken()) {
        return false;
    }
    const SecretStoreResult token = readAccessToken();
    if (!token.succeeded()) {
        setStatus(QStringLiteral("secret-store-failed"), token.diagnostic);
        return false;
    }
    if (QGuiApplication::clipboard() == nullptr) {
        setStatus(QStringLiteral("clipboard-unavailable"));
        return false;
    }
    QGuiApplication::clipboard()->setText(token.value);
    setStatus(QStringLiteral("token-copied"));
    return true;
}

bool LibraryServerController::regenerateAccessToken() {
    if (running() || busy() || !secureStorageAvailable()) {
        return false;
    }
    const SecretStoreResult result = secret_store_->write(
        QString::fromLatin1(secret_service),
        QString::fromLatin1(secret_account),
        newAccessToken()
    );
    if (!result.succeeded()) {
        setStatus(QStringLiteral("secret-store-failed"), result.diagnostic);
        return false;
    }
    access_token_stored_ = true;
    setStatus(QStringLiteral("token-regenerated"));
    emit stateChanged();
    return true;
}

void LibraryServerController::loadTokenState() {
    if (!secureStorageAvailable()) {
        access_token_stored_ = false;
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return;
    }
    const SecretStoreResult token = readAccessToken();
    access_token_stored_ = token.succeeded() && token.value.size() >= 32;
    if (token.status != SecretStoreStatus::Success && token.status != SecretStoreStatus::NotFound) {
        setStatus(QStringLiteral("secret-store-failed"), token.diagnostic);
    }
}

bool LibraryServerController::ensureAccessToken() {
    if (access_token_stored_) {
        return true;
    }
    return regenerateAccessToken();
}

SecretStoreResult LibraryServerController::readAccessToken() const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("Shadow's local credential file is unavailable."),
        };
    }
    return secret_store_->read(
        QString::fromLatin1(secret_service),
        QString::fromLatin1(secret_account)
    );
}

BackendLibraryServerConfig LibraryServerController::currentConfig(const QString& token) const {
    return {
        .bind_address = QStringLiteral("0.0.0.0:%1").arg(port_),
        .authorization = token,
        .display_name = display_name_,
        .share_roots = shared_folders_,
        .serves_originals = serves_originals_,
    };
}

void LibraryServerController::startTask(
    const LibraryServerTaskKind kind,
    BackendLibraryServerConfig config
) {
    if (watcher_.isRunning()) {
        return;
    }
    switch (kind) {
    case LibraryServerTaskKind::Refresh:
        if (!indexing()) {
            setStatus(QStringLiteral("refreshing"));
        }
        break;
    case LibraryServerTaskKind::Start:
        setStatus(QStringLiteral("starting"));
        break;
    case LibraryServerTaskKind::Stop:
        setStatus(QStringLiteral("stopping"));
        break;
    case LibraryServerTaskKind::Restart:
        setStatus(QStringLiteral("rescanning"));
        break;
    case LibraryServerTaskKind::ResetCache:
        setStatus(QStringLiteral("clearing-cache"));
        break;
    }
    watcher_.setFuture(
        QtConcurrent::run([operations = operations_, kind, config = std::move(config)] {
            return runTask(operations, kind, config);
        })
    );
    emit busyChanged();
}

void LibraryServerController::finishTask() {
    const LibraryServerTaskResult result = watcher_.result();
    emit busyChanged();
    if (result.snapshot.has_value()) {
        applySnapshot(*result.snapshot);
    }
    if (!result.error.isEmpty()) {
        setStatus(QStringLiteral("operation-failed"), result.error);
        return;
    }
    switch (result.kind) {
    case LibraryServerTaskKind::Refresh:
        setStatus(statusForSnapshot(snapshot_));
        break;
    case LibraryServerTaskKind::Start:
    case LibraryServerTaskKind::Restart:
        setStatus(statusForSnapshot(snapshot_));
        break;
    case LibraryServerTaskKind::Stop:
        setStatus(QStringLiteral("stopped"));
        break;
    case LibraryServerTaskKind::ResetCache:
        setStatus(QStringLiteral("cache-cleared"));
        break;
    }
}

void LibraryServerController::applySnapshot(const BackendLibraryServerSnapshot& snapshot) {
    snapshot_ = snapshot;
    if (indexing()) {
        progress_timer_.start();
    } else {
        progress_timer_.stop();
    }
    emit stateChanged();
}

void LibraryServerController::setStatus(const QString& code, const QString& diagnostic) {
    if (status_code_ == code && diagnostic_text_ == diagnostic) {
        return;
    }
    status_code_ = code;
    diagnostic_text_ = diagnostic;
    emit stateChanged();
}

void LibraryServerController::persistFolders() {
    settings_->setValue(QString::fromLatin1(shared_folders_key), shared_folders_);
    settings_->sync();
}
