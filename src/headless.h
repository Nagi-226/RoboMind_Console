// headless.h —— 无窗口接收端（让 M3 可被机械验证）
//
// 存在理由：GUI 的输出只能人眼看，而人眼验证在自动化流程里等于零覆盖。
// 这个模式把"收到什么"打成可断言的行 + 一行 summary，并带退出码。

#ifndef HEADLESS_H
#define HEADLESS_H

#include <QtGlobal>

struct HeadlessOptions {
    quint16 port{9000};
    int     duration_s{0};   // >0：跑满 N 秒自动退出；0：一直跑（Ctrl+C）
    int     expect{-1};      // >=0：期望精确帧数（保留，但帧数随窗口抖动，不建议 CI 用）
    double  min_fps{-1.0};   // >=0：帧率下限（推荐判据，与采集时长解耦）
    double  max_fps{-1.0};   // >=0：帧率上限
    bool    quiet{false};    // true：只打 summary，不打每帧
};

// 返回进程退出码（0 = 成功）
int RunHeadless(const HeadlessOptions& opt);

#endif  // HEADLESS_H
