#include "status_query.h"
#include "display_bridge.h"
#include "lanchat_sink.h"
#include <QDateTime>
#include <QRegularExpression>
#include <QHostAddress>
#include <QUuid>
#include <limits>

namespace {
qint64 now(){return QDateTime::currentMSecsSinceEpoch();}
QString title(const QString& kind){return kind.startsWith("logs")?"日志":kind=="perception"?"感知":kind=="link"?"链路":kind=="device"?"设备":"帮助";}
bool identifier(const QString& value){static const QRegularExpression expression("^[A-Za-z0-9_.-]{1,63}$");return expression.match(value).hasMatch();}
QString recognize(QString text,bool group){
    text=text.normalized(QString::NormalizationForm_KC).trimmed().toLower();
    // 日志内容较多，只接受明确命令，私聊中的普通“日志”也不触发。
    const QHash<QString,QString> logs={{"/日志","logs"},{"/日志 错误","logs_error"},{"/日志 链路","logs_link"},{"/日志 帮助","logs_help"}};
    if(logs.contains(text))return logs.value(text);
    if(group&&!text.startsWith('/'))return {};
    if(text.startsWith('/'))text.remove(0,1);
    const QHash<QString,QString> aliases={{"感知","perception"},{"感知状态","perception"},{"perception","perception"},
        {"链路","link"},{"链路状态","link"},{"网络","link"},{"连接","link"},{"link","link"},
        {"设备","device"},{"设备状态","device"},{"device","device"},{"帮助","help"},{"help","help"}};
    return aliases.value(text);
}
void prune(QHash<QString,qint64>& map,int limit,qint64 oldest){
    for(auto it=map.begin();it!=map.end();)if(it.value()<oldest)it=map.erase(it);else ++it;
    while(map.size()>=limit){auto first=map.begin();for(auto it=map.begin();it!=map.end();++it)if(it.value()<first.value())first=it;map.erase(first);}
}
}
StatusQuery::StatusQuery(LanChatSink* sink,DisplayBridge* display,std::function<QJsonObject()> state,QObject* parent)
    :QObject(parent),sink_(sink),display_(display),state_(std::move(state)){
    connect(display_,&DisplayBridge::statusReply,this,[this](const QString& payload){receive(payload);});
    timer_.setInterval(100);connect(&timer_,&QTimer::timeout,this,[this]{tick();});timer_.start();
}
QString StatusQuery::linkSummary()const{
    const auto state=state_();
    return QString("S3 ↔ Console：%1\nConsole ↔ LanChat：%2\n已接收感知帧：%3；解码失败：%4")
        .arg(state.value("board_connected").toBool()?"已连接":"离线",state.value("lanchat_state").toString())
        .arg(state.value("frames").toInteger()).arg(state.value("decode_failures").toInteger());
}
void StatusQuery::reply(const QJsonObject& original,const QString& kind,const QString& body){
    const QString text=QString("[S3 · %1 · #%2]\n%3").arg(title(kind),original.value("msg_id").toVariant().toString(),body);
    if(sink_->sendReply(original,text))++replied_;
}
void StatusQuery::consume(const QJsonObject& message){
    const int type=message.value("type").toInt(),sender=message.value("fromId").toInt();
    if((type!=6&&type!=17)||sender<1||message.value("content_type").toString("text")!="text"||message.contains("attachment"))return;
    const QString id=message.value("msg_id").toVariant().toString();if(!identifier(id))return;
    const QString kind=recognize(message.value("msg").toString(),type==17);if(kind.isEmpty())return;
    const QString key=QString("%1:%2:%3:%4").arg(type).arg(sender).arg(message.value("groupId").toInt()).arg(id);
    if(processed_.contains(key))return;
    const auto timestamp=now();prune(processed_,256,timestamp-600000);processed_.insert(key,timestamp);
    QJsonObject original{{"type",type},{"fromId",sender},{"toId",message.value("toId")},{"groupId",message.value("groupId")},{"msg_id",id}};
    const QString user=QString::number(sender);prune(users_,128,timestamp-120000);prune(throttled_,128,timestamp-120000);
    if(users_.contains(user)&&timestamp-users_.value(user)<750){
        if(timestamp-throttled_.value(user)>750){reply(original,kind,"查询过于频繁，请稍后重试。");throttled_.insert(user,timestamp);}
        return;
    }
    users_.insert(user,timestamp);++requested_;
    if(kind.startsWith("logs")){
        const QString filter=kind=="logs_error"?"error":kind=="logs_link"?"link":kind=="logs_help"?"help":"all";
        reply(original,kind,logReply_?logReply_(filter):"日志采集未启用，尚无可用缓存。");return;
    }
    if(kind=="help"){
        reply(original,kind,"私聊：感知 / 链路 / 设备 / 帮助\n群聊：/感知 /链路 /设备 /帮助\n日志：/日志、/日志 错误、/日志 链路、/日志 帮助\n支持有限同义词，如感知状态、网络、设备状态。\n只读取真实状态；当前不支持自由对话或控制设备。\n原查询文字的板端显示回执与本查询回复分别记录。");return;
    }
    const auto state=state_();
    if(!state.value("board_connected").toBool()){reply(original,kind,(kind=="link"?linkSummary()+"\n":"")+"板端离线，当前快照不可用。");return;}
    if(pending_.size()>=4){reply(original,kind,"查询繁忙，请稍后重试。");return;}
    const QString token=QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending_.insert(token,Pending{original,kind,timestamp,0});tick();
}
void StatusQuery::tick(){
    const auto timestamp=now();const auto state=state_();
    for(auto it=pending_.begin();it!=pending_.end();){
        QString error;
        if(!state.value("board_connected").toBool())error="板端离线，查询中断。";
        else if(timestamp-it->created>=5000){error="查询超时，未收到当前快照。";++timeouts_;}
        else if(display_->status().value("terminal_ready").toBool()&&!display_->status().value("query_supported").toBool())error="当前板端固件不支持状态查询，请更新固件。";
        if(!error.isEmpty()){reply(it->original,it->kind,(it->kind=="link"?linkSummary()+"\n":"")+error);it=pending_.erase(it);continue;}
        if(timestamp-it->sent>=1000&&display_->requestStatus(it.key(),it->kind))it->sent=timestamp;
        ++it;
    }
}
void StatusQuery::receive(const QString& payload){
    if(payload.toUtf8().size()>512){++invalid_;return;}
    QHash<QString,QString> values;
    for(const auto& part:payload.split(';')){
        const int split=part.indexOf('=');
        if(split<1||values.contains(part.left(split))){++invalid_;return;}
        values.insert(part.left(split),part.mid(split+1));
    }
    const auto it=pending_.find(values.value("id"));
    if(it==pending_.end())return;
    if(values.value("version")!="1"||values.value("kind")!=it->kind){++invalid_;return;}
    if(now()-it->created>=5000){tick();return;}
    const QString body=format(it->kind,values);
    if(body.isEmpty()){++invalid_;return;}
    reply(it->original,it->kind,body);pending_.erase(it);
}
QString StatusQuery::format(const QString& kind,const QHash<QString,QString>& v)const{
    bool valid=true;
    const auto number=[&](const QString& key,qulonglong maximum=std::numeric_limits<qulonglong>::max()){
        static const QRegularExpression digits("^[0-9]{1,20}$");bool parsed=false;
        const auto n=v.value(key).toULongLong(&parsed);if(!parsed||!digits.match(v.value(key)).hasMatch()||n>maximum)valid=false;return n;
    };
    const auto up=number("up");
    if(kind=="perception"){
        const auto sample=number("sample",1);if(!valid)return {};
        if(!sample)return "当前无可用感知样本，未计算；不能据此判断正常或疲劳。";
        const auto sampled=number("sample_ms");if(!valid||sampled>up)return {};
        const auto age=up-sampled;
        if(age>3000)return QString("感知样本已过期（%1 秒前），不用于判断当前状态。").arg(age/1000.0,0,'f',1);
        const auto state=[&](const QString& validity,const QString& value,const QStringList& labels){
            const QString flag=v.value(validity),data=v.value(value);
            if(flag=="N"||flag=="I"){if(data!="-")valid=false;return flag=="N"?QString("未计算"):QString("无效 / 结果不可信");}
            if(flag!="O"){valid=false;return QString();}
            const auto n=number(value,static_cast<qulonglong>(labels.size()-1));return n<static_cast<qulonglong>(labels.size())?labels[static_cast<int>(n)]:QString();
        };
        const auto fatigue=state("dv","dl",{"正常","轻度疲劳","中度疲劳","重度疲劳"});
        const auto eyes=state("ev","es",{"睁眼","闭眼","不确定"});
        const auto mouth=state("mv","ms",{"闭合","张开"});
        const auto absent=number("ab",65535),degrade=number("dg",255);if(!valid)return {};
        return QString("疲劳：%1\n眼睛：%2\n嘴部：%3\n离开计时字段：%4 秒\n退化标志：0x%5\n样本年龄：%6 秒；板端采样时刻：开机后 %7 毫秒\n未计算或无效项不代表正常。").arg(fatigue,eyes,mouth).arg(absent/10.0,0,'f',1).arg(degrade,2,16,QChar('0')).arg(age/1000.0,0,'f',2).arg(sampled);
    }
    if(kind=="link"){
        const auto wifi=number("wifi",1),tcp=number("tcp",1),tx=number("tx",0xffffffff),rx=number("rx",0xffffffff),age=number("last_rx_age",0xffffffff);
        bool parsed=false;const int rssi=v.value("rssi").toInt(&parsed);
        static const QRegularExpression signedNumber("^-?[0-9]{1,3}$");
        if(!parsed||!signedNumber.match(v.value("rssi")).hasMatch()||rssi<-127||rssi>0)valid=false;
        const QString ip=v.value("ip");if(ip!="unknown"&&QHostAddress(ip).protocol()!=QAbstractSocket::IPv4Protocol)valid=false;
        if(!valid)return {};
        return QString("%1\nWi-Fi：%2；IP：%3；信号：%4\n板端 VVOS TCP：%5\n板端 TX %6 / RX %7；最近接收 %8 秒前\n板端运行：%9 秒").arg(linkSummary(),wifi?"已连接":"断开",ip,rssi?QString::number(rssi)+" dBm":"未知",tcp?"已连接":"断开").arg(tx).arg(rx).arg(age/1000.0,0,'f',2).arg(up/1000.0,0,'f',1);
    }
    const auto heap=number("heap",10000000),largest=number("largest",10000000),minimum=number("minimum",10000000),psram=number("psram",100000000),ppm=number("ppm",1),zao=number("zao",1);
    if(!valid||largest>heap||minimum>heap)return {};
    return QString("运行时间：%1 秒\n内部堆：%2 KiB；最大连续块：%3 KiB\n内部堆历史最低：%4 KiB\nPSRAM 可用：%5 MiB\n模块：LVGL / TCP；PPM %6；ZAO_MINI %7\n启用信息不等同于算法结果有效。").arg(up/1000.0,0,'f',1).arg(heap/1024.0,0,'f',1).arg(largest/1024.0,0,'f',1).arg(minimum/1024.0,0,'f',1).arg(psram/1048576.0,0,'f',2).arg(ppm?"已启用":"未启用",zao?"已接入":"未接入");
}
QJsonObject StatusQuery::status()const{return {{"pending",pending_.size()},{"requested",static_cast<qint64>(requested_)},{"replied",static_cast<qint64>(replied_)},{"timeouts",static_cast<qint64>(timeouts_)},{"invalid",static_cast<qint64>(invalid_)}};}
