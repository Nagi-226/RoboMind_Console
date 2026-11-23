// M3 入口：启动 TCP 接收端 GUI（默认端口 9000，argv[1] 可覆盖）
//
// headless 验证请用独立目标 robomind-headless（控制台子系统）——
// GUI 是 WIN32 子系统，stdout 默认不接父控制台，把 --headless 塞进 GUI
// 会重蹈 M2 的覆辙：输出静默丢失而退出码仍为 0。理由同 selfcheck 独立成目标。

#include <QApplication>
#include <QString>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QFileInfo>
#include <QDateTime>

#include "mainwindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationVersion("2026.11.23.2");
    QApplication::setApplicationName("RoboMind Console");

    quint16 port = 9000;
    bool bridge = false;
    QString statusFile;
    QString stopFile;
    QString logPort;
    int durationMs = 0;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--bridge") bridge = true;
        else if (arg == "--status-file" && i + 1 < argc) statusFile = QString::fromLocal8Bit(argv[++i]);
        else if (arg == "--stop-file" && i + 1 < argc) stopFile = QString::fromLocal8Bit(argv[++i]);
        else if (arg == "--log-port" && i + 1 < argc) logPort = QString::fromLocal8Bit(argv[++i]);
        else if (arg == "--duration-ms" && i + 1 < argc) durationMs = QString::fromLocal8Bit(argv[++i]).toInt();
        else if (arg == "--port" && i + 1 < argc) {
            bool ok = false;
            const uint value = QString::fromLocal8Bit(argv[++i]).toUInt(&ok);
            if (!ok || value < 1 || value > 65535) return 2;
            port = static_cast<quint16>(value);
        }
    }
    if (argc > 1) {
        bool ok = false;
        const uint parsed = QString::fromLocal8Bit(argv[1]).toUInt(&ok);
        if (ok && parsed > 0 && parsed <= 65535) {
            port = static_cast<quint16>(parsed);
        }
    }

    MainWindow window(port,nullptr,statusFile.isEmpty()?QString():QFileInfo(statusFile).absolutePath()+"/logs");
    if(!logPort.isEmpty())window.setLogCapture(true,logPort);
    if (!bridge) window.show();
    else app.setQuitOnLastWindowClosed(false);
    // 文件快照避免 WIN32 子系统 stdout 丢失；只暴露链路状态，不含凭据/私有载荷。
    QTimer statusTimer;
    int statusWriteFailures=0;
    auto updateStatus = [&] {
        if (!stopFile.isEmpty() && QFile::exists(stopFile)) {
            app.quit();
            return;
        }
        if (!statusFile.isEmpty()) {
            QFile logRequest(QFileInfo(statusFile).absolutePath()+"/logs.request");
            if(logRequest.exists()&&logRequest.size()<=512&&logRequest.open(QIODevice::ReadOnly)){
                const auto command=QJsonDocument::fromJson(logRequest.readAll()).object();logRequest.close();logRequest.remove();
                const qint64 age=QDateTime::currentMSecsSinceEpoch()-command.value("created_ms").toInteger();
                const QString action=command.value("action").toString(),port=command.value("port").toString();
                if(age>=0&&age<5000&&(action=="stop"||(action=="start"&&!port.isEmpty()&&port.size()<=64)))window.setLogCapture(action=="start",port);
            }
            QFile request(QFileInfo(statusFile).absolutePath() + "/navigation.request");
            if(request.exists()&&request.size()<=512&&request.open(QIODevice::ReadOnly)){
                const auto command=QJsonDocument::fromJson(request.readAll()).object();request.close();request.remove();
                const qint64 age=QDateTime::currentMSecsSinceEpoch()-command.value("created_ms").toInteger();
                if(age>=0&&age<5000)window.navigateBoard(command.value("id").toString(),command.value("action").toString());
            }
            QSaveFile output(statusFile);
            if (!output.open(QIODevice::WriteOnly)) { if(++statusWriteFailures>=40)app.exit(3); return; }
            output.write(QJsonDocument(window.bridgeStatus()).toJson(QJsonDocument::Compact));
            // Windows can briefly lock the previous snapshot while the client reads it.
            // Preserve that snapshot and retry next tick; persistent failures still exit.
            if (!output.commit()) { if(++statusWriteFailures>=40)app.exit(3); }
            else statusWriteFailures=0;
        }
    };
    if (bridge) {
        if (statusFile.isEmpty() || stopFile.isEmpty()) return 2;
        QObject::connect(&statusTimer, &QTimer::timeout, &app, updateStatus);
        statusTimer.start(250);
        QTimer::singleShot(0, &app, updateStatus);
    }
    if (durationMs > 0) QTimer::singleShot(durationMs, &app, &QApplication::quit);
    return app.exec();
}
