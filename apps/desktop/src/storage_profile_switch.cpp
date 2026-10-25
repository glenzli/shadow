#include "storage_profile_switch.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>

namespace {
constexpr qint64 maximum_message = 64 * 1024;
constexpr int startup_timeout = 60'000;
} // namespace

StorageProfileRestart::StorageProfileRestart(QObject* parent) : QObject(parent) {
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        fail(tr("The target library did not finish starting. The current library remains open."));
    });
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        auto* candidate = server_.nextPendingConnection();
        if (socket_) {
            candidate->disconnectFromServer();
            candidate->deleteLater();
            return;
        }
        socket_ = candidate;
        connect(socket_, &QLocalSocket::readyRead, this, &StorageProfileRestart::receive);
        connect(socket_, &QLocalSocket::disconnected, this, [this] {
            if (timeout_.isActive())
                fail(tr(
                    "The target library stopped before switching. The current library remains open."
                ));
        });
        receive();
    });
}

qint64 StorageProfileRestart::childPid() const {
    return child_pid_;
}

bool StorageProfileRestart::start(
    const StorageProfileInfo& target,
    const QStringList& arguments,
    const QString& executable,
    QString* const error
) {
    target_ = target;
    incoming_.clear();
    activating_ = false;
    child_pid_ = 0;
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    const QString server_name =
        "shadow-profile-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!server_.listen(server_name)) {
        qWarning().noquote() << "Profile restart socket:" << server_.errorString();
        *error = tr("Could not prepare the library restart.");
        return false;
    }
    QProcess child;
    child.setProgram(executable);
    auto child_arguments = arguments;
    child_arguments.append({"--profile-switch-server", server_name});
    child.setArguments(child_arguments);
    auto environment = QProcessEnvironment::systemEnvironment();
    // The old process has bound these values to its storage owner. Never carry
    // them, photo-open requests or smoke/import commands into the new owner.
    for (const auto& key : environment.keys()) {
        if (key.startsWith("SHADOW_DESKTOP_") || key == "SHADOW_PAINT_BRUSH_SETTINGS_FILE")
            environment.remove(key);
    }
    child.setProcessEnvironment(environment);
    if (!child.startDetached(&child_pid_)) {
        qWarning().noquote() << "Profile restart process:" << child.errorString();
        server_.close();
        *error = tr("Could not start Shadow for the target library.");
        return false;
    }
    timeout_.start(startup_timeout);
    return true;
}

void StorageProfileRestart::receive() {
    if (!socket_ || !timeout_.isActive())
        return;
    incoming_ += socket_->readAll();
    if (incoming_.size() > maximum_message) {
        fail(tr("The target library returned an invalid startup response."));
        return;
    }
    while (incoming_.contains('\n')) {
        const auto end = incoming_.indexOf('\n');
        const auto message = QJsonDocument::fromJson(incoming_.left(end)).object();
        incoming_.remove(0, end + 1);
        const QString state = message.value("state").toString();
        if (state == "failed") {
            fail(
                tr("Could not open the target library: %1").arg(message.value("error").toString())
            );
            return;
        }
        if (message.value("root").toString() != target_.root
            || message.value("id").toString() != target_.id) {
            fail(tr("The target library returned an invalid startup response."));
            return;
        }
        if (state == "ready" && !activating_) {
            activating_ = true;
            socket_->write("activate\n");
            socket_->flush();
        } else if (state == "active" && activating_) {
            timeout_.stop();
            server_.close();
            emit activated();
            return;
        } else {
            fail(tr("The target library returned an invalid startup response."));
            return;
        }
    }
}

void StorageProfileRestart::fail(const QString& error) {
    timeout_.stop();
    server_.close();
    if (socket_) {
        auto* const previous = socket_;
        socket_ = nullptr;
        previous->disconnect(this);
        previous->abort();
        previous->deleteLater();
    }
    emit failed(error);
}

StorageProfileSwitchEndpoint::StorageProfileSwitchEndpoint(
    const QStringList& arguments,
    QObject* parent
) : QObject(parent) {
    QString server;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        if (arg != "--profile-switch-server" && !arg.startsWith("--profile-switch-server="))
            continue;
        QString value;
        if (arg.contains('='))
            value = arg.sliced(arg.indexOf('=') + 1);
        else if (i + 1 < arguments.size())
            value = arguments[++i];
        requested_ = true;
        if (!server.isEmpty() || !value.startsWith("shadow-profile-")
            || QUuid(value.sliced(15)).isNull()) {
            error_ = tr("The library restart request is invalid.");
            return;
        }
        server = value;
    }
    if (!requested_)
        return;
    socket_.connectToServer(server);
    if (!socket_.waitForConnected(3000)) {
        error_ = tr("The previous Shadow session is no longer available.");
        return;
    }
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, &StorageProfileSwitchEndpoint::abandoned);
    timeout_.start(startup_timeout);
    connect(&socket_, &QLocalSocket::disconnected, this, [this] {
        if (!activated_)
            emit abandoned();
    });
    connect(&socket_, &QLocalSocket::readyRead, this, [this] {
        incoming_ += socket_.readAll();
        if (incoming_.size() > maximum_message) {
            emit abandoned();
            return;
        }
        if (!incoming_.contains('\n'))
            return;
        if (incoming_ != "activate\n" || !ready_ || activating_) {
            emit abandoned();
            return;
        }
        incoming_.clear();
        activating_ = true;
        emit activate();
    });
}

bool StorageProfileSwitchEndpoint::requested() const {
    return requested_;
}
bool StorageProfileSwitchEndpoint::valid() const {
    return error_.isEmpty();
}
QString StorageProfileSwitchEndpoint::error() const {
    return error_;
}

void StorageProfileSwitchEndpoint::send(const QString& state, const QString& error) {
    if (!requested_ || !valid())
        return;
    const QJsonObject
        message{{"state", state}, {"root", profile_.root}, {"id", profile_.id}, {"error", error}};
    socket_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    socket_.flush();
}

void StorageProfileSwitchEndpoint::ready(const StorageProfileInfo& profile) {
    profile_ = profile;
    ready_ = true;
    send("ready");
}

void StorageProfileSwitchEndpoint::confirmActivated() {
    if (!activating_)
        return;
    activated_ = true;
    timeout_.stop();
    send("active");
}

void StorageProfileSwitchEndpoint::reportFailure(const QString& error) {
    send("failed", error);
    socket_.waitForBytesWritten(100);
}
