#include "edit_tool_stdio.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <cstdlib>
#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif
namespace {
void require(bool value, const char* message) {
    if (!value) {
        qCritical() << message;
        std::exit(1);
    }
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
#if defined(Q_OS_UNIX)
    int input[2], output[2];
    require(::pipe(input) == 0 && ::pipe(output) == 0, "create pipes");
    ::fcntl(output[0], F_SETFL, O_NONBLOCK);
    {
        EditToolStdio transport(input[0], output[1]);
        QList<QByteArray> lines;
        bool closed = false, drained = false;
        QObject::connect(&transport, &EditToolStdio::lineReceived, [&](auto line) {
            lines.append(line);
        });
        QObject::connect(&transport, &EditToolStdio::inputClosed, [&] { closed = true; });
        QObject::connect(&transport, &EditToolStdio::drained, [&] { drained = true; });
        require(::write(input[1], "{\"id\":", 6) == 6, "write fragment");
        app.processEvents();
        require(lines.isEmpty(), "partial request is not admitted");
        require(::write(input[1], "1}\n{}\n", 6) == 6, "finish fragment");
        app.processEvents();
        require(lines.size() == 2 && lines[0] == "{\"id\":1}", "line framing preserves requests");
        // Stop draining the peer. Writes must return promptly, remain bounded,
        // and close admission rather than freezing the GUI on pipe capacity.
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 500 && !closed; ++i)
            transport.send({{"payload", QString(4096, QLatin1Char('x'))}});
        require(elapsed.elapsed() < 1000, "backpressure cannot block the Qt thread");
        require(closed, "bounded backpressure disconnects input");
        transport.finish();
        app.processEvents();
        require(drained, "closed transport can finish");
    }
    ::close(input[0]);
    ::close(input[1]);
    ::close(output[0]);
    ::close(output[1]);
    require(::pipe(input) == 0 && ::pipe(output) == 0, "create second pipes");
    {
        EditToolStdio transport(input[0], output[1]);
        bool closed = false;
        QObject::connect(&transport, &EditToolStdio::inputClosed, [&] { closed = true; });
        ::close(output[0]);
        transport.send({{"result", "peer gone"}});
        require(closed, "broken pipe handled without SIGPIPE termination");
    }
    ::close(input[0]);
    ::close(input[1]);
    ::close(output[1]);
    require(::pipe(input) == 0 && ::pipe(output) == 0, "create drain pipes");
    {
        EditToolStdio transport(input[0], output[1]);
        bool drained = false;
        QObject::connect(&transport, &EditToolStdio::drained, [&] { drained = true; });
        transport.send({{"payload", QString(128 * 1024, QLatin1Char('x'))}});
        transport.finish();
        require(!drained, "pending output must drain before clean exit");
        ::close(output[0]);
        for (int i = 0; i < 20 && !drained; ++i)
            app.processEvents();
        require(drained, "peer closing during drain must complete shutdown");
    }
    ::close(input[0]);
    ::close(input[1]);
    ::close(output[1]);
#endif
    return 0;
}
