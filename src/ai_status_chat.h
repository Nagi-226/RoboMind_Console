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
    Q_OBJECT
public:
    struct Config {
        bool enabled=false;
        QString provider="anthropic",endpoint,model,key,reasoningEffort;
        int timeoutMs=45000;
        int maxTokens=1200,dailyLimit=100,minIntervalMs=2000,maxConcurrent=4;
        QString usageFile;
        static Config environment();
        bool valid() const;
    };
    AiStatusChat(LanChatSink* sink,StatusQuery* queries,std::function<QJsonObject()> state,QObject* parent=nullptr);
    AiStatusChat(const Config& config,LanChatSink* sink,StatusQuery* queries,std::function<QJsonObject()> state,QObject* parent=nullptr);
    bool consume(const QJsonObject& message);
    QJsonObject status() const;
signals:
    void questionStarted(const QString& id);
    void answerReady(const QString& id,const QString& answer,bool success);
private:
    struct Session { QJsonArray history; qint64 touched=0; };
    struct Job { QJsonObject original; QString conversation,question,context; int remaining=3; qint64 created=0; };
    void query(const QString& id,const QString& kind,const QString& body);
    void invoke(const QString& id);
    void finish(const QString& id,const QString& answer,bool success);
    void respond(const QJsonObject& original,const QString& text);
    void prune();
    void loadUsage();
    bool saveUsage();
    void resetDay();
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
    QString usageDay_,lastError_,lastErrorKind_;
    qint64 dailyRequested_=0,inputTokens_=0,outputTokens_=0,cooldownUntil_=0,lastInvoke_=0;
    int lastHttp_=0;
};
