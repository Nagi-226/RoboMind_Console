#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QTimer>

class AiStatusChat;
class LanChatSink;

class GroupDiscussion : public QObject {
    Q_OBJECT
public:
    struct Config {
        int groupId{0};
        int hostId{0};
        static Config load();
    };

    GroupDiscussion(Config config, LanChatSink* ds, LanChatSink* gpt,
                    AiStatusChat* whale, AiStatusChat* dragon, QObject* parent);
    QJsonObject status() const;

signals:
    void boardLine(const QString& requestId, int turn, const QString& text);
    void boardCancelled(const QString& requestId);

private:
    void receive(const QJsonObject& message);
    void groupEvent(const QJsonObject& event);
    void nextTurn();
    void finish(const QString& result);

    Config config_;
    LanChatSink *ds_, *gpt_;
    AiStatusChat *whale_, *dragon_;
    QTimer deadline_;
    QQueue<QString> seen_;
    QString requestId_, question_, pendingAnswer_, pendingText_, result_;
    QJsonArray transcript_;
    bool awaitingMembers_{false};
};
