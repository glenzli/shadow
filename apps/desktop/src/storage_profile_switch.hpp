#pragma once

#include "storage_profile.hpp"

#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

// Two-phase restart: the old process retains its profile lock until the new
// process has loaded its services/QML and acknowledged activation.
class StorageProfileRestart final : public QObject {
    Q_OBJECT
  public:
    explicit StorageProfileRestart(QObject* parent = nullptr);
    bool start(
        const StorageProfileInfo& target,
        const QStringList& arguments,
        const QString& executable,
        QString* error
    );
    [[nodiscard]] qint64 childPid() const;
  signals:
    void activated();
    void failed(const QString& error);

  private:
    void receive();
    void fail(const QString& error);
    QLocalServer server_;
    QLocalSocket* socket_ = nullptr;
    QTimer timeout_;
    StorageProfileInfo target_;
    QByteArray incoming_;
    qint64 child_pid_ = 0;
    bool activating_ = false;
};

// Constructed before opening storage so every child startup failure can be
// returned to the still-running old process. Unrequested normal launches are
// inert. Only the restart owner is allowed to activate the hidden window.
class StorageProfileSwitchEndpoint final : public QObject {
    Q_OBJECT
  public:
    explicit StorageProfileSwitchEndpoint(const QStringList& arguments, QObject* parent = nullptr);
    [[nodiscard]] bool requested() const;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] QString error() const;
    void ready(const StorageProfileInfo& profile);
    void confirmActivated();
    void reportFailure(const QString& error);
  signals:
    void activate();
    void abandoned();

  private:
    void send(const QString& state, const QString& error = {});
    QLocalSocket socket_;
    QTimer timeout_;
    StorageProfileInfo profile_;
    QByteArray incoming_;
    QString error_;
    bool requested_ = false;
    bool ready_ = false;
    bool activating_ = false;
    bool activated_ = false;
};
