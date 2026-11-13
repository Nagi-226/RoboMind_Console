#include "headless.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdarg>
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
    quint64 first_ts{0};
    quint64 last_ts{0};
    bool    has_ts{false};
    quint64 decode_fail{0};
    quint64 bytes{0};
    quint64 bad_header{0};
};

// 日志时间戳前缀：没有它就无法判断"连接发生在第几秒"（本轮诊断绕远的直接原因）
QElapsedTimer* g_clock = nullptr;

void Log(const char* fmt, ...)
{
    const double t = (g_clock != nullptr) ? static_cast<double>(g_clock->elapsed()) / 1000.0 : 0.0;
    std::printf("[headless] +%.1fs ", t);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

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
    QElapsedTimer clock;
    clock.start();
    g_clock = &clock;

    QTcpServer server;
    FrameStream stream;
    Stats st;
    QByteArray chunks;   // 本轮 readyRead 收到的原始字节

    Log("listening on %u", static_cast<unsigned>(opt.port));
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
            Log("client connected");
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
                    if (!st.has_ts) {
                        st.first_ts = static_cast<quint64>(env.timestamp_ms);
                        st.has_ts = true;
                    }
                    st.last_ts = static_cast<quint64>(env.timestamp_ms);
                    ++st.frames;
                    if (!opt.quiet) {
                        const QString p = QString::fromStdString(env.payload);
                        std::printf(
                            "[headless] +%.1fs frame#%llu seq=%llu ts=%llu dv=%s dl=%s ev=%s "
                            "es=%s ab=%s dg=%s\n",
                            static_cast<double>(clock.elapsed()) / 1000.0,
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
                Log("client disconnected");
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
    const double window_s = static_cast<double>(clock.elapsed()) / 1000.0;
    /*
     * fps 用**首末帧 ts 跨度**算，不是用 window。
     * 原因（实测）：headless 先启动、板子晚连时，window 里包含了"等连接"的空档，
     * 用 window 会把 9.5 fps 算成 5.0 fps（300 帧 / 60 s）。
     * N 帧之间只有 N-1 个间隔，故用 frames-1。
     */
    const double span_s = (st.has_ts && st.last_ts > st.first_ts)
                              ? (static_cast<double>(st.last_ts - st.first_ts) / 1000.0)
                              : 0.0;
    const double fps = (span_s > 0.0 && st.frames > 1)
                           ? (static_cast<double>(st.frames - 1) / span_s)
                           : 0.0;
    std::printf(
        "[headless] summary: frames=%llu window=%.1fs span=%.1fs fps=%.2f decode_fail=%llu "
        "bad_header=%llu partial=%zu bytes=%llu\n",
        static_cast<unsigned long long>(st.frames), window_s, span_s, fps,
        static_cast<unsigned long long>(st.decode_fail),
        static_cast<unsigned long long>(st.bad_header), partial,
        static_cast<unsigned long long>(st.bytes));
    std::fflush(stdout);

    int rc = 0;
    if (st.decode_fail != 0 || st.bad_header != 0) {
        rc = 1;
    }
    if (opt.min_fps >= 0.0 && fps < opt.min_fps) {
        std::printf("[headless] FPS TOO LOW: %.2f < %.2f\n", fps, opt.min_fps);
        std::fflush(stdout);
        rc = 1;
    }
    if (opt.max_fps >= 0.0 && fps > opt.max_fps) {
        std::printf("[headless] FPS TOO HIGH: %.2f > %.2f\n", fps, opt.max_fps);
        std::fflush(stdout);
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
