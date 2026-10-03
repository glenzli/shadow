#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <memory>
class QSocketNotifier;

// Explicit process pipes only; no listening socket, global server, or discovery
// of another editor. Read/write queues are bounded and never block Qt input.
class EditToolStdio final : public QObject {
    Q_OBJECT
  public:
    explicit EditToolStdio(int input = 0, int output = 1, QObject* parent = nullptr);
    ~EditToolStdio() override;
    void send(const QJsonObject& response);
    void finish();
  signals:
    void lineReceived(const QByteArray& line);
    void inputClosed();
    void drained();

  private:
    void readAvailable();
    void processLines();
    void writeAvailable();
    void stopInput();
    int input_, output_, input_flags_ = -1, output_flags_ = -1;
    struct SignalState;
    std::unique_ptr<SignalState> signal_state_;
    std::unique_ptr<QSocketNotifier> reader_, writer_;
    QByteArray incoming_, outgoing_;
    bool input_closed_ = false, output_closed_ = false, finishing_ = false;
};
