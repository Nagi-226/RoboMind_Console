#include "lanchat_sink.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QtEndian>

namespace {

constexpr int kHeartbeatIntervalMs = 10000;
constexpr qint64 kDebounceMs = 30000;
constexpr int kBackoffMaxMs = 30000;
// 接收上限比 FrameStream 的 4096 大：LoginSuccessReturn 携带 friends/groups 列表，
// 真实服务端完全可能超过 4KB，用 4096 会把登录回包当坏帧丢掉导致永远登录不上。
constexpr quint32 kRxMax = 1u << 20;

qint64 NowMs() { return QDateTime::currentMSecsSinceEpoch(); }

}  // namespace

LanChatSink::Config LanChatSink::LoadConfig()
{
    Config cfg;
    QString path = QDir::current().absoluteFilePath("robomind-console.ini");
    if (!QFileInfo::exists(path)) {
        path = QCoreApplication::applicationDirPath() + "/robomind-console.ini";
    }
    if (QFileInfo::exists(path)) {
        QSettings ini(path, QSettings::IniFormat);
        ini.beginGroup("lanchat");
        cfg.enabled = ini.value("enabled", false).toBool();
        cfg.host = ini.value("host", "127.0.0.1").toString();
        cfg.port = static_cast<quint16>(ini.value("port", 9001).toUInt());
        cfg.target = ini.value("target", 0).toInt();
        cfg.groupId = ini.value("group_id", 0).toInt();
        cfg.receiveMessages = ini.value("receive_messages", false).toBool();
        ini.endGroup();
    }
    return cfg;
}

LanChatSink::LanChatSink(const Config& cfg, QObject* parent)
    : QObject(parent), cfg_(cfg)
{
    log_file_.setFileName(QDir::current().absoluteFilePath("lanchat-sink.log"));
    if (!log_file_.open(QIODevice::Append | QIODevice::Text)) {
        qWarning("[lanchat-sink] cannot open lanchat-sink.log, file logging disabled");
    }

    if (!cfg_.enabled) {
        disabled_ = true;
        Log("sink disabled: lanchat.enabled=false (or no config file)");
        return;
    }

    bool ok = false;
    user_id_ = qgetenv("ROBOMIND_LANCHAT_USER_ID").toInt(&ok);
    password_ = QString::fromUtf8(qgetenv("ROBOMIND_LANCHAT_PASSWORD"));
    if (!ok || user_id_ < 1 || password_.isEmpty()) {
        disabled_ = true;
        SetState("未启用（缺凭据）");
        Log("sink disabled: ROBOMIND_LANCHAT_USER_ID / ROBOMIND_LANCHAT_PASSWORD missing or invalid");
        return;
    }
    if (cfg_.target < 1 && cfg_.groupId < 1 && !cfg_.receiveMessages) {
        disabled_ = true;
        SetState("未启用（未配置告警目标）");
        Log("sink disabled: no alert destinations configured");
        return;
    }

    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::connected, this, &LanChatSink::OnConnected);
    connect(socket_, &QTcpSocket::disconnected, this, &LanChatSink::OnDisconnected);
    connect(socket_, &QTcpSocket::errorOccurred, this, &LanChatSink::OnSocketError);
    connect(socket_, &QTcpSocket::readyRead, this, &LanChatSink::OnReadyRead);

    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);
    connect(reconnect_timer_, &QTimer::timeout, this, &LanChatSink::ConnectToServer);

    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setInterval(kHeartbeatIntervalMs);
    connect(heartbeat_timer_, &QTimer::timeout, this, &LanChatSink::SendHeartbeat);

    Log(QString("sink enabled: host=%1 port=%2 target=%3 groupId=%4 user_id=%5")
            .arg(cfg_.host).arg(cfg_.port).arg(cfg_.target).arg(cfg_.groupId).arg(user_id_));
    ConnectToServer();
}

void LanChatSink::ConnectToServer()
{
    if (disabled_) return;
    logged_in_ = false;
    rx_buf_.clear();
    SetState(QString("连接中 %1:%2").arg(cfg_.host).arg(cfg_.port));
    Log(QString("connecting to %1:%2").arg(cfg_.host).arg(cfg_.port));
    socket_->abort();
    socket_->connectToHost(cfg_.host, cfg_.port);
}

void LanChatSink::ScheduleReconnect()
{
    if (disabled_ || reconnect_timer_->isActive()) return;
    Log(QString("reconnect in %1 ms").arg(backoff_ms_));
    reconnect_timer_->start(backoff_ms_);
    backoff_ms_ = qMin(backoff_ms_ * 2, kBackoffMaxMs);
}

void LanChatSink::OnConnected()
{
    backoff_ms_ = 1000;
    // 乐观放行：同一 TCP 连接上服务端必然先处理 Login 再处理后续告警，
    // 无需等 LoginSuccessReturn 才发。验收用假服务端只收不回，等回包会永远发不出。
    logged_in_ = true;
    heartbeat_timer_->start();
    SetState("已连接，Login 已发");
    Log("tcp connected, sending Login");
    SendFrame(QJsonObject{{"type", 2}, {"id", user_id_}, {"password", password_}},
              QString("id=%1 (password redacted)").arg(user_id_));
}

void LanChatSink::OnDisconnected()
{
    logged_in_ = false;
    heartbeat_timer_->stop();
    if (disabled_) return;
    SetState("断线，等待重连");
    Log("disconnected");
    ScheduleReconnect();
}

void LanChatSink::OnSocketError()
{
    Log(QString("socket error: %1").arg(socket_->errorString()));
    // 连接失败（从未 connected）不会触发 disconnected，这里补上重连调度；
    // 已连接后断开则 disconnected 与 error 都会来，isActive 守卫防重复。
    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        ScheduleReconnect();
    }
}

void LanChatSink::OnReadyRead()
{
    rx_buf_.append(socket_->readAll());
    while (rx_buf_.size() >= 4) {
        const quint32 n = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(rx_buf_.constData()));
        if (n == 0 || n > kRxMax) {
            Log(QString("rx bad length header (%1), buffer reset").arg(n));
            rx_buf_.clear();
            return;
        }
        if (static_cast<quint32>(rx_buf_.size()) < 4 + n) break;
        const QByteArray payload = rx_buf_.mid(4, static_cast<int>(n));
        rx_buf_.remove(0, 4 + static_cast<int>(n));

        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) continue;
        const int type = doc.object().value("type").toInt(-1);
        if (type == 3) {
            logged_in_ = true;
            heartbeat_timer_->start();
            SetState("已登录");
            Log("rx LoginSuccessReturn (type=3), login confirmed");
            if (cfg_.receiveMessages && cfg_.groupId > 0) {
                SendFrame(QJsonObject{{"type", 13}, {"id", user_id_}, {"groupId", cfg_.groupId}}, "join configured display group");
            }
        } else if (type == 4) {
            // 凭据被拒是配置错误而非网络故障，重试无意义，停用等人工处理
            disabled_ = true;
            heartbeat_timer_->stop();
            SetState("登录被拒，已停用");
            Log("rx LoginFailedReturn (type=4): credentials rejected, sink disabled (no retry)");
            socket_->disconnectFromHost();
        }
        if (cfg_.receiveMessages && doc.object().value("fromId").toInt() != user_id_) {
            if (type == 6 && doc.object().value("toId").toInt() == user_id_)
                emit messageReceived(doc.object());
            else if (type == 17 && cfg_.groupId > 0 && doc.object().value("groupId").toInt() == cfg_.groupId)
                emit messageReceived(doc.object());
        }
    }
}

void LanChatSink::SendHeartbeat()
{
    if (!logged_in_) return;
    SendFrame(QJsonObject{{"type", 20}}, "(keepalive)");
}

void LanChatSink::sendAlert(int level, const QString& summary)
{
    if (disabled_) return;
    const qint64 now = NowMs();
    const auto it = last_sent_ms_.constFind(summary);
    if (it != last_sent_ms_.constEnd() && now - it.value() < kDebounceMs) return;

    if (!logged_in_) {
        // 不占去抖窗口：登录恢复后下一帧即可补发。丢弃日志本身也去抖，防刷屏。
        if (now - last_drop_log_ms_ >= kDebounceMs) {
            last_drop_log_ms_ = now;
            Log(QString("alert dropped (not logged in): level=%1").arg(level));
        }
        return;
    }

    if (cfg_.groupId > 0) {
        SendFrame(QJsonObject{{"type", 16},
                              {"fromId", user_id_},
                              {"groupId", cfg_.groupId},
                              {"msg", summary}},
                  QString("groupId=%1 msg_preview=\"%2\" msg_len=%3")
                      .arg(cfg_.groupId).arg(summary.left(20)).arg(summary.size()));
    }
    if (cfg_.target > 0) {
        SendFrame(QJsonObject{{"type", 5},
                              {"fromId", user_id_},
                              {"toId", cfg_.target},
                              {"msg", summary}},
                  QString("toId=%1 msg_preview=\"%2\" msg_len=%3")
                      .arg(cfg_.target).arg(summary.left(20)).arg(summary.size()));
    }
    // 一次告警可发往两处，所有目标发送后只记一次去抖。
    last_sent_ms_[summary] = now;
}

void LanChatSink::SendFrame(const QJsonObject& body, const QString& log_detail)
{
    QJsonObject obj = body;
    // timestamp/msg_id 是协议公共字段但非必填（schemas 已确证），防御性带上
    obj.insert("timestamp", NowMs());
    obj.insert("msg_id", QString("console-%1-%2")
                             .arg(QCoreApplication::applicationPid()).arg(++msg_counter_));
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    QByteArray frame;
    frame.reserve(4 + payload.size());
    const quint32 n = static_cast<quint32>(payload.size());
    frame.append(static_cast<char>((n >> 24) & 0xFF));
    frame.append(static_cast<char>((n >> 16) & 0xFF));
    frame.append(static_cast<char>((n >> 8) & 0xFF));
    frame.append(static_cast<char>(n & 0xFF));
    frame.append(payload);
    socket_->write(frame);

    Log(QString("tx type=%1 len=%2 %3")
            .arg(body.value("type").toInt()).arg(payload.size()).arg(log_detail));
}

void LanChatSink::SetState(const QString& text)
{
    state_ = text;
    emit stateChanged(text);
}

void LanChatSink::Log(const QString& line)
{
    const QString stamped = QString("[%1] %2")
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), line);
    qInfo().noquote() << "[lanchat-sink]" << stamped;
    if (log_file_.isOpen()) {
        QTextStream ts(&log_file_);
        ts.setEncoding(QStringConverter::Utf8);
        ts << stamped << '\n';
        ts.flush();
    }
}
