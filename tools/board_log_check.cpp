#include "board_logs.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <functional>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);} }while(false)
static bool waitFor(const std::function<bool()>& check,int ms=8000){QElapsedTimer timer;timer.start();while(timer.elapsed()<ms){QCoreApplication::processEvents();if(check())return true;QThread::msleep(20);}return false;}
int main(int argc,char** argv){QCoreApplication app(argc,argv);const auto args=app.arguments();
    if(args.contains("--port")){
        const auto port=args.value(args.indexOf("--port")+1);QTemporaryDir directory;CHECK(directory.isValid());BoardLogs logs(directory.path());logs.start(port);
        CHECK(waitFor([&]{return logs.status().value("state").toString()=="running";}));
        CHECK(waitFor([&]{return logs.status().value("count").toInt()>=3;},20000));
        const auto live=logs.status();CHECK(!logs.reply("all").contains("尚无缓存"));logs.stop();CHECK(waitFor([&]{return logs.status().value("state").toString()=="stopped";}));
        logs.start(port);CHECK(waitFor([&]{return logs.status().value("state").toString()=="running";}));logs.stop();
        std::printf("PASS USB actual capture, query cache, stop/reopen: rows=%d sequence=%lld\n",live.value("count").toInt(),static_cast<long long>(live.value("sequence").toInteger()));return 0;
    }
    BoardLogBuffer buffer;const auto input=QString("\x1b[31mE (42) camera: 中文错误\x1b[0m\r\n[producer] frames=100 ok=99\n[wire] len=2 hex=ffaa\nW (55) wifi: password=do-not-leak\n").toUtf8();
    for(char ch:input)buffer.feed(QByteArray(1,ch));const auto rows=buffer.rows();CHECK(rows.size()==4);CHECK(rows[0].toObject().value("text").toString()=="中文错误");CHECK(rows[0].toObject().value("level").toString()=="E");CHECK(rows[1].toObject().value("module").toString()=="producer");CHECK(rows[2].toObject().value("wire").toBool());CHECK(!QJsonDocument(rows).toJson().contains("do-not-leak"));
    for(const auto& raw:QStringList{"Authorization: Bearer abc-def","{\"api_key\":\"hide-this\"}","chat_text=private-chat","SSID: office-network"}){const auto safe=BoardLogBuffer::sanitize(raw);CHECK(safe.contains("[masked]"));}
    CHECK(BoardLogBuffer::utf8Limit("汉字",4)=="汉");CHECK(BoardLogBuffer::utf8Limit("🙂🙂",6)=="🙂");
    buffer.feed("I (44) camera: "+QString(5000,QChar(0x4e2d)).toUtf8()+"\nI (45) terminal: recovery\n");CHECK(buffer.counts().value("truncated").toInteger()==1);CHECK(buffer.rows().last().toObject().value("text").toString()=="recovery");
    for(int i=0;i<3000;++i)buffer.feed("[producer] "+QString(2000,QChar('x')).toUtf8()+"\n");CHECK(buffer.rows().size()<2000);CHECK(buffer.counts().value("bytes").toInteger()<=4*1024*1024);CHECK(buffer.counts().value("evicted").toInteger()>0);
    BoardLogBuffer restored;restored.restore(buffer.rows());CHECK(restored.rows()==buffer.rows());
    QTemporaryDir directory;CHECK(directory.isValid());
    {BoardLogs logs(directory.path());CHECK(waitFor([&]{return logs.status().value("state").toString()=="stopped";}));logs.start("COM_NOT_PRESENT");CHECK(waitFor([&]{return logs.status().value("state").toString()=="unavailable";}));CHECK(logs.reply("all").contains("不能据此判断设备正常"));CHECK(logs.reply("help").contains("/日志 错误"));logs.stop();CHECK(waitFor([&]{return logs.status().value("state").toString()=="stopped";}));}
    std::puts("PASS logs: split UTF-8/ANSI, ESP_LOG/printf, masking, hex, overlong recovery, cache bounds, restore, unavailable/stop/help");
}
