// M1 · GUI 壳 + PPM 面板骨架
//
// 本文件的 PPM 面板由**假数据**驱动（Non-Goal：M3 之前不接网络）。
// 目的是把"字段卡片 / 状态色块 / 布局"先定型，M3 接通后只把数据源换掉。
//
// 解码逻辑不在这里 —— 见 selfcheck.cpp（独立控制台目标，可单独验证）。

#include <QApplication>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace {

// ---------------------------------------------------------------- 语义解析
// payload 形如：dv=O;dl=2;ev=O;es=1;mv=N;ms=-;ab=350;dg=1
QMap<QString, QString> ParsePayload(const QString& payload)
{
    QMap<QString, QString> kv;
    for (const QString& pair : payload.split(';', Qt::SkipEmptyParts)) {
        const int eq = pair.indexOf('=');
        if (eq > 0) {
            kv.insert(pair.left(eq).trimmed(), pair.mid(eq + 1).trimmed());
        }
    }
    return kv;
}

// 疲劳等级 → (文本, 颜色)。颜色口径与板端 UI 一致：无人=中性色，绝不红。
struct PerceptState {
    QString text;
    QString color;
};

PerceptState DeriveState(const QMap<QString, QString>& kv)
{
    const QString dv = kv.value("dv", "N");
    if (dv == "N") {
        return {"未计算 / 无人", "#A7B0C0"};   // 中性蓝灰：无人不是告警
    }
    if (dv == "I") {
        return {"结果不可信", "#6B7484"};
    }
    const QString dl = kv.value("dl", "0");
    if (dl == "0") return {"正常", "#35C46A"};
    if (dl == "1") return {"轻度疲劳", "#FFB020"};
    if (dl == "2") return {"中度疲劳", "#FF5C5C"};
    return {"重度疲劳", "#FF5C5C"};
}

// 一张字段卡片：键名（小字灰） + 值（大字等宽）
QWidget* MakeCard(const QString& key, const QString& value, const QString& hint)
{
    auto* card = new QFrame;
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet("QFrame { background: #1B1F2A; border: 1px solid #2E3444;"
                        " border-radius: 6px; }");

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(2);

    auto* key_label = new QLabel(key);
    key_label->setStyleSheet("color: #A7B0C0; border: none;");

    auto* value_label = new QLabel(value);
    QFont mono("Consolas");
    mono.setPointSize(12);
    mono.setBold(true);
    value_label->setFont(mono);
    value_label->setStyleSheet("color: #F2F4F8; border: none;");

    layout->addWidget(key_label);
    layout->addWidget(value_label);
    if (!hint.isEmpty()) {
        auto* hint_label = new QLabel(hint);
        hint_label->setStyleSheet("color: #6B7484; border: none;");
        layout->addWidget(hint_label);
    }
    return card;
}

QWidget* MakePpmPanel()
{
    // ⚠️ 假数据：取自规格 §3.8 向量 1。M3 之前不接板子。
    const QString fake_payload = "dv=O;dl=2;ev=O;es=1;mv=N;ms=-;ab=350;dg=1";
    const QMap<QString, QString> kv = ParsePayload(fake_payload);

    auto* panel = new QWidget;
    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // 顶部：状态色块（一眼可见，不靠读数字）
    const PerceptState state = DeriveState(kv);
    auto* banner = new QFrame;
    banner->setStyleSheet(
        QString("QFrame { background: %1; border: none; border-radius: 6px; }").arg(state.color));
    auto* banner_layout = new QHBoxLayout(banner);
    banner_layout->setContentsMargins(12, 10, 12, 10);
    auto* banner_text = new QLabel("感知状态：" + state.text);
    QFont bold = banner_text->font();
    bold.setPointSize(bold.pointSize() + 2);
    bold.setBold(true);
    banner_text->setFont(bold);
    banner_text->setStyleSheet("color: #12141C; border: none;");
    banner_layout->addWidget(banner_text);
    banner_layout->addStretch(1);
    root->addWidget(banner);

    // 中部：8 个字段卡片（4 列 × 2 行）
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    const struct {
        const char* key;
        const char* title;
        const char* hint;
    } fields[] = {
        {"dv", "dl 有效性", "N/I/O"},
        {"dl", "疲劳等级", "0..3"},
        {"ev", "es 有效性", "N/I/O"},
        {"es", "睁闭眼", "0睁 1闭 2?"},
        {"mv", "ms 有效性", "N/I/O"},
        {"ms", "张嘴", "0闭 1张"},
        {"ab", "离开时长", "×100ms"},
        {"dg", "降级位图", "NO_FACE=1"},
    };
    int i = 0;
    for (const auto& f : fields) {
        const QString v = kv.value(f.key, "<缺失>");
        grid->addWidget(MakeCard(f.title, v, QString("%1 · %2").arg(f.key).arg(f.hint)),
                        i / 4, i % 4);
        ++i;
    }
    root->addLayout(grid);

    auto* note = new QLabel(
        "⚠️ 以上为**假数据**（规格 §3.8 向量 1）。M3 接通 TCP 后替换数据源，布局不变。");
    note->setWordWrap(true);
    note->setStyleSheet("color: #FFB020;");
    root->addWidget(note);
    root->addStretch(1);
    return panel;
}

QWidget* MakeRawPanel()
{
    auto* panel = new QFrame;
    panel->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* title = new QLabel("Raw");
    QFont f = title->font();
    f.setPointSize(f.pointSize() + 2);
    f.setBold(true);
    title->setFont(f);

    auto* hint = new QLabel(
        "M3 将在此显示原始帧：长度头、11 个字段原文、payload 键值对。\n"
        "用途：解码结果与预期不符时，定位到具体字段。");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #A7B0C0;");

    layout->addWidget(title);
    layout->addWidget(hint);
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
    window.resize(1100, 720);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(MakePpmPanel());
    splitter->addWidget(MakeRawPanel());
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    window.setCentralWidget(splitter);
    window.statusBar()->showMessage("M1+M2 · 未连接（PPM 面板为假数据，M3 接 TCP）");
    window.show();

    return app.exec();
}
