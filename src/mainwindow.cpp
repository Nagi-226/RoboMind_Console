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
#include <QUuid>
#include <QVBoxLayout>

#include "interconnect/message_codec.hpp"
#include "interconnect/message_envelope.hpp"
#include "lanchat_sink.h"
#include "display_bridge.h"
#include "status_query.h"
#include "board_logs.h"
#include "ai_status_chat.h"
#include "speech_bridge.h"
#include <QCoreApplication>
#include <QStandardPaths>

namespace {

using vr::core::ErrorCode;
using vr::interconnect::MessageCodec;
using vr::interconnect::MessageEnvelope;

bool ShowsPersona(const QJsonObject& view,const QString& persona)
{
    return view.value("immersion").toBool() && view.value("persona").toString()==persona;
}

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

    const auto lanConfig = LanChatSink::LoadConfig();
    sink_ = new LanChatSink(lanConfig, this);
    display_ = new DisplayBridge(this);
    queries_=new StatusQuery(sink_,display_,[this]{return bridgeStatus();},this);
    logs_=new BoardLogs(logDirectory.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/board-logs":logDirectory,this);
    queries_->setLogReply([this](const QString& filter){return logs_->reply(filter);});
    ai_=new AiStatusChat(sink_,queries_,[this]{return bridgeStatus();},this);
    speech_=new SpeechBridge(display_,this);
    chatPresentationTimer_.setInterval(200);
    connect(&chatPresentationTimer_,&QTimer::timeout,this,&MainWindow::updateChatPresentation);
    connect(display_,&DisplayBridge::viewChanged,this,&MainWindow::updateChatPresentation);
    const int gptId = qEnvironmentVariableIntValue("ROBOMIND_GPT_USER_ID");
    const auto gptPassword = qEnvironmentVariable("ROBOMIND_GPT_PASSWORD");
    const auto gptConfig = AiStatusChat::Config::gptEnvironment();
    if (gptId > 0 && gptId != sink_->userId() && !gptPassword.isEmpty() && gptConfig.enabled && gptConfig.valid()) {
        auto privateChat = lanConfig;
        privateChat.target = 0;
        privateChat.groupId = 0;
        privateChat.receiveMessages = true;
        privateChat.logFile = "lanchat-gpt-sink.log";
        gptSink_ = new LanChatSink(privateChat,gptId,gptPassword,this);
        gptAi_ = new AiStatusChat(gptConfig,gptSink_,queries_,[this]{
            auto state=bridgeStatus();
            state["lanchat_state"]=gptSink_->stateText();
            return state;
        },this);
    }
    const auto updateChat = [this] {
        auto label = QString("LanChat: %1").arg(sink_->stateText());
        if(gptSink_)label += QString(" · GPT: %1").arg(gptSink_->stateText());
        sink_label_->setText(label);
        display_->setLanChatConnected(sink_->online() || (gptSink_ && gptSink_->online()));
    };
    const auto wireChat = [this,updateChat](LanChatSink* account,AiStatusChat* chat,const QString& persona) {
        connect(account,&LanChatSink::stateChanged,this,updateChat);
        connect(account,&LanChatSink::messageReceived,this,[this,account,chat,persona](const QJsonObject& message){
            const int sender=message.value("fromId").toInt();
            if(sender==sink_->userId()||(gptSink_&&sender==gptSink_->userId()))return;
            if(gptSink_&&sender>0&&message.value("type").toInt()==6&&!message.value("msg").toString().trimmed().isEmpty()){
                const auto id=message.value("msg_id").toVariant().toString();
                const auto key=QString::number(account->userId())+":"+id;
                if(!id.isEmpty()&&!recentPrivateMessages_.contains(key)){
                    if(recentPrivateMessages_.size()>=256)recentPrivateMessages_.dequeue();
                    recentPrivateMessages_.enqueue(key);
                    showChatPersona(persona);
                }
            }
            display_->enqueue(message);
            if(StatusQuery::recognizes(message))queries_->consume(message,account);else chat->consume(message);
        });
        connect(chat,&AiStatusChat::questionStarted,this,[this,persona](const QString& id,bool direct){
            if(!chatPresentation_.control.isEmpty()){
                if(direct&&chatPresentation_.persona==persona){
                    chatPresentation_.question=id;
                    chatPresentation_.answer.clear();
                    chatPresentation_.answered=false;
                }
                return;
            }
            if(!gptSink_||ShowsPersona(display_->status().value("view").toObject(),persona))speech_->begin(id);
        });
        connect(chat,&AiStatusChat::answerReady,this,[this](const QString& id,const QString& answer,bool success){
            if(id==chatPresentation_.question){
                chatPresentation_.answer=answer;
                chatPresentation_.answered=true;
                chatPresentation_.success=success;
            }else speech_->answer(id,answer,success);
        });
    };
    wireChat(sink_,ai_,"whale");
    if(gptSink_)wireChat(gptSink_,gptAi_,"dragon");
    updateChat();
}

MainWindow::~MainWindow()
{
    // Socket disconnect handlers update widgets; detach while they still exist.
    clearChatPresentation();
    disconnect(sink_,nullptr,this,nullptr);
    if(gptSink_)disconnect(gptSink_,nullptr,this,nullptr);
    if(client_){
        disconnect(client_,nullptr,this,nullptr);
        display_->attach(nullptr);
        client_->abort();
        client_=nullptr;
    }
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
    clearChatPresentation();
    if (client_ != nullptr) {
        client_->deleteLater();
        client_ = nullptr;
    }
    stream_.Reset();
    SetConnState("断线（等待重连）", "#FFB020");
    status_hint_->setText("板子已断开 —— 等待其自动重连（本应用无需重启）");
}

void MainWindow::showChatPersona(const QString& persona)
{
    if(!client_)return;
    if(chatPresentation_.persona==persona)return;
    if(chatPresentation_.control.isEmpty()&&ShowsPersona(display_->status().value("view").toObject(),persona))return;

    // Use a new control ID even if the board still shows the requested persona:
    // an earlier switch to the other persona may already be on its way.
    speech_->cancel();
    chatPresentation_={};
    chatPresentation_.persona=persona;
    chatPresentation_.control=QUuid::createUuid().toString(QUuid::Id128);
    chatPresentation_.expires=QDateTime::currentMSecsSinceEpoch()+5000;
    chatPresentationTimer_.start();
    updateChatPresentation();
}

void MainWindow::updateChatPresentation()
{
    if(chatPresentation_.control.isEmpty())return;
    const auto timestamp=QDateTime::currentMSecsSinceEpoch();
    const auto status=display_->status();
    const bool dragon=chatPresentation_.persona=="dragon";
    const auto view=status.value("view").toObject();
    if(timestamp>=chatPresentation_.expires){
        clearChatPresentation();
        return;
    }
    if(view.value("control").toString()==chatPresentation_.control&&ShowsPersona(view,chatPresentation_.persona)){
        const auto ready=chatPresentation_;
        clearChatPresentation();
        if(!ready.question.isEmpty()){
            speech_->begin(ready.question);
            if(ready.answered)speech_->answer(ready.question,ready.answer,ready.success);
        }
        return;
    }
    if(!status.value("terminal_ready").toBool())return;
    if(!status.value("immersion_supported").toBool()||!status.value("navigation").toBool()||
       (dragon&&!status.value("dragon_supported").toBool())){
        clearChatPresentation();
        return;
    }
    if(timestamp-chatPresentation_.lastSent>=500&&
       display_->navigate(chatPresentation_.control,dragon?"immersion_dragon":"immersion"))
        chatPresentation_.lastSent=timestamp;
}

void MainWindow::clearChatPresentation()
{
    chatPresentationTimer_.stop();
    chatPresentation_={};
}

bool MainWindow::navigateBoard(const QString& id,const QString& action){
    const auto view=display_->status().value("view").toObject();
    const bool samePersona=view.value("immersion").toBool()&&
        ((action=="immersion"&&view.value("persona").toString("whale")=="whale")||
         (action=="immersion_dragon"&&view.value("persona").toString()=="dragon"));
    if(action=="stop_speech"||action=="terminal"||action=="audio_test")speech_->cancel();
    const bool accepted=display_->navigate(id,action);
    if((accepted&&!action.startsWith("volume_"))||action=="stop_speech")clearChatPresentation();
    if(accepted&&!samePersona&&!action.startsWith("volume_"))speech_->cancel();
    return accepted;
}
void MainWindow::setLogCapture(bool enabled,const QString& port){if(enabled)logs_->start(port);else logs_->stop();}
QJsonObject MainWindow::bridgeStatus() const
{
    return {{"updated_ms", QDateTime::currentMSecsSinceEpoch()},{"console_version",QCoreApplication::applicationVersion()},
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
            {"gpt_lanchat_state",gptSink_?gptSink_->stateText():QString()},
            {"gpt_account_id",gptSink_?gptSink_->userId():0},
            {"gpt_ai",gptAi_?gptAi_->status():QJsonObject{}},
            {"display", display_->status()},
            {"speech",speech_?speech_->status():QJsonObject{}},
            {"logs", logs_ ? logs_->status() : QJsonObject()},
            {"ai", ai_ ? ai_->status() : QJsonObject()},
            {"queries", queries_ ? queries_->status() : QJsonObject()}};
}

void MainWindow::OnAcceptError(int code)
{
    SetConnState("监听异常", "#FF5C5C");
    status_hint_->setText(QString("acceptError=%1: %2").arg(code).arg(server_->errorString()));
}
