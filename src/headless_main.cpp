// robomind-headless 入口：无窗口接收端，供机械验证
//
// 用法：
//   robomind-headless [--port N] [--duration S] [--expect N] [--quiet]

#include <QCoreApplication>
#include <QString>

#include <cstdio>
#include <cstring>

#include "headless.h"

int main(int argc, char** argv)
{
    // QCoreApplication 必须最先构造（Qt 的要求），故先扫描参数再建
    QCoreApplication app(argc, argv);

    HeadlessOptions opt;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](unsigned* out) {
            if (i + 1 < argc) { *out = static_cast<unsigned>(std::atoi(argv[++i])); return true; }
            return false;
        };
        if (std::strcmp(a, "--port") == 0) {
            unsigned v = 0;
            if (next(&v) && v > 0 && v <= 65535) opt.port = static_cast<quint16>(v);
        } else if (std::strcmp(a, "--duration") == 0) {
            unsigned v = 0;
            if (next(&v)) opt.duration_s = static_cast<int>(v);
        } else if (std::strcmp(a, "--expect") == 0) {
            unsigned v = 0;
            if (next(&v)) opt.expect = static_cast<int>(v);
        } else if (std::strcmp(a, "--quiet") == 0) {
            opt.quiet = true;
        } else if (std::strcmp(a, "--help") == 0) {
            std::printf("用法: %s [--port N] [--duration S] [--expect N] [--quiet]\n", argv[0]);
            return 0;
        }
    }
    return RunHeadless(opt);
}
