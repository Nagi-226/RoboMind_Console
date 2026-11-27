// LanChatSink —— 把 PPM 告警转发到 LanChat-Next 服务端（方案 C：Console 做桥）
//
// 帧格式与 VVOS wire 同构：[u32 大端长度] + payload，payload 换成 JSON。
// 发送四个消息类型：Login(2) / SendMsg(5) / SendGroupMsg(16) / Heartbeat(20)。
// 接收方向处理登录回包；启用 receiveMessages 时接收私聊 / 配置群消息并桥接到板端。
//
// 凭据只从环境变量读（ROBOMIND_LANCHAT_USER_ID / ROBOMIND_LANCHAT_PASSWORD），
// 不落配置文件、不落日志。缺凭据 → 静默降级为不启用。

#ifndef LANCHAT_SINK_H
#define LANCHAT_SINK_H

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpSocket;
class QTimer;
class QJsonObject;

class LanChatSink : public QObject {
    Q_OBJECT

public:
    struct Config {
        bool    enabled{false};
        QString host{"127.0.0.1"};
        quint16 port{9001};
        int     target{0};    // 私聊：接收者数字用户 id
        int     groupId{0};   // 群发：群 id。0 = 不发群
        bool    receiveMessages{false}; // ReceiveMsg -> VVOS display; default preserves send-only configs.
        QString logFile{"lanchat-sink.log"};
    };

    // 读 robomind-console.ini：先当前工作目录，后 exe 所在目录；无文件 → 全默认（关闭）。
    static Config LoadConfig();

    explicit LanChatSink(const Config& cfg, QObject* parent = nullptr);
    LanChatSink(const Config& cfg, int userId, const QString& password, QObject* parent);

    // 同一 summary 30 秒内不重复发。未登录时丢弃（不占去抖窗口，恢复后可补发）。
    void sendAlert(int level, const QString& summary);
    bool sendReply(const QJsonObject& original,const QString& text);

    QString stateText() const { return state_; }
    bool online() const { return login_confirmed_; }
    int userId() const { return user_id_; }

signals:
    void stateChanged(const QString& text);
    void messageReceived(const QJsonObject& message);

private:
    void ConnectToServer();
    void ScheduleReconnect();
    void OnConnected();
    void OnDisconnected();
    void OnSocketError();
    void OnReadyRead();
    void SendHeartbeat();
    void SendFrame(const QJsonObject& body, const QString& log_detail);
    void SetState(const QString& text);
    void Log(const QString& line);

    Config      cfg_;
    int         user_id_{0};
    QString     password_;          // 只用于 Login 包，绝不进日志
    QTcpSocket* socket_{nullptr};
    QTimer*     reconnect_timer_{nullptr};
    QTimer*     heartbeat_timer_{nullptr};
    QByteArray  rx_buf_;
    int         backoff_ms_{1000};
    bool        logged_in_{false};
    bool        login_confirmed_{false};
    bool        disabled_{false};   // 配置关闭 / 缺凭据 / 登录被拒（type=4）
    quint64     msg_counter_{0};
    QHash<QString, qint64> last_sent_ms_;
    qint64      last_drop_log_ms_{0};
    QFile       log_file_;
    QString     state_{"未启用"};
};

#endif  // LANCHAT_SINK_H
