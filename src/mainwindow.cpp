#include "mainwindow.h"

#include <QFont>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QSplitter>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVBoxLayout>

#include "interconnect/message_codec.hpp"
#include "interconnect/message_envelope.hpp"
#include "lanchat_sink.h"
#include "display_bridge.h"
#include "status_query.h"
#include "board_logs.h"
#include "ai_status_chat.h"
#include <QStandardPaths>

namespace {

using vr::core::ErrorCode;
using vr::interconnect::MessageCodec;
using vr::interconnect::MessageEnvelope;

QMap<QString, QString> ParsePayload(const QString& payload)
{
    QMap<QString, QString> kv;
    for (const QString& pair : payload.split(';', Qt::SkipEmptyParts)) {
        const int eq = pair.indexOf('=');
        if (eq > 0) {
            kv.insert(pair.left(eq).trimmed(), pair.mid(eq + 1).trimmed());
        }
    }
    return kv;
}

// 颜色口径与板端 UI 一致：无人用中性蓝灰，**绝不红**（红只留给"需干预的人"）
QPair<QString, QString> DeriveState(const QMap<QString, QString>& kv)
{
    const QString dv = kv.value("dv", "N");
    if (dv == "N") return {"未计算 / 无人", "#A7B0C0"};
    if (dv == "I") return {"结果不可信", "#6B7484"};
    const QString dl = kv.value("dl", "0");
    if (dl == "0") return {"正常", "#35C46A"};
    if (dl == "1") return {"轻度疲劳", "#FFB020"};
    if (dl == "2") return {"中度疲劳", "#FF5C5C"};
    return {"重度疲劳", "#FF5C5C"};
}

QFrame* MakeCard(const QString& title, const QString& hint, QLabel** value_out)
{
    auto* card = new QFrame;
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet("QFrame { background: #1B1F2A; border: 1px solid #2E3444;"
                        " border-radius: 6px; }");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(2);

    auto* t = new QLabel(title);
    t->setStyleSheet("color: #A7B0C0; border: none;");
    auto* v = new QLabel("—");
    QFont mono("Consolas");
    mono.setPointSize(12);
    mono.setBold(true);
    v->setFont(mono);
    v->setStyleSheet("color: #F2F4F8; border: none;");
    auto* h = new QLabel(hint);
    h->setStyleSheet("color: #6B7484; border: none;");

    layout->addWidget(t);
    layout->addWidget(v);
    layout->addWidget(h);
    *value_out = v;
    return card;
}

}  // namespace

MainWindow::MainWindow(quint16 port, QWidget* parent, const QString& logDirectory)
    : QMainWindow(parent), port_(port)
{
    BuildUi();

    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &MainWindow::OnNewConnection);
    connect(server_, &QTcpServer::acceptError, this, &MainWindow::OnAcceptError);

    if (!server_->listen(QHostAddress::AnyIPv4, port_)) {
        SetConnState(QString("监听失败: %1").arg(server_->errorString()), "#FF5C5C");
        status_hint_->setText(
            QString("无法监听 %1 —— 若被防火墙拦截，请先放行该端口（不是代码问题）").arg(port_));
    } else {
        SetConnState("未连接", "#A7B0C0");
        status_hint_->setText(QString("正在监听 %1，等待板子上报…").arg(port_));
    }

    sink_ = new LanChatSink(LanChatSink::LoadConfig(), this);
    display_ = new DisplayBridge(this);
    connect(sink_, &LanChatSink::messageReceived, display_, &DisplayBridge::enqueue);
    queries_=new StatusQuery(sink_,display_,[this]{return bridgeStatus();},this);
    logs_=new BoardLogs(logDirectory.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/board-logs":logDirectory,this);
    queries_->setLogReply([this](const QString& filter){return logs_->reply(filter);});
    ai_=new AiStatusChat(sink_,queries_,[this]{return bridgeStatus();},this);
    connect(sink_,&LanChatSink::messageReceived,this,[this](const QJsonObject& message){
        if(StatusQuery::recognizes(message))queries_->consume(message);else ai_->consume(message);
    });
    connect(sink_, &LanChatSink::stateChanged, this, [this](const QString& s) {
        sink_label_->setText(QString("LanChat: %1").arg(s));
        display_->setLanChatConnected(s == "已登录");
    });
    sink_label_->setText(QString("LanChat: %1").arg(sink_->stateText()));
}

void MainWindow::BuildUi()
{
    setWindowTitle("RoboMind Console");
    resize(1100, 720);

    // ---- PPM 面板 ----
    auto* ppm = new QWidget;
    auto* root = new QVBoxLayout(ppm);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* top = new QHBoxLayout;
    banner_ = new QLabel("等待数据…");
    QFont bf = banner_->font();
    bf.setPointSize(bf.pointSize() + 2);
    bf.setBold(true);
    banner_->setFont(bf);
    banner_->setStyleSheet("background: #1B1F2A; color: #A7B0C0; border-radius: 6px; padding: 10px;");
    top->addWidget(banner_, 1);

    conn_label_ = new QLabel("未连接");
    conn_label_->setStyleSheet("color: #A7B0C0;");
    top->addWidget(conn_label_);

    sink_label_ = new QLabel("LanChat: 未启用");
    sink_label_->setStyleSheet("color: #6B7484;");
    top->addWidget(sink_label_);
    root->addLayout(top);

    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    const struct { const char* key; const char* title; const char* hint; } fields[] = {
        {"dv", "dl 有效性", "N/I/O"}, {"dl", "疲劳等级", "0..3"},
        {"ev", "es 有效性", "N/I/O"}, {"es", "睁闭眼", "0睁 1闭"},
        {"mv", "ms 有效性", "N/I/O"}, {"ms", "张嘴", "0闭 1张"},
        {"ab", "离开时长", "×100ms"}, {"dg", "降级位图", "NO_FACE=1"},
    };
    int i = 0;
    for (const auto& f : fields) {
        QLabel* v = nullptr;
        grid->addWidget(MakeCard(f.title, QString("%1 · %2").arg(f.key).arg(f.hint), &v),
                        i / 4, i % 4);
        field_labels_.insert(f.key, v);
        ++i;
    }
    root->addLayout(grid);

    status_hint_ = new QLabel("—");
    status_hint_->setWordWrap(true);
    status_hint_->setStyleSheet("color: #FFB020;");
    root->addWidget(status_hint_);
    root->addStretch(1);

    // ---- Raw 面板 ----
    auto* raw = new QFrame;
    raw->setFrameShape(QFrame::StyledPanel);
    auto* rl = new QVBoxLayout(raw);
    rl->setContentsMargins(12, 12, 12, 12);
    auto* rt = new QLabel("Raw（最近一帧原文）");
    QFont rf = rt->font();
    rf.setPointSize(rf.pointSize() + 2);
    rf.setBold(true);
    rt->setFont(rf);
    raw_view_ = new QLabel("—");
    raw_view_->setWordWrap(true);
    raw_view_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono("Consolas");
    mono.setPointSize(9);
    raw_view_->setFont(mono);
    raw_view_->setStyleSheet("color: #A7B0C0;");
    rl->addWidget(rt);
    rl->addWidget(raw_view_);
    rl->addStretch(1);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(ppm);
    splitter->addWidget(raw);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    setCentralWidget(splitter);

    statusBar()->showMessage("M3 · TCP 上报接收端");
}

void MainWindow::SetConnState(const QString& text, const QString& color)
{
    conn_label_->setText(text);
    conn_label_->setStyleSheet(QString("color: %1;").arg(color));
}

void MainWindow::OnNewConnection()
{
    while (server_->hasPendingConnections()) {
        QTcpSocket* s = server_->nextPendingConnection();
        if (client_ != nullptr) {
            // 只服务一个上报源；后来的连接直接拒绝，避免帧交错
            s->disconnectFromHost();
            continue;
        }
        client_ = s;
        display_->attach(client_);
        stream_.Reset();
        connect(client_, &QTcpSocket::readyRead, this, &MainWindow::OnReadyRead);
        connect(client_, &QTcpSocket::disconnected, this, &MainWindow::OnDisconnected);
        SetConnState("已连接", "#35C46A");
        status_hint_->setText(
            QString("已连接：%1:%2").arg(client_->peerAddress().toString()).arg(client_->peerPort()));
    }
}

void MainWindow::OnReadyRead()
{
    if (client_ == nullptr) return;

    std::vector<std::string> frames;
    const QByteArray chunk = client_->readAll();
    stream_.Feed(chunk.constData(), static_cast<size_t>(chunk.size()), &frames);

    if (stream_.bad_header()) {
        ++bad_headers_;
        status_hint_->setText("收到非法长度头 —— 流已错位，已清空缓冲重新对齐");
        stream_.Reset();
    }
    for (const std::string& f : frames) {
        ApplyFrame(QString::fromStdString(f));
    }
}

void MainWindow::ApplyFrame(const QString& text)
{
    MessageEnvelope env;
    const ErrorCode rc = MessageCodec::Decode(text.toStdString(), &env);
    if (rc != ErrorCode::kOk) {
        ++decode_failures_;
        status_hint_->setText(QString("解码失败 rc=%1（帧已丢弃）")
                                  .arg(static_cast<int>(rc)));
        return;
    }
    if (env.topic.rfind("ui/", 0) == 0 || env.topic=="device/query_reply") { display_->receive(env); return; }
    ++frames_;
    last_frame_ms_ = QDateTime::currentMSecsSinceEpoch();
    last_source_ = QString::fromStdString(env.source);
    raw_view_->setText(text);

    const QMap<QString, QString> kv = ParsePayload(QString::fromStdString(env.payload));
    const auto state = DeriveState(kv);

    banner_->setText(QString("感知状态：%1").arg(state.first));
    banner_->setStyleSheet(QString("background: %1; color: #12141C; border-radius: 6px;"
                                   " padding: 10px;").arg(state.second));

    for (auto it = field_labels_.begin(); it != field_labels_.end(); ++it) {
        const QString v = kv.value(it.key(), "<缺失>");
        it.value()->setText(v);
    }

    // dv=N/I 时 dl 数值无意义（板端 wire_codec 注释明确），只在结果有效时告警。
    // attention_level 未上 wire（payload 仅 dv/dl/ev/es/mv/ms/ab/dg 8 键），第一版不判。
    if (kv.value("dv") == "O") {
        const int dl = kv.value("dl", "0").toInt();
        if (dl >= 2) {
            sink_->sendAlert(dl, QString("PPM 疲劳告警：drowsy_level=%1").arg(dl));
        }
    }

    statusBar()->showMessage(
        QString("seq=%1  ts=%2 ms  已收 %3 帧  source=%4")
            .arg(static_cast<qulonglong>(env.sequence))
            .arg(static_cast<qulonglong>(env.timestamp_ms))
            .arg(frames_)
            .arg(QString::fromStdString(env.source)));
}

void MainWindow::OnDisconnected()
{
    if (client_ != nullptr) {
        client_->deleteLater();
        client_ = nullptr;
    }
    stream_.Reset();
    SetConnState("断线（等待重连）", "#FFB020");
    status_hint_->setText("板子已断开 —— 等待其自动重连（本应用无需重启）");
}

bool MainWindow::navigateBoard(const QString& id,const QString& action){return display_->navigate(id,action);}
void MainWindow::setLogCapture(bool enabled,const QString& port){if(enabled)logs_->start(port);else logs_->stop();}
QJsonObject MainWindow::bridgeStatus() const
{
    return {{"updated_ms", QDateTime::currentMSecsSinceEpoch()},
            {"listening", server_->isListening()},
            {"listen_port", port_},
            {"listen_error", server_->isListening() ? QString() : server_->errorString()},
            {"board_connected", client_ && client_->state() == QAbstractSocket::ConnectedState},
            {"board_peer", client_ ? client_->peerAddress().toString() : QString()},
            {"frames", static_cast<qint64>(frames_)},
            {"decode_failures", static_cast<qint64>(decode_failures_)},
            {"bad_headers", static_cast<qint64>(bad_headers_)},
            {"last_frame_ms", last_frame_ms_},
            {"source", last_source_},
            {"lanchat_state", sink_->stateText()},
            {"display", display_->status()},
            {"logs", logs_ ? logs_->status() : QJsonObject()},
            {"ai", ai_ ? ai_->status() : QJsonObject()},
            {"queries", queries_ ? queries_->status() : QJsonObject()}};
}

void MainWindow::OnAcceptError(int code)
{
    SetConnState("监听异常", "#FF5C5C");
    status_hint_->setText(QString("acceptError=%1: %2").arg(code).arg(server_->errorString()));
}
