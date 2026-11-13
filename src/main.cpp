// M3 入口：启动 TCP 接收端（默认端口 9000，可用 argv[1] 覆盖）
//
// 解码与切帧逻辑分别在 selfcheck.cpp / framecheck.cpp / frame_stream.h，
// 本文件只做启动，不承载业务。

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
