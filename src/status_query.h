#pragma once
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QTimer>
#include <functional>
class LanChatSink;
class DisplayBridge;

// Bounded read-only queries; LAN identity/routing remains on the PC.
class StatusQuery : public QObject {
public:
    StatusQuery(LanChatSink* sink,DisplayBridge* display,std::function<QJsonObject()> state,QObject* parent=nullptr);
    void consume(const QJsonObject& message, LanChatSink* sender = nullptr);
    static bool recognizes(const QJsonObject& message);
    void snapshot(const QString& kind, std::function<void(const QString&)> completed);
    QJsonObject status() const;
    void setLogReply(std::function<QString(const QString&)> reader) { logReply_ = std::move(reader); }
private:
    using Result = std::function<void(const QString&)>;
    struct Pending { QString kind; qint64 created=0,sent=0; QList<Result> waiters; };
    void enqueue(const QString& kind, Result completed);
    qsizetype pendingCount() const;
    void complete(const Pending& pending, const QString& body);
    void tick();
    void receive(const QString& payload);
    void reply(LanChatSink* sender,const QJsonObject& original,const QString& kind,const QString& body);
    QString format(const QString& kind,const QHash<QString,QString>& values) const;
    QString linkSummary(const LanChatSink* sender) const;
    LanChatSink* sink_;
    DisplayBridge* display_;
    std::function<QJsonObject()> state_;
    std::function<QString(const QString&)> logReply_;
    QHash<QString,Pending> pending_;
    QString inFlight_;
    QHash<QString,qint64> processed_,users_,throttled_;
    QTimer timer_;
    quint64 requested_=0,replied_=0,timeouts_=0,invalid_=0;
};
