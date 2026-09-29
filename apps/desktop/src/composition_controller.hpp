#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>
class CompositionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QUrl preview READ preview NOTIFY changed)
    Q_PROPERTY(QString dimensions READ dimensions NOTIFY changed)
    Q_PROPERTY(QString savedPath READ savedPath NOTIFY changed)
    Q_PROPERTY(int inputCount READ inputCount NOTIFY changed)
  public:
    explicit CompositionController(QObject* parent = nullptr);
    ~CompositionController() override;
    bool busy() const;
    bool ready() const;
    QString status() const;
    QString error() const;
    QUrl preview() const;
    QString dimensions() const;
    QString savedPath() const;
    int inputCount() const;
    Q_INVOKABLE void prepare(const QVariantList& targets, const QString& mode);
    Q_INVOKABLE void start(int max_edge, bool align, bool deghost, bool compensate);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void save(const QUrl& destination);
    Q_INVOKABLE QUrl suggestedDestination() const;
  signals:
    void changed();
    void saved(const QUrl& file);

  private:
    struct Publication {
        QString path;
        QString error;
    };
    static Publication
    publish(QString source, QString target, std::shared_ptr<std::atomic_bool> cancelled);
    QString errorMessage(const QString& code) const;
    void receive();
    void finished(int exit_code, QProcess::ExitStatus status);
    QProcess process_;
    QFutureWatcher<Publication> publication_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::unique_ptr<QTemporaryDir> staging_;
    QVariantList inputs_;
    QString mode_, phase_, error_, saved_path_;
    QByteArray pending_;
    int index_ = 0, width_ = 0, height_ = 0;
    bool running_ = false, ready_ = false;
};
