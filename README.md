# RoboMind Console

PC 端调试控制台：接收 RoboMind-S3 板子经 WiFi 推送的 PPM 感知数据并显示。

**当前进度：M1（壳）+ M2（解码）+ M3（TCP 接入 + 实时面板）已完成并端到端验收。**
验收数据：`fps=9.47`、`decode_fail=0`、`partial=0`，断连后 **3.1 秒自动重连**。

---

## 这是什么

板端 `ppm_producer` 把感知结果编码成 **11 字段文本信封**（`[u32 大端长度] + 文本`），
经 TCP 推给本程序。本程序负责解码并显示。

- **不连板子也能验证**：M2 的解码逻辑与 GUI 无关，用 `selfcheck` 单独跑。
  这样"解码对不对"与"链路通不通"是两个独立问题，出故障能立刻定位是哪一段。

---

## 与 RoboMind-S3 的关系

本程序是 **[RoboMind-S3](https://github.com/Nagi-226/RoboMind-S3)** 的配套上位机。

两者是**数据生产方 ↔ 消费方**关系：板子采集与决策，PC 显示。**协议契约是唯一的耦合点** —— 没有共享代码，没有共享构建。

### 数据流

```
RoboMind-S3（ESP32-S3）                        RoboMind-Console（PC）
┌──────────────────────────┐                  ┌────────────────────────┐
│ 摄像头                    │                  │                        │
│   ↓                      │                  │  QTcpServer :9000      │
│ ppm_producer             │                  │        ↓               │
│   ↓ ppm_run() ~105ms/帧  │                  │  frame_stream          │
│ ppm_semantics_t          │                  │  （流式切帧，处理半包）  │
│   ↓                      │                  │        ↓               │
│ wire_codec               │   TCP 9000       │  MessageCodec::Decode  │
│ 11 字段文本信封 ──────────┼─────────────────►│        ↓               │
│ [u32 BE 长度] + 文本      │                  │  PpmPanel 实时显示      │
└──────────────────────────┘                  └────────────────────────┘
```

### ⚠️ 协议契约的真相源

**板端 `wire_codec.cpp` 是本仓库所用 `MessageCodec`（在 VVOS 里）的独立实现，不是共享代码。**

| 想改什么 | 真相源（改这里） | 必须同步 |
|---|---|---|
| **帧格式 / 字段数** | **VVOS 的 `MessageCodec` + wire 规格** | 板端 `wire_codec.cpp` + 本仓库 `testdata/` 基准包 + `framecheck` |
| 新增 topic | 板端 `wire_codec.cpp` | **本仓库无需改**（未知 topic 落 `RawPanel`） |
| 新增 payload 字段 | PPM profile 的 `output.fields` | 板端 payload 语法 + 本仓库解析 |

**「副本永远不是真相源」**：改 `wire_codec.cpp` 之前**必须先看 `message_codec.cpp`**，只看规格不够 —— 规格可能滞后于实现（本项目就发生过：规格写 8 字段、实现已支持 11）。

改完必须用 `framecheck` 跑 `testdata/frames_buggy.bin`（应 FAIL）和 `testdata/frames_fixed.bin`（应 PASS），证明检查没被削弱。

### 版本对应

| 本仓库 | 对应板端 | 协议 |
|---|---|---|
| `master` @ `c67941a` | RoboMind-S3 `master` @ `98474f5` | 11 字段信封，TCP 9000 |

### 联调（三步，含两个必踩的坑）

**① 板端开启上报**

```
CONFIG_ROBOMIND_PPM_PRODUCER_ENABLED=y
CONFIG_ROBOMIND_TCP_CLIENT_ENABLED=y
CONFIG_ROBOMIND_TCP_CLIENT_HOST="<PC 的 IP>"
```

**② PC 端放行防火墙** ⚠️ **坑一**

```powershell
New-NetFirewallRule -DisplayName "RoboMind Console 9000" -Direction Inbound -LocalPort 9000 -Protocol TCP -Action Allow
```

**不加这条板子连不上**，而现象是"静默无数据" —— 极易误判成代码问题。**先查防火墙，别先怀疑代码。**

> 验证方法：`Test-NetConnection <PC_IP> -Port 9000`，或从**另一台机器**连（**本机 loopback 测不通不代表局域网不通，反之亦然**）。

**③ 起 server，再上电板子**（顺序无所谓，板子有指数退避会自己连回来）

⚠️ **坑二**：用 `Start-Process` 起的 server 会被 PowerShell 会话回收 —— **必须在同一次调用里 `WaitForExit`**，否则跑两分钟就静默消失，且**没有 summary**。

---

## 构建

依赖：

| 项 | 版本 / 路径 |
|---|---|
| Qt | `D:/Qt/6.10.3/mingw_64` |
| 编译器 | `D:/Qt/Tools/mingw1310_64`（GCC 13.1.0） |
| CMake | 3.16+ |
| VVOS | `E:/Github Project/Virtual-Vehicle-OS`（**基线 commit `13c46b0`**） |

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="D:/Qt/Tools/mingw1310_64/bin/gcc.exe" \
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_PREFIX_PATH="D:/Qt/6.10.3/mingw_64"

cmake --build build
```

VVOS 路径不是本仓库的子目录，可用 `-DVVOS_ROOT=<path>` 覆盖。

### ⚠️ 关于 VVOS 依赖的可复现性

本工程用 `add_subdirectory()` 引用 **本地路径**，这是**路径依赖，不是版本约束**。
README 顶部记录的 `13c46b0` 是我们验证过的基线 —— 升级 VVOS 时必须同步改这一行，
否则会出现"同一份代码在别人机器上行为不同"。

（将来若要给第三方用，需 VVOS 补 `install(EXPORT ...)` + `VVOSConfig.cmake` 才能 `find_package`。
当前阶段不需要。）

---

## 运行

### ⚠️ 已知陷阱：双击"没反应"

若双击 exe 后 **无窗口、无报错、进程却存在一会儿就没了**，99% 是缺
`platforms/qwindows.dll`（Qt 平台插件）。

**这不是 UI 代码问题。** 它的恶劣之处在于：退出码是 0、没有错误对话框、
任务管理器里能看到进程 —— 一切"正常"，就是没窗口。

**诊断方法：查进程的 `MainWindowHandle`，不是先查 QML 代码。**

```powershell
$p = Start-Process .\RoboMindConsole.exe -PassThru
Start-Sleep 5; $p.Refresh()
"handle=$($p.MainWindowHandle)  title=[$($p.MainWindowTitle)]"
# handle=0 ⇒ 平台插件没加载（缺 qwindows.dll，或 PATH/部署目录不对）
```

### 方式一：从开发环境启动（需要 Qt 在 PATH）

```bash
export PATH="D:/Qt/6.10.3/mingw_64/bin:$PATH"
./build/RoboMindConsole.exe
```

### 方式二：分发版（不依赖 PATH）

```bash
./build/RoboMindConsole.exe          # 已由 `cmake --build build --target deploy` 部署
```

`build/dist/` 由 `windeployqt` 生成，自带 Qt DLL 与平台插件，**可在没装 Qt 的机器上双击运行**。

### 打分发版

```bash
cmake --build build --target deploy
```

`deploy` 是 CMake 自定义目标，内部调用 `windeployqt`（路径从 `Qt6::qmake` 反推，不写死），
把 Qt 运行时依赖复制到 exe 旁边。**部署完 `build/RoboMindConsole.exe` 即可在无 Qt 的机器上双击运行。**

> **为什么做成 CMake 目标而不是"记得手动跑"**：手动步骤会退化成"每次都要记得"，
> 而忘跑的症状是"双击没反应"，且和 UI 代码缺陷长得一模一样。做成目标后它跟着构建走。

<details>
<summary>手动方式（deploy 目标不可用时）</summary>

```bash
D:/Qt/6.10.3/mingw_64/bin/windeployqt.exe --release --no-translations \
  build/RoboMindConsole.exe
```

⚠️ 若用 `--dir <新目录>` 指定输出目录，`windeployqt` **只复制依赖、不复制 exe 本身**，
必须手动 `cp build/RoboMindConsole.exe <新目录>/`。就地部署（如上）没有这个问题。
</details>

**验收分发版必须在"PATH 里没有 Qt"的环境里做** —— 在装了 Qt 的机器上测，等于没测。
最直接的做法是把 PATH 缩到最小再启动：

```powershell
$env:PATH = "C:\Windows\System32;C:\Windows"
Start-Process .\build\RoboMindConsole.exe -PassThru |
  ForEach-Object { Start-Sleep 5; $_.Refresh(); "handle=$($_.MainWindowHandle)" }
# handle 非 0 ⇒ 自包含成立
```

### 解码自检（无需板子）

```bash
./build/selfcheck.exe
echo $?          # 0 = 全部通过；非 0 = 有失败项
```

覆盖规格 `VVOS/docs/u02-u03-implementation-spec-2026-11-13.md` §3.8 的 6 组向量：

| 向量 | 内容 |
|---|---|
| 1 | 典型 11 字段：文本 112 B / 帧 116 B / 头 `00 00 00 70` |
| 2 | 降级+饱和：文本 108 B / 帧 112 B / 头 `00 00 00 6C` |
| 3 | 转义：`\|` 解码后还原为 `\|`（source 与 topic 各测一次） |
| 4 | 空字段：`topic` 与 `trace_id` 为空串，不得变成 `"null"` 或 `"\"\""` |
| 5 | 拒收：9 个负向用例，全部应为 `kInvalidParam` |
| 6 | 粘包/半包：向量 1 的帧拆 3 段喂入，结果与一次喂入逐字段相同 |

### GUI 壳

```bash
./build/RoboMindConsole.exe
```

双击运行**不会弹控制台窗口**（PE 子系统 = Windows GUI）。
这是 `WIN32_EXECUTABLE ON` 的效果，可用下面命令机械验证：

```bash
objdump -p build/RoboMindConsole.exe | grep -i ^Subsystem
# 期望：Subsystem  00000002  (Windows GUI)
```

---

## 为什么 selfcheck 是独立可执行文件

GUI 是 `WIN32` 子系统，stdout 默认不接父控制台。若把 `--selfcheck` 塞进 GUI 程序，
要么靠 `AttachConsole` + `freopen` 折腾（且在某些 shell/重定向下失效），要么输出丢失。

分成两个目标后：`selfcheck.exe` 是控制台程序，输出与退出码都是天然的；
`RoboMindConsole.exe` 保持 GUI。两条判据互不干扰。

---

## 目录结构

```
.
├── CMakeLists.txt
├── src/
│   ├── main.cpp        # M1：Qt 主窗口 + PPM / Raw 两个占位面板
│   └── selfcheck.cpp   # M2：6 组向量 + 帧解析器（粘包/半包）
└── README.md
```

---

## 协议变更流程

协议真相在 VVOS（`docs/` 有 wire 规格、`src/interconnect/message_codec.*` 是实现）。
正确顺序：

```
VVOS 改 codec  →  本仓库升级 VVOS_ROOT 指向的 commit  →  固件（RoboMind-S3）跟改编码器
```

**不要反过来**（先改固件再改 codec），否则解码端与编码端会漂移，而这种漂移在
"两边各自单测都通过"时是看不出来的。

---

## Non-Goals（本里程碑明确不做）

- ❌ 不连板子、不做 TCP、不做网络
- ❌ PPM / Raw 面板只有占位，不渲染实际数据
- ❌ 不处理粘包之外的协议边界（超长帧、非法 UTF-8、长度头异常）
- ❌ 不做亮/暗主题、不做国际化
- ❌ 不做日志落盘（selfcheck 只打 stdout）
- ❌ 不安装/不打包（无 windeployqt 步骤）
