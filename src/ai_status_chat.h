#pragma once
#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <functional>
class LanChatSink;
class StatusQuery;
class QNetworkReply;

// PC-owned, read-only model gateway. Questions and answers never become device commands.
class AiStatusChat : public QObject {
public:
    struct Config {
        bool enabled=false;
        QString provider="anthropic",endpoint,model,key;
        int timeoutMs=45000;
        static Config environment();
        bool valid() const;
    };
    AiStatusChat(LanChatSink* sink,StatusQuery* queries,std::function<QJsonObject()> state,QObject* parent=nullptr);
    AiStatusChat(const Config& config,LanChatSink* sink,StatusQuery* queries,std::function<QJsonObject()> state,QObject* parent=nullptr);
    bool consume(const QJsonObject& message);
    QJsonObject status() const;
private:
    struct Session { QJsonArray history; qint64 touched=0; };
    struct Job { QJsonObject original; QString conversation,question,context; int remaining=3; qint64 created=0; };
    void query(const QString& id,const QString& kind,const QString& body);
    void invoke(const QString& id);
    void finish(const QString& id,const QString& answer,bool success);
    void respond(const QJsonObject& original,const QString& text);
    void prune();
    Config config_;
    LanChatSink* sink_;
    StatusQuery* queries_;
    std::function<QJsonObject()> state_;
    QNetworkAccessManager network_;
    QHash<QString,Job> jobs_;
    QHash<QString,Session> sessions_;
    QHash<QString,qint64> seen_,users_;
    QTimer timer_;
    quint64 requested_=0,replied_=0,failed_=0;
};
