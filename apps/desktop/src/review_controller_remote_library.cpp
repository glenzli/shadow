#include "review_controller.hpp"

// QML-facing remote Library routing. Network, mirror, credential, and
// materialization lifecycles remain owned by ReviewRemoteLibraryCoordinator.

bool ReviewController::remoteLibraryBusy() const noexcept {
    return remote_library_coordinator_.busy();
}

QVariantList ReviewController::remoteLibraries() const {
    return remote_library_coordinator_.connections();
}

bool ReviewController::remoteLibrarySyncing() const noexcept {
    return remote_library_coordinator_.syncing();
}

bool ReviewController::remoteLibraryMaterializing() const noexcept {
    return remote_library_coordinator_.materializing();
}

bool ReviewController::remoteLibrarySecureStorageAvailable() const noexcept {
    return remote_library_coordinator_.secureStorageAvailable();
}

bool ReviewController::remoteLibraryTokenStored() const noexcept {
    return remote_library_coordinator_.tokenStored();
}

QString ReviewController::remoteLibraryServerAddress() const {
    return remote_library_coordinator_.serverAddress();
}

bool ReviewController::remoteLibraryConnected() const noexcept {
    return remote_library_coordinator_.tokenStored()
           && !remote_library_coordinator_.serverAddress().isEmpty();
}

QString ReviewController::remoteLibraryServerName() const {
    return remote_library_coordinator_.serverName();
}

int ReviewController::remoteLibraryPhotoCount() const noexcept {
    return remote_library_coordinator_.remotePhotoCount();
}

QString ReviewController::remoteLibraryStatusCode() const {
    return remote_library_coordinator_.statusCode();
}

QString ReviewController::remoteLibraryDiagnosticText() const {
    return remote_library_coordinator_.diagnosticText();
}

QString ReviewController::remoteLibraryMaterializingPhotoId() const {
    return remote_library_coordinator_.materializingPhotoId();
}

bool ReviewController::saveRemoteLibraryConnection(
    const QString& server_address,
    const QString& token
) {
    return remote_library_coordinator_.saveConnection(server_address, token);
}

QString ReviewController::saveRemoteLibraryConnection(
    const QString& connection_id,
    const QString& server_address,
    const QString& token
) {
    return remote_library_coordinator_.saveConnection(connection_id, server_address, token);
}

bool ReviewController::removeRemoteLibraryConnection() {
    return remote_library_coordinator_.removeConnection();
}

bool ReviewController::removeRemoteLibraryConnection(const QString& connection_id) {
    return remote_library_coordinator_.removeConnection(connection_id);
}

void ReviewController::syncRemoteLibrary() {
    remote_library_coordinator_.syncNow();
}

void ReviewController::syncRemoteLibrary(const QString& connection_id) {
    remote_library_coordinator_.syncNow(connection_id);
}

void ReviewController::syncAllRemoteLibraries() {
    remote_library_coordinator_.syncAll();
}

void ReviewController::materializeRemotePhoto(const QString& photo_id) {
    remote_library_coordinator_.materializeForEdit(photo_id);
}

void ReviewController::refreshRemoteLibraryPresentation() {
    if (remoteLibraryPresentationEligible()) {
        remote_library_coordinator_.reapplyRemoteItems();
    } else {
        (void)model_.replaceRemoteItems({});
    }
}

bool ReviewController::remoteLibraryPresentationEligible() const {
    return album_coordinator_.albumId().isEmpty() && filtered_model_.captureMonth().isEmpty()
           && filtered_model_.chineseLunarMonth() == 0 && filtered_model_.chineseLunarDay() == 0
           && filtered_model_.chineseLunarMonthType() == QStringLiteral("all")
           && filtered_model_.cameraKey().isEmpty() && filtered_model_.lensKey().isEmpty()
           && filtered_model_.countryKey().isEmpty() && filtered_model_.localityKey().isEmpty()
           && !filtered_model_.travelFilterEnabled() && filtered_model_.keywordIdsAll().isEmpty()
           && filtered_model_.excludedKeywordIdsAny().isEmpty();
}

int ReviewController::visibleRemotePhotoCount() const {
    if (!remoteLibraryPresentationEligible()) {
        return 0;
    }
    int count = 0;
    for (int row = 0; row < filtered_model_.rowCount(); ++row) {
        const QModelIndex index = filtered_model_.index(row, 0);
        if (filtered_model_.data(index, ReviewModel::IsRemoteRole).toBool()) {
            ++count;
        }
    }
    return count;
}
