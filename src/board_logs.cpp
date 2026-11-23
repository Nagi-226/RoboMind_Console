#include "board_logs.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QUuid>

QString BoardLogBuffer::utf8Limit(const QString& text,int maximum) {
    const auto bytes=text.toUtf8();
    if(bytes.size()<=maximum)return text;
    int end=maximum;
    while(end>0&&(static_cast<unsigned char>(bytes[end])&0xc0)==0x80)--end;
    return QString::fromUtf8(bytes.constData(),end);
}
QString BoardLogBuffer::sanitize(QString text) {
    // 去掉终端颜色和控制序列；密码即使出现在未识别的 RAW 行也先遮住。
    static const QRegularExpression ansi("\\x1b(?:\\[[0-?]*[ -/]*[@-~]|\\][^\\x07]*(?:\\x07|\\x1b\\\\))");
    static const QRegularExpression controls("[\\x00-\\x08\\x0b-\\x1f\\x7f]");
    static const QRegularExpression secrets("(?i)([\\\"']?(?:password|passwd|pwd|secret|api[_-]?key|access[_-]?token|refresh[_-]?token|token|authorization|cookie|ssid|chat[_-]?(?:text|body)|msg(?:_text)?)[\\\"']?\\s*[:=]\\s*).*$");
    text.remove(ansi);text.remove(controls);
    text.replace(secrets,"\\1[masked]");
    text.replace(QRegularExpression("(?i)Bearer\\s+[^\\s]+"),"Bearer [masked]");
    return text.trimmed();
}
void BoardLogBuffer::append(QJsonObject row) {
    const qint64 size=QJsonDocument(row).toJson(QJsonDocument::Compact).size();
    rows_.append(row);bytes_+=size;
    while(rows_.size()>2000||bytes_>4*1024*1024){bytes_-=QJsonDocument(rows_.first().toObject()).toJson(QJsonDocument::Compact).size();rows_.removeFirst();++evicted_;}
}
void BoardLogBuffer::line() {
    QString text=sanitize(QString::fromUtf8(partial_));partial_.clear();
    const bool cut=overflow_||text.toUtf8().size()>2048;overflow_=false;
    if(text.isEmpty())return;
    if(cut){text=utf8Limit(text,2000)+" [truncated]";++truncated_;}
    QString level="RAW",module="raw";
    static const QRegularExpression esp("^([IWEVD]) \\([0-9]+\\) ([A-Za-z0-9_.-]{1,64}):\\s*(.*)$");
    static const QRegularExpression print("^\\[([A-Za-z0-9_.-]{1,64})\\]\\s*(.*)$");
    const auto match=esp.match(text),raw=print.match(text);
    if(match.hasMatch()){level=match.captured(1);module=match.captured(2);text=match.captured(3);}
    else if(raw.hasMatch()){module=raw.captured(1);text=raw.captured(2);if(module!="wire")level="I";}
    const bool wire=module=="wire"||text.contains(QRegularExpression("(?i)\\bhex="));
    append({{"seq",++sequence_},{"received_ms",QDateTime::currentMSecsSinceEpoch()},{"level",level},{"module",module},{"text",text},{"wire",wire},{"truncated",cut},{"session",session_},{"port",port_}});
}
void BoardLogBuffer::feed(const QByteArray& bytes) {
    for(char ch:bytes){if(ch=='\n'){line();continue;}if(partial_.size()<8192)partial_.append(ch);else overflow_=true;}
}
void BoardLogBuffer::finish(){if(!partial_.isEmpty()||overflow_)line();}
void BoardLogBuffer::restore(const QJsonArray& rows){
    for(const auto& value:rows){auto row=value.toObject();if(row.value("seq").toInteger()<=sequence_)continue;
        row["text"]=utf8Limit(sanitize(row.value("text").toString()),2048);
        row["module"]=sanitize(row.value("module").toString()).left(64);
        sequence_=row.value("seq").toInteger();append(row);}
}
QJsonObject BoardLogBuffer::counts()const{return {{"count",rows_.size()},{"bytes",bytes_},{"sequence",sequence_},{"evicted",evicted_},{"truncated",truncated_},{"last_ms",rows_.isEmpty()?0:rows_.last().toObject().value("received_ms").toInteger()}};}

struct BoardLogs::Shared { mutable QMutex mutex; QJsonObject status; QJsonArray rows; };
class BoardLogs::Worker : public QObject {
public:
    Worker(QString directory,std::shared_ptr<Shared> shared):directory_(std::move(directory)),shared_(std::move(shared)){}
    void initialize(){
        QDir().mkpath(directory_);QFile old(directory_+"/logs.snapshot.json");
        if(old.size()<=12*1024*1024&&old.open(QIODevice::ReadOnly))buffer_.restore(QJsonDocument::fromJson(old.readAll()).object().value("rows").toArray());
        written_=buffer_.counts().value("sequence").toInteger();
        serial_=new QSerialPort(this);serial_->setReadBufferSize(65536);
        connect(serial_,&QSerialPort::readyRead,this,[this]{buffer_.feed(serial_->readAll());dirty_=true;});
        connect(serial_,&QSerialPort::errorOccurred,this,[this](QSerialPort::SerialPortError error){
            if(error==QSerialPort::NoError||closing_)return;
            state_=(error==QSerialPort::PermissionError||error==QSerialPort::OpenError)?"busy":"unavailable";
            closing_=true;serial_->close();closing_=false;buffer_.finish();dirty_=true;
        });
        timer_=new QTimer(this);timer_->setInterval(200);connect(timer_,&QTimer::timeout,this,[this]{tick();});timer_->start();tick();
    }
    void start(const QString& port){stop();port_=port;buffer_.setSource(session_,port_);enabled_=true;state_="starting";retry_=0;dirty_=true;tick();}
    void stop(){enabled_=false;closing_=true;if(serial_)serial_->close();closing_=false;buffer_.finish();state_="stopped";dirty_=true;if(serial_)publish();}
    void shutdown(){stop();if(timer_)timer_->stop();}
private:
    void tick(){
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(now-retry_>=2000){retry_=now;ports_={};for(const auto& info:QSerialPortInfo::availablePorts())ports_.append(QJsonObject{{"name",info.portName()},{"description",info.description()}});
            if(enabled_&&!serial_->isOpen()){
                bool present=false;for(const auto& value:ports_)if(value.toObject().value("name").toString()==port_)present=true;
                if(!present)state_="unavailable";
                else {serial_->setPortName(port_);serial_->setBaudRate(115200);serial_->setFlowControl(QSerialPort::NoFlowControl);if(serial_->open(QIODevice::ReadOnly))state_="running";}
            }dirty_=true;
        }
        if(dirty_)publish();
    }
    void history(const QJsonArray& rows){
        QFile file(directory_+"/board-log-0.jsonl");
        if(!rows.isEmpty()&&rows.first().toObject().value("seq").toInteger()>written_+1){historyLost_+=rows.first().toObject().value("seq").toInteger()-written_-1;written_=rows.first().toObject().value("seq").toInteger()-1;}
        for(const auto& value:rows){const auto row=value.toObject();if(row.value("seq").toInteger()<=written_)continue;
            const auto bytes=QJsonDocument(row).toJson(QJsonDocument::Compact)+'\n';
            if(!file.isOpen()&&!file.open(QIODevice::WriteOnly|QIODevice::Append)){++writeErrors_;return;}
            if(file.size()+bytes.size()>5*1024*1024){file.close();
                // 只轮换本模块固定命名的四份历史；不操作用户导出的文件。
                const QString oldest=directory_+"/board-log-3.jsonl";
                if(QFile::exists(oldest)&&!QFile::remove(oldest)){++writeErrors_;return;}
                for(int i=2;i>=0;--i){const QString source=directory_+QString("/board-log-%1.jsonl").arg(i),target=directory_+QString("/board-log-%1.jsonl").arg(i+1);
                    if(QFile::exists(source)&&!QFile::rename(source,target)){++writeErrors_;return;}}
                if(!file.open(QIODevice::WriteOnly|QIODevice::Append)){++writeErrors_;return;}
            }
            if(file.write(bytes)!=bytes.size()){++writeErrors_;return;}written_=row.value("seq").toInteger();
        }
    }
    void publish(){
        const auto rows=buffer_.rows();history(rows);
        auto status=buffer_.counts();status["state"]=state_;status["port"]=port_;status["enabled"]=enabled_;status["source"]="board_usb";status["ports"]=ports_;status["session"]=session_;status["write_errors"]=writeErrors_;status["history_lost"]=historyLost_;status["updated_ms"]=QDateTime::currentMSecsSinceEpoch();
        QSaveFile file(directory_+"/logs.snapshot.json");
        if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(QJsonObject{{"status",status},{"rows",rows}}).toJson(QJsonDocument::Compact))<0||!file.commit())++writeErrors_;
        {QMutexLocker lock(&shared_->mutex);shared_->status=status;shared_->rows=rows;}dirty_=false;
    }
    QString directory_,port_,state_="stopped",session_=QUuid::createUuid().toString(QUuid::Id128);
    std::shared_ptr<Shared> shared_;BoardLogBuffer buffer_;QSerialPort* serial_=nullptr;QTimer* timer_=nullptr;
    QJsonArray ports_;bool enabled_=false,closing_=false,dirty_=true;qint64 retry_=0,written_=0,writeErrors_=0,historyLost_=0;
};
BoardLogs::BoardLogs(const QString& directory,QObject* parent):QObject(parent),shared_(std::make_shared<Shared>()),worker_(new Worker(directory,shared_)){
    worker_->moveToThread(&thread_);connect(&thread_,&QThread::started,worker_,[this]{worker_->initialize();});connect(&thread_,&QThread::finished,worker_,&QObject::deleteLater);thread_.start();
}
BoardLogs::~BoardLogs(){QMetaObject::invokeMethod(worker_,[this]{worker_->shutdown();},Qt::BlockingQueuedConnection);thread_.quit();thread_.wait();}
void BoardLogs::start(const QString& port){QMetaObject::invokeMethod(worker_,[this,port]{worker_->start(port);});}
void BoardLogs::stop(){QMetaObject::invokeMethod(worker_,[this]{worker_->stop();});}
QJsonObject BoardLogs::status()const{QMutexLocker lock(&shared_->mutex);return shared_->status;}
QString BoardLogs::reply(const QString& filter)const{
    if(filter=="help")return "支持 /日志、/日志 错误、/日志 链路、/日志 帮助。\n读取主机 USB 已采集日志，最近 20 条，回复最多 6 KiB。\n采集前和断开期间的输出无法补取；不通过聊天控制采集。";
    QJsonObject state;QJsonArray rows;{QMutexLocker lock(&shared_->mutex);state=shared_->status;rows=shared_->rows;}
    QStringList lines;QString first,last;
    for(auto it=rows.end();it!=rows.begin()&&lines.size()<20;){const auto row=(--it)->toObject();
        if(row.value("wire").toBool()||(filter=="error"&&row.value("level").toString()!="E")||(filter=="link"&&!row.value("module").toString().contains("wifi",Qt::CaseInsensitive)&&row.value("module").toString()!="terminal_tcp"))continue;
        const auto time=QDateTime::fromMSecsSinceEpoch(row.value("received_ms").toInteger()).toString("MM-dd HH:mm:ss.zzz");
        if(last.isEmpty())last=time;first=time;lines.prepend(QString("%1 %2 %3 %4: %5").arg(time,row.value("port").toString(),row.value("level").toString(),row.value("module").toString(),row.value("text").toString()));
    }
    const auto lastMs=state.value("last_ms").toInteger();
    QString body=QString("来源：板端 USB / 主机采集 %1\n采集状态：%2\n最近采集：%3\n返回 %4 条（最多 20 条）%5\n淘汰 %6 条；截断 %7 条\n\n")
        .arg(state.value("port").toString(),state.value("state").toString()=="running"?"采集中":"采集不可用，仅历史缓存",lastMs>0?QDateTime::fromMSecsSinceEpoch(lastMs).toString("MM-dd HH:mm:ss.zzz"):"尚无缓存")
        .arg(lines.size()).arg(lines.isEmpty()?QString():QString("；范围 %1 — %2").arg(first,last)).arg(state.value("evicted").toInteger()).arg(state.value("truncated").toInteger());
    body+=lines.isEmpty()?"此范围内没有已采集日志，不能据此判断设备正常。":lines.join('\n');
    if(body.toUtf8().size()>5600)body=BoardLogBuffer::utf8Limit(body,5530)+"\n[回复已截断，更多内容请在主机日志页查看]";
    return body;
}
