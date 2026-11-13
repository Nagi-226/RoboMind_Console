// M1 · GUI 壳：主窗口 + 两个面板占位（PPM / Raw）
//
// 本文件只做"壳"：不连板子、不做网络、面板内不放实际控件。
// 解码逻辑在 selfcheck.cpp（独立控制台程序），不在此处。

#include <QApplication>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace {

// 一个占位面板：标题 + 说明文字。真正的内容留给后续里程碑。
QWidget* MakePlaceholder(const QString& title, const QString& hint)
{
    auto* panel = new QFrame;
    panel->setFrameShape(QFrame::StyledPanel);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* title_label = new QLabel(title);
    QFont f = title_label->font();
    f.setPointSize(f.pointSize() + 2);
    f.setBold(true);
    title_label->setFont(f);

    auto* hint_label = new QLabel(hint);
    hint_label->setWordWrap(true);
    hint_label->setStyleSheet("color: #666;");

    layout->addWidget(title_label);
    layout->addWidget(hint_label);
    layout->addStretch(1);
    return panel;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("RoboMind Console");

    QMainWindow window;
    window.setWindowTitle("RoboMind Console");
    window.resize(1000, 680);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(MakePlaceholder(
        "PPM",
        "M3 将在此显示感知状态：疲劳等级、睁闭眼、在场时长、降级位。\n"
        "数据来源：板端 producer 经 TCP 推送的 11 字段信封。"));
    splitter->addWidget(MakePlaceholder(
        "Raw",
        "M3 将在此显示原始帧：长度头、11 个字段、payload 的键值对。\n"
        "用于排查「解码结果与预期不符」时定位到具体字段。"));
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    window.setCentralWidget(splitter);
    window.statusBar()->showMessage("M1 壳 · 未连接（M2 解码逻辑见 selfcheck）");
    window.show();

    return app.exec();
}
