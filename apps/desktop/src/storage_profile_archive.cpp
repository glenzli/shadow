#include "storage_profile_archive.hpp"
#include "backend/native_path_input.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QLockFile>
#include <QTimer>
#include <QtConcurrentRun>

#include <exception>
#include <utility>

namespace {
struct ArchiveResult final {
    QString root;
    QString diagnostic;
};
using Control = rust::Box<shadow::desktop::ProfileArchiveControl>;
} // namespace

struct StorageProfileArchive::Impl final {
    QFutureWatcher<ArchiveResult> watcher;
    QTimer poll;
    std::shared_ptr<Control> control;
};

StorageProfileArchive::StorageProfileArchive(QObject* parent) :
    QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->poll.setInterval(150);
    connect(&impl_->poll, &QTimer::timeout, this, [this] {
        if (!impl_->control)
            return;
        const auto value = shadow::desktop::profile_archive_progress(**impl_->control);
        emit progress(value.completed_files, value.total_files);
    });
    connect(&impl_->watcher, &QFutureWatcher<ArchiveResult>::finished, this, [this] {
        impl_->poll.stop();
        const auto result = impl_->watcher.result();
        impl_->control.reset();
        if (result.diagnostic.isEmpty())
            emit completed(result.root);
        else
            emit failed(result.diagnostic);
    });
}

StorageProfileArchive::~StorageProfileArchive() {
    cancel();
    impl_->watcher.waitForFinished();
}

void StorageProfileArchive::cancel() {
    if (impl_->control)
        shadow::desktop::cancel_profile_archive(**impl_->control);
}

void StorageProfileArchive::start(const StorageProfileArchiveRequest& request) {
    if (impl_->control)
        return;
    impl_->control = std::make_shared<Control>(shadow::desktop::new_profile_archive_control());
    const auto control = impl_->control;
    impl_->watcher.setFuture(QtConcurrent::run([request, control] {
        ArchiveResult result;
        try {
            std::unique_ptr<QLockFile> server_lock, enrollment_lock;
            if (request.action == 0 || request.action == 2) {
                server_lock = std::make_unique<QLockFile>(
                    QDir(request.source).filePath("server-manager.lock")
                );
                enrollment_lock = std::make_unique<QLockFile>(
                    QDir(request.source).filePath("storage-profile-initialization.lock")
                );
                server_lock->setStaleLockTime(0);
                enrollment_lock->setStaleLockTime(0);
                if (!server_lock->tryLock() || !enrollment_lock->tryLock()) {
                    result.diagnostic = QCoreApplication::translate(
                        "StorageProfileController",
                        "Close the library server and finish other library operations before "
                        "backing up or copying this library."
                    );
                    return result;
                }
            }
            const auto receipt = shadow::desktop::run_profile_archive(
                request.action,
                native_path_input::path(request.source),
                native_path_input::path(request.parent),
                request.name.toStdString(),
                request.expected_id.toStdString(),
                **control
            );
            result.root = native_path_input::qstring(receipt.root);
        } catch (const std::exception& exception) {
            result.diagnostic = QString::fromUtf8(exception.what());
        }
        return result;
    }));
    impl_->poll.start();
}
