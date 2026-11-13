#include "headless.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>
#include <string>
#include <vector>

#include "frame_stream.h"
#include "interconnect/message_codec.hpp"
#include "interconnect/message_envelope.hpp"

namespace {

using vr::core::ErrorCode;
using vr::interconnect::MessageCodec;
using vr::interconnect::MessageEnvelope;

struct Stats {
    quint64 frames{0};
    quint64 decode_fail{0};
    quint64 bytes{0};
    quint64 bad_header{0};
};

QString Pick(const QString& payload, const char* key)
{
    // payload: "dv=N;dl=0;..." —— 只取 headless 输出需要的几个键
    const QString k = QString::fromLatin1(key) + "=";
    const int at = payload.indexOf(k);
    if (at < 0) return "-";
    const int start = at + k.size();
    int end = payload.indexOf(';', start);
    if (end < 0) end = payload.size();
    return payload.mid(start, end - start);
}

}  // namespace

int RunHeadless(const HeadlessOptions& opt)
{
    QTcpServer server;
    FrameStream stream;
    Stats st;
    QByteArray chunks;   // 本轮 readyRead 收到的原始字节

    std::printf("[headless] listening on %u\n", static_cast<unsigned>(opt.port));
    std::fflush(stdout);

    if (!server.listen(QHostAddress::AnyIPv4, opt.port)) {
        std::printf("[headless] listen FAILED: %s\n", qPrintable(server.errorString()));
        std::fflush(stdout);
        return 3;
    }

    QTcpSocket* client = nullptr;

    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        while (server.hasPendingConnections()) {
            QTcpSocket* s = server.nextPendingConnection();
            if (client != nullptr) {
                // 只服务一个上报源；后到的直接断开
                s->disconnectFromHost();
                continue;
            }
            client = s;
            stream.Reset();
            std::printf("[headless] client connected\n");
            std::fflush(stdout);

            QObject::connect(client, &QTcpSocket::readyRead, [&]() {
                const QByteArray chunk = client->readAll();
                st.bytes += static_cast<quint64>(chunk.size());
                std::vector<std::string> frames;
                stream.Feed(chunk.constData(), static_cast<size_t>(chunk.size()), &frames);
                if (stream.bad_header()) {
                    ++st.bad_header;
                    std::printf("[headless] BAD HEADER -> buffer reset\n");
                    std::fflush(stdout);
                    stream.Reset();
                }
                for (const std::string& f : frames) {
                    const QString text = QString::fromStdString(f);
                    MessageEnvelope env;
                    if (MessageCodec::Decode(text.toStdString(), &env) != ErrorCode::kOk) {
                        ++st.decode_fail;
                        continue;
                    }
                    ++st.frames;
                    if (!opt.quiet) {
                        const QString p = QString::fromStdString(env.payload);
                        std::printf(
                            "[headless] frame#%llu seq=%llu ts=%llu dv=%s dl=%s ev=%s es=%s "
                            "ab=%s dg=%s\n",
                            static_cast<unsigned long long>(st.frames),
                            static_cast<unsigned long long>(env.sequence),
                            static_cast<unsigned long long>(env.timestamp_ms),
                            qPrintable(Pick(p, "dv")), qPrintable(Pick(p, "dl")),
                            qPrintable(Pick(p, "ev")), qPrintable(Pick(p, "es")),
                            qPrintable(Pick(p, "ab")), qPrintable(Pick(p, "dg")));
                        std::fflush(stdout);
                    }
                }
            });

            QObject::connect(client, &QTcpSocket::disconnected, [&]() {
                std::printf("[headless] client disconnected\n");
                std::fflush(stdout);
                client->deleteLater();
                client = nullptr;
                stream.Reset();
            });
        }
    });

    QObject::connect(&server, &QTcpServer::acceptError, [&](QAbstractSocket::SocketError) {
        std::printf("[headless] acceptError: %s\n", qPrintable(server.errorString()));
        std::fflush(stdout);
    });

    QCoreApplication* app = QCoreApplication::instance();

    QTimer killer;
    if (opt.duration_s > 0) {
        killer.setSingleShot(true);
        killer.setInterval(opt.duration_s * 1000);
        QObject::connect(&killer, &QTimer::timeout, app, &QCoreApplication::quit);
        killer.start();
    }
    // 无 duration 时由外部终止 —— 那种情况下不会有 summary，属已知边界
    app->exec();

    const size_t partial = stream.pending();
    std::printf(
        "[headless] summary: frames=%llu decode_fail=%llu bad_header=%llu partial=%zu bytes=%llu\n",
        static_cast<unsigned long long>(st.frames),
        static_cast<unsigned long long>(st.decode_fail),
        static_cast<unsigned long long>(st.bad_header), partial,
        static_cast<unsigned long long>(st.bytes));
    std::fflush(stdout);

    int rc = 0;
    if (st.decode_fail != 0 || st.bad_header != 0) {
        rc = 1;
    }
    if (opt.expect >= 0 && st.frames != static_cast<quint64>(opt.expect)) {
        std::printf("[headless] EXPECT MISMATCH: got %llu, want %d\n",
                    static_cast<unsigned long long>(st.frames), opt.expect);
        std::fflush(stdout);
        rc = 1;
    }
    std::printf("[headless] exit=%d\n", rc);
    std::fflush(stdout);
    return rc;
}
