// MainWindow —— M3：TCP server + 实时 PPM 面板
//
// 职责：
//   1. 监听 9000，接受板子上报连接
//   2. 收字节 → FrameStream 切帧（处理粘包/半包）→ MessageCodec::Decode
//   3. 把解码结果渲染到 PPM 面板；Raw 面板显示最近一帧原文
//   4. 连接状态可见：未连接 / 已连接 / 断线

#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QHash>
#include <QMainWindow>
#include <QJsonObject>
#include <QQueue>
#include <QString>
#include <QTimer>

#include "frame_stream.h"

class QLabel;
class QTcpServer;
class QTcpSocket;
class LanChatSink;
class DisplayBridge;
class StatusQuery;
class BoardLogs;
class AiStatusChat;
class GroupDiscussion;
class DiscussionPlayback;
class SpeechBridge;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(quint16 port, QWidget* parent = nullptr, const QString& logDirectory = {});
    ~MainWindow() override;
    QJsonObject bridgeStatus() const;
    bool navigateBoard(const QString& id,const QString& action);
    void setLogCapture(bool enabled,const QString& port);

private slots:
    void OnNewConnection();
    void OnReadyRead();
    void OnDisconnected();
    void OnAcceptError(int code);

private:
    void BuildUi();
    void ApplyFrame(const QString& text);
    void SetConnState(const QString& text, const QString& color);
    void SetField(const QString& key, const QString& value);
    void showChatPersona(const QString& persona);
    void updateChatPresentation();
    void clearChatPresentation();

    quint16       port_;
    QTcpServer*   server_{nullptr};
    QTcpSocket*   client_{nullptr};
    FrameStream   stream_;
    LanChatSink*  sink_{nullptr};
    LanChatSink*  gptSink_{nullptr};
    DisplayBridge* display_{nullptr};
    StatusQuery* queries_{nullptr};
    BoardLogs* logs_{nullptr};
    AiStatusChat* ai_{nullptr};
    AiStatusChat* gptAi_{nullptr};
    GroupDiscussion* discussion_{nullptr};
    DiscussionPlayback* discussionPlayback_{nullptr};
    SpeechBridge* speech_{nullptr};

    struct ChatPresentation {
        QString persona, control, question, answer;
        qint64 expires=0,lastSent=0;
        bool answered=false,success=false;
    } chatPresentation_;
    QQueue<QString> recentPrivateMessages_;
    QTimer chatPresentationTimer_;

    QLabel*       conn_label_{nullptr};
    QLabel*       sink_label_{nullptr};
    QLabel*       status_hint_{nullptr};
    QLabel*       banner_{nullptr};
    QLabel*       raw_view_{nullptr};
    QHash<QString, QLabel*> field_labels_;   // key -> value label
    quint64       frames_{0};
    quint64       decode_failures_{0};
    quint64       bad_headers_{0};
    qint64        last_frame_ms_{0};
    QString       last_source_;
};

#endif  // MAIN_WINDOW_H
