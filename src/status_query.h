#pragma once
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QTimer>
#include <functional>
class LanChatSink;
class DisplayBridge;

// Bounded read-only queries; LAN identity/routing remains on the PC.
class StatusQuery : public QObject {
public:
    StatusQuery(LanChatSink* sink,DisplayBridge* display,std::function<QJsonObject()> state,QObject* parent=nullptr);
    void consume(const QJsonObject& message);
    QJsonObject status() const;
    void setLogReply(std::function<QString(const QString&)> reader) { logReply_ = std::move(reader); }
private:
    struct Pending { QJsonObject original; QString kind; qint64 created=0,sent=0; };
    void tick();
    void receive(const QString& payload);
    void reply(const QJsonObject& original,const QString& kind,const QString& body);
    QString format(const QString& kind,const QHash<QString,QString>& values) const;
    QString linkSummary() const;
    LanChatSink* sink_;
    DisplayBridge* display_;
    std::function<QJsonObject()> state_;
    std::function<QString(const QString&)> logReply_;
    QHash<QString,Pending> pending_;
    QHash<QString,qint64> processed_,users_,throttled_;
    QTimer timer_;
    quint64 requested_=0,replied_=0,timeouts_=0,invalid_=0;
};
