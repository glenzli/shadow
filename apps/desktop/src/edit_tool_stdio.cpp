#include "edit_tool_stdio.hpp"
#include "edit_tool_protocol.hpp"
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <stdexcept>
#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct EditToolStdio::SignalState {
#if defined(Q_OS_UNIX)
    struct sigaction previous{};
    bool installed = false;
#endif
};
EditToolStdio::EditToolStdio(int input, int output, QObject* parent) :
    QObject(parent), input_(input), output_(output),
    signal_state_(std::make_unique<SignalState>()) {
#if defined(Q_OS_UNIX)
    struct stat input_stat{}, output_stat{};
    if (::fstat(input_, &input_stat) != 0 || ::fstat(output_, &output_stat) != 0
        || !(S_ISFIFO(input_stat.st_mode) || S_ISSOCK(input_stat.st_mode))
        || !(S_ISFIFO(output_stat.st_mode) || S_ISSOCK(output_stat.st_mode)))
        throw std::runtime_error("--agent-stdio requires connected stdin and stdout pipes");
    input_flags_ = ::fcntl(input_, F_GETFL);
    output_flags_ = ::fcntl(output_, F_GETFL);
    if (input_flags_ < 0 || output_flags_ < 0)
        throw std::runtime_error("cannot inspect agent process pipes");
    if (::fcntl(input_, F_SETFL, input_flags_ | O_NONBLOCK) < 0)
        throw std::runtime_error("cannot configure agent input pipe");
    if (::fcntl(output_, F_SETFL, output_flags_ | O_NONBLOCK) < 0) {
        ::fcntl(input_, F_SETFL, input_flags_);
        throw std::runtime_error("cannot configure agent output pipe");
    }
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (::sigaction(SIGPIPE, &ignore, &signal_state_->previous) != 0) {
        ::fcntl(input_, F_SETFL, input_flags_);
        ::fcntl(output_, F_SETFL, output_flags_);
        throw std::runtime_error("cannot configure agent broken-pipe handling");
    }
    signal_state_->installed = true;
    reader_ = std::make_unique<QSocketNotifier>(input_, QSocketNotifier::Read, this);
    writer_ = std::make_unique<QSocketNotifier>(output_, QSocketNotifier::Write, this);
    writer_->setEnabled(false);
    connect(reader_.get(), &QSocketNotifier::activated, this, [this] { readAvailable(); });
    connect(writer_.get(), &QSocketNotifier::activated, this, [this] { writeAvailable(); });
#else
    throw std::runtime_error("--agent-stdio is supported only on POSIX platforms");
#endif
}
EditToolStdio::~EditToolStdio() {
    reader_.reset();
    writer_.reset();
#if defined(Q_OS_UNIX)
    if (input_flags_ >= 0)
        ::fcntl(input_, F_SETFL, input_flags_);
    if (output_flags_ >= 0)
        ::fcntl(output_, F_SETFL, output_flags_);
    if (signal_state_->installed)
        ::sigaction(SIGPIPE, &signal_state_->previous, nullptr);
#endif
}
void EditToolStdio::stopInput() {
    if (input_closed_)
        return;
    input_closed_ = true;
    if (reader_)
        reader_->setEnabled(false);
    incoming_.clear();
    emit inputClosed();
}
void EditToolStdio::readAvailable() {
#if defined(Q_OS_UNIX)
    // A bounded turn lets queued user input and cancellation run under a flood.
    for (int turn = 0; turn < 16 && !input_closed_; ++turn) {
        char buffer[4096];
        const auto count = ::read(input_, buffer, sizeof(buffer));
        if (count > 0) {
            incoming_.append(buffer, static_cast<qsizetype>(count));
            processLines();
        } else if (count == 0) {
            if (!incoming_.isEmpty())
                send(EditToolProtocol::failure({}, "invalid_request", "unterminated JSON line"));
            stopInput();
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else if (errno != EINTR) {
            stopInput();
            return;
        }
    }
#endif
}
void EditToolStdio::processLines() {
    while (!input_closed_) {
        const auto newline = incoming_.indexOf('\n');
        if ((newline < 0 && incoming_.size() > EditToolProtocol::maximumLineBytes)
            || newline > EditToolProtocol::maximumLineBytes) {
            send(EditToolProtocol::failure({}, "invalid_request", "request exceeds 64 KiB"));
            stopInput();
            return;
        }
        if (newline < 0)
            return;
        const auto line = incoming_.first(newline);
        incoming_.remove(0, newline + 1);
        emit lineReceived(line);
    }
}
void EditToolStdio::send(const QJsonObject& response) {
    if (output_closed_)
        return;
    auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (outgoing_.size() + bytes.size() > 256 * 1024) {
        output_closed_ = true;
        outgoing_.clear();
        if (writer_)
            writer_->setEnabled(false);
        stopInput();
        return;
    }
    outgoing_.append(bytes);
    writeAvailable();
}
void EditToolStdio::writeAvailable() {
#if defined(Q_OS_UNIX)
    if (output_closed_)
        return;
    for (int turn = 0; turn < 16 && !outgoing_.isEmpty(); ++turn) {
        const auto count =
            ::write(output_, outgoing_.constData(), static_cast<size_t>(outgoing_.size()));
        if (count > 0)
            outgoing_.remove(0, static_cast<qsizetype>(count));
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else {
            output_closed_ = true;
            outgoing_.clear();
            writer_->setEnabled(false);
            stopInput();
            if (finishing_) {
                finishing_ = false;
                QTimer::singleShot(0, this, [this] { emit drained(); });
            }
            return;
        }
    }
    writer_->setEnabled(!outgoing_.isEmpty());
    if (finishing_ && outgoing_.isEmpty()) {
        finishing_ = false;
        QTimer::singleShot(0, this, [this] { emit drained(); });
    }
#endif
}
void EditToolStdio::finish() {
    finishing_ = true;
    // Do not emit inputClosed recursively during an explicit clean shutdown.
    input_closed_ = true;
    if (reader_)
        reader_->setEnabled(false);
    incoming_.clear();
    if (output_closed_) {
        finishing_ = false;
        QTimer::singleShot(0, this, [this] { emit drained(); });
    } else
        writeAvailable();
}
