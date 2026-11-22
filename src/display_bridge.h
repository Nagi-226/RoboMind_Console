#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QByteArray>
#include "interconnect/message_envelope.hpp"
class QTcpSocket;
class DisplayBridge:public QObject {
    Q_OBJECT
public:
    explicit DisplayBridge(QObject* parent=nullptr);
    void attach(QTcpSocket* socket);
    void setLanChatConnected(bool connected);
    void enqueue(const QJsonObject& message);
    void receive(const vr::interconnect::MessageEnvelope& envelope);
    QJsonObject status() const;
    bool navigate(const QString& id,const QString& action);
    bool requestStatus(const QString& id,const QString& kind);
signals:
    void statusReply(const QString& payload);
private:
    struct Pending {QString id,payload; qint64 created=0,lastSent=0;};
    void pump();
    void projectState();
    bool send(const QString& topic,const QString& payload,int ttl);
    void record(const QString& id,const QString& state,const QString& reason={});
    QPointer<QTcpSocket> board_;
    QHash<QString,Pending> pending_;
    QJsonArray deliveries_;
    QTimer timer_;
    quint64 sequence_=0,rendered_=0,rejected_=0;
    bool ready_=false,lanchat_=false;
    bool navigation_=false;
    bool queries_=false;
    QJsonObject view_;
};
