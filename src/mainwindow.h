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
#include <QString>

#include "frame_stream.h"

class QLabel;
class QTcpServer;
class QTcpSocket;
class LanChatSink;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(quint16 port, QWidget* parent = nullptr);

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

    quint16       port_;
    QTcpServer*   server_{nullptr};
    QTcpSocket*   client_{nullptr};
    FrameStream   stream_;
    LanChatSink*  sink_{nullptr};

    QLabel*       conn_label_{nullptr};
    QLabel*       sink_label_{nullptr};
    QLabel*       status_hint_{nullptr};
    QLabel*       banner_{nullptr};
    QLabel*       raw_view_{nullptr};
    QHash<QString, QLabel*> field_labels_;   // key -> value label
    quint64       frames_{0};
};

#endif  // MAIN_WINDOW_H
