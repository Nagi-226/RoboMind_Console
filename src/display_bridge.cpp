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
void DisplayBridge::attach(QTcpSocket* socket){board_=socket;ready_=false;navigation_=false;queries_=false;immersion_=pcm_=false;view_={};if(socket){connect(socket,&QTcpSocket::disconnected,this,[this,socket]{if(board_!=socket)return;ready_=false;navigation_=false;queries_=false;immersion_=pcm_=false;view_={};});projectState();}}
void DisplayBridge::setLanChatConnected(bool connected){if(lanchat_==connected)return;lanchat_=connected;projectState();}
void DisplayBridge::record(const QString& id,const QString& state,const QString& reason){
    for(int i=0;i<deliveries_.size();++i)if(deliveries_[i].toObject().value("id").toString()==id){deliveries_.removeAt(i);break;}
    deliveries_.prepend(QJsonObject{{"id",id},{"state",state},{"reason",reason},{"updated_ms",now()}});
    while(deliveries_.size()>16)deliveries_.removeLast();
}
void DisplayBridge::enqueue(const QJsonObject& message){
    if(message.value("content_type").toString("text")!="text" || message.contains("attachment")) return;
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
    const QStringList allowed={"home","inbox","ppm","link","device","detail","next","previous","read","scroll_down","scroll_up","lang_zh","lang_en","immersion","terminal","audio_test","stop_speech","volume_10","volume_20","volume_30","volume_40","volume_50"};
    return ready_&&navigation_&&idValid(id)&&allowed.contains(action)&&send("ui/navigation",QString("id=%1;action=%2").arg(id,action),5000);
}
bool DisplayBridge::sendMedia(const QString& topic,const QString& payload,int ttl){
    const QStringList allowed={"ui/assistant_state","ui/audio_start","ui/audio_chunk","ui/audio_cancel"};
    return ready_&&immersion_&&allowed.contains(topic)&&send(topic,payload,ttl);
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
    if(envelope.source!="robomind-s3"||envelope.target!="console")return;
    if(envelope.topic=="device/query_reply"){if(ready_&&queries_)emit statusReply(QString::fromStdString(envelope.payload));return;}
    const auto values=fields(QString::fromStdString(envelope.payload));
    if(envelope.topic=="ui/audio_status"){
        if(!ready_||!immersion_)return;QJsonObject object;for(auto it=values.begin();it!=values.end();++it){if(it.key()=="id"||it.key()=="error")object.insert(it.key(),it.value());else if(it.key()=="done"||it.key()=="rendered"||it.key()=="cancelled")object.insert(it.key(),it.value()=="1");else {bool valid=false;const qint64 n=it.value().toLongLong(&valid);if(valid)object.insert(it.key(),n);}}
        emit audioStatus(object);return;
    }
    if(envelope.topic=="ui/capabilities"){
        ready_=values.value("version")=="1"&&values.value("text_max").toInt()>=384;navigation_=values.value("navigation")=="1";queries_=values.value("query")=="1";immersion_=values.value("immersion")=="1";pcm_=values.value("pcm")=="16000";projectState();pump();return;
    }
    if(envelope.topic=="ui/view_state"){
        bool valid=false;const int page=values.value("page").toInt(&valid);
        if(ready_&&valid&&page>=0&&page<5)view_={{"page",page},{"detail",values.value("detail")=="1"},{"unread",values.value("unread").toInt()},{"count",values.value("count").toInt()},{"language",values.value("language")},{"control",values.value("control")},{"immersion",values.value("immersion")=="1"},{"volume",values.value("volume").toInt()}};
        return;
    }
    if(envelope.topic!="ui/message_ack")return;
    const QString id=values.value("id"),state=values.value("status");
    if(!pending_.contains(id)||(state!="accepted"&&state!="rendered"&&state!="rejected"))return;
    record(id,state,state=="rejected"?"板端收件箱已满或拒绝该消息":QString());
    if(state=="rendered"){++rendered_;pending_.remove(id);}
    else if(state=="rejected"){++rejected_;pending_.remove(id);}
}
bool DisplayBridge::requestStatus(const QString& id,const QString& kind){return ready_&&queries_&&idValid(id)&&(kind=="perception"||kind=="link"||kind=="device")&&send("device/query",QString("id=%1;kind=%2").arg(id,kind),5000);}
QJsonObject DisplayBridge::status()const{return {{"terminal_ready",ready_},{"navigation",navigation_},{"query_supported",queries_},{"immersion_supported",immersion_},{"pcm_supported",pcm_},{"view",view_},{"pending",pending_.size()},{"rendered",static_cast<qint64>(rendered_)},{"rejected",static_cast<qint64>(rejected_)},{"deliveries",deliveries_}};}
