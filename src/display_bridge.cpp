#include "display_bridge.h"
#include "interconnect/message_codec.hpp"
#include <QTcpSocket>
#include <QDateTime>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <QtEndian>
#include <QStringList>

namespace {
QString percent(const QString& text){return QString::fromLatin1(QUrl::toPercentEncoding(text));}
QHash<QString,QString> fields(const QString& text){
    QHash<QString,QString> result;
    for(const auto& part:text.split(';')){const int eq=part.indexOf('=');if(eq>0)result.insert(part.left(eq),QUrl::fromPercentEncoding(part.mid(eq+1).toLatin1()));}
    return result;
}
bool idValid(const QString& id){static const QRegularExpression expression("^[A-Za-z0-9_.-]{1,63}$");return expression.match(id).hasMatch();}
qint64 now(){return QDateTime::currentMSecsSinceEpoch();}
}
DisplayBridge::DisplayBridge(QObject* parent):QObject(parent){timer_.setInterval(250);connect(&timer_,&QTimer::timeout,this,&DisplayBridge::pump);timer_.start();}
void DisplayBridge::attach(QTcpSocket* socket){board_=socket;ready_=false;navigation_=false;view_={};if(socket){connect(socket,&QTcpSocket::disconnected,this,[this]{ready_=false;navigation_=false;view_={};});projectState();}}
void DisplayBridge::setLanChatConnected(bool connected){if(lanchat_==connected)return;lanchat_=connected;projectState();}
void DisplayBridge::record(const QString& id,const QString& state,const QString& reason){
    for(int i=0;i<deliveries_.size();++i)if(deliveries_[i].toObject().value("id").toString()==id){deliveries_.removeAt(i);break;}
    deliveries_.prepend(QJsonObject{{"id",id},{"state",state},{"reason",reason},{"updated_ms",now()}});
    while(deliveries_.size()>16)deliveries_.removeLast();
}
void DisplayBridge::enqueue(const QJsonObject& message){
    const QString id=message.value("msg_id").toVariant().toString();
    const QString text=message.value("msg").toString();
    if(!idValid(id))return;
    if(pending_.contains(id))return;
    for(const auto& item:deliveries_)if(item.toObject().value("id").toString()==id&&item.toObject().value("state").toString()=="rendered")return;
    if(text.isEmpty()||text.toUtf8().size()>384||pending_.size()>=8){++rejected_;record(id,"rejected",text.toUtf8().size()>384?"正文超过 384 字节":"板端待发送队列已满");return;}
    const int sender=message.value("fromId").toInt();if(sender<1)return;
    const int group=message.value("groupId").toInt();
    QString name=message.value("fromNick").toString();if(name.isEmpty())name=QString("User %1").arg(sender);
    if(name.toUtf8().size()>63)name=QString::number(sender);
    const qint64 timestamp=message.value("timestamp").toVariant().toLongLong();
    const QString time=QDateTime::fromMSecsSinceEpoch(timestamp>0?timestamp:now()).toString("HH:mm");
    const QString payload=QString("id=%1;sender=%2;origin=%3;time=%4;text=%5")
        .arg(percent(id),percent(name),percent(group>0?QString("Group %1").arg(group):QString("Direct")),percent(time),percent(text));
    pending_.insert(id,Pending{id,payload,now(),0});record(id,"queued");pump();
}
bool DisplayBridge::send(const QString& topic,const QString& payload,int ttl){
    if(!board_||board_->state()!=QTcpSocket::ConnectedState||board_->bytesToWrite()>16384)return false;
    vr::interconnect::MessageEnvelope envelope;
    envelope.channel=vr::interconnect::ChannelType::kEvent;envelope.qos=vr::interconnect::DeliveryQoS::kAtLeastOnce;
    envelope.sequence=++sequence_;envelope.timestamp_ms=static_cast<quint64>(now());envelope.ttl_ms=static_cast<quint32>(qMax(1,ttl));
    envelope.source="console";envelope.target="robomind-s3";envelope.topic=topic.toStdString();envelope.trace_id="display";envelope.payload=payload.toStdString();
    std::string encoded;
    if(vr::interconnect::MessageCodec::Encode(envelope,&encoded)!=vr::core::ErrorCode::kOk||encoded.size()>2048)return false;
    QByteArray frame(4,0);qToBigEndian<quint32>(static_cast<quint32>(encoded.size()),reinterpret_cast<uchar*>(frame.data()));frame.append(encoded.data(),static_cast<int>(encoded.size()));
    return board_->write(frame)==frame.size();
}
void DisplayBridge::projectState(){send("ui/project_state",QString("lanchat=%1").arg(lanchat_?1:0),30000);}
bool DisplayBridge::navigate(const QString& id,const QString& action){
    const QStringList allowed={"home","inbox","ppm","link","device","detail","next","previous","read","scroll_down","scroll_up","lang_zh","lang_en"};
    return ready_&&navigation_&&idValid(id)&&allowed.contains(action)&&send("ui/navigation",QString("id=%1;action=%2").arg(id,action),5000);
}
void DisplayBridge::pump(){
    const qint64 timestamp=now();
    for(auto it=pending_.begin();it!=pending_.end();){
        const qint64 age=timestamp-it->created;
        if(age>=30000){record(it.key(),"expired","板端显示回执超时，请重发");it=pending_.erase(it);continue;}
        if(ready_&&timestamp-it->lastSent>=1000&&send("ui/message",it->payload,static_cast<int>(30000-age)))it->lastSent=timestamp;
        ++it;
    }
}
void DisplayBridge::receive(const vr::interconnect::MessageEnvelope& envelope){
    const auto values=fields(QString::fromStdString(envelope.payload));
    if(envelope.topic=="ui/capabilities"){
        ready_=values.value("version")=="1"&&values.value("text_max").toInt()>=384;navigation_=values.value("navigation")=="1";projectState();pump();return;
    }
    if(envelope.topic=="ui/view_state"){
        bool valid=false;const int page=values.value("page").toInt(&valid);
        if(ready_&&valid&&page>=0&&page<5)view_={{"page",page},{"detail",values.value("detail")=="1"},{"unread",values.value("unread").toInt()},{"count",values.value("count").toInt()},{"language",values.value("language")},{"control",values.value("control")}};
        return;
    }
    if(envelope.topic!="ui/message_ack")return;
    const QString id=values.value("id"),state=values.value("status");
    if(!pending_.contains(id)||(state!="accepted"&&state!="rendered"&&state!="rejected"))return;
    record(id,state,state=="rejected"?"板端收件箱已满或拒绝该消息":QString());
    if(state=="rendered"){++rendered_;pending_.remove(id);}
    else if(state=="rejected"){++rejected_;pending_.remove(id);}
}
QJsonObject DisplayBridge::status()const{return {{"terminal_ready",ready_},{"navigation",navigation_},{"view",view_},{"pending",pending_.size()},{"rendered",static_cast<qint64>(rendered_)},{"rejected",static_cast<qint64>(rejected_)},{"deliveries",deliveries_}};}
