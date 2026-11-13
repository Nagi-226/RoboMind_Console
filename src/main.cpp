// M3 入口：启动 TCP 接收端 GUI（默认端口 9000，argv[1] 可覆盖）
//
// headless 验证请用独立目标 robomind-headless（控制台子系统）——
// GUI 是 WIN32 子系统，stdout 默认不接父控制台，把 --headless 塞进 GUI
// 会重蹈 M2 的覆辙：输出静默丢失而退出码仍为 0。理由同 selfcheck 独立成目标。

#include <QApplication>
#include <QString>

#include "mainwindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("RoboMind Console");

    quint16 port = 9000;
    if (argc > 1) {
        bool ok = false;
        const uint parsed = QString::fromLocal8Bit(argv[1]).toUInt(&ok);
        if (ok && parsed > 0 && parsed <= 65535) {
            port = static_cast<quint16>(parsed);
        }
    }

    MainWindow window(port);
    window.show();
    return app.exec();
}
