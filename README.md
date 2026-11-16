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
$p = Start-Process .\build\dist\RoboMindConsole.exe -PassThru
Start-Sleep 5; $p.Refresh()
"handle=$($p.MainWindowHandle)  title=[$($p.MainWindowTitle)]"
# handle=0 ⇒ 平台插件没加载（缺 qwindows.dll，或 PATH/部署目录不对）
```

### 方式一：从开发环境启动（需要 Qt 在 PATH）

```bash
export PATH="D:/Qt/6.10.3/mingw_64/bin:$PATH"
./build/dist/RoboMindConsole.exe
```

### 方式二：分发版（不依赖 PATH）

```bash
./build/dist/RoboMindConsole.exe          # 已由 `cmake --build build --target deploy` 部署
```

`deploy` 是**就地部署**：Qt DLL 与平台插件直接落在 exe 旁边（`build/dist/`），所以
`build/dist/RoboMindConsole.exe` 整个目录拷走就能跑，**可在没装 Qt 的机器上双击运行**。

> **为什么 GUI 产物在 `dist/` 而不是 `build/` 根下**：`CMakeLists.txt` 给
> `robomind-console` 钉了 `RUNTIME_OUTPUT_DIRECTORY = build/dist`。
> 曾经 exe 在 `build/` 根、分发目录又是 `build/dist/`，于是同时存在两份：
> 一份带动新代码、一份是上次手动拷过去的旧版，**双击到哪份全看运气**，
> 症状是"明明改了却看不到效果"（本项目实际踩过：dist 里那份落后一整个里程碑）。
> 三个控制台程序（`selfcheck` / `framecheck` / `robomind-headless`）仍在 `build/` 根下。

### 打分发版

```bash
cmake --build build --target deploy
```

`deploy` 是 CMake 自定义目标，内部调用 `windeployqt`（路径从 `Qt6::qmake` 反推，不写死），
把 Qt 运行时依赖复制到 exe 旁边。**部署完 `build/dist/RoboMindConsole.exe` 即可在无 Qt 的机器上双击运行。**

> **为什么做成 CMake 目标而不是"记得手动跑"**：手动步骤会退化成"每次都要记得"，
> 而忘跑的症状是"双击没反应"，且和 UI 代码缺陷长得一模一样。做成目标后它跟着构建走。

<details>
<summary>手动方式（deploy 目标不可用时）</summary>

```bash
D:/Qt/6.10.3/mingw_64/bin/windeployqt.exe --release --no-translations \
  build/dist/RoboMindConsole.exe
```

⚠️ 若用 `--dir <新目录>` 指定输出目录，`windeployqt` **只复制依赖、不复制 exe 本身**，
必须手动 `cp build/dist/RoboMindConsole.exe <新目录>/`。就地部署（如上）没有这个问题。
</details>

**验收分发版必须在"PATH 里没有 Qt"的环境里做** —— 在装了 Qt 的机器上测，等于没测。
最直接的做法是把 PATH 缩到最小再启动：

```powershell
$env:PATH = "C:\Windows\System32;C:\Windows"
Start-Process .\build\dist\RoboMindConsole.exe -PassThru |
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
./build/dist/RoboMindConsole.exe
```

双击运行**不会弹控制台窗口**（PE 子系统 = Windows GUI）。
这是 `WIN32_EXECUTABLE ON` 的效果，可用下面命令机械验证：

```bash
objdump -p build/dist/RoboMindConsole.exe | grep -i ^Subsystem
# 期望：Subsystem  00000002  (Windows GUI)
```

---

## LanChat 告警转发（可选，默认关闭）

Console 作为桥，把 PPM 疲劳告警转发到 LanChat-Next 服务端（方案 C，板子零改动）：

```
板子 --VVOS envelope(TCP 9000)--> Console --[u32 BE len]+JSON--> LanChat 服务端(默认 9001)
```

帧格式与 VVOS wire 同构，只换 payload 为 JSON。第一版只实现三个消息类型：
`Login`(2) / `SendMsg`(5) / `Heartbeat`(20，10s 一次)；接收方向只判
`LoginSuccessReturn`(3) / `LoginFailedReturn`(4)，其余不解析。

### 配置

复制 `robomind-console.ini.example` 为 `robomind-console.ini`，放在**当前工作目录**
或 **exe 所在目录**（前者优先）。无配置文件 = 全默认 = 关闭。

| 键 | 默认 | 说明 |
|---|---|---|
| `lanchat.enabled` | `false` | 默认关闭，避免一启动就连不上报错 |
| `lanchat.host` | `127.0.0.1` | LanChat 服务端地址 |
| `lanchat.port` | `9001` | LanChat 服务端端口 |
| `lanchat.target` | `0` | **接收者数字用户 id（私聊）。群发需 `SendGroupMsg`(type=16)，未实现。** |

**凭据只从环境变量读**，不进配置文件、不进日志：

```
ROBOMIND_LANCHAT_USER_ID      # 数字 id，账号需预先用 LanChat 客户端注册
ROBOMIND_LANCHAT_PASSWORD     # 明文密码，仅登录时用
```

缺任一变量 → sink 保持不启用，不报错不崩溃。登录被服务端拒绝（type=4）→ 停用并不再重试
（凭据错误是配置问题，重试无意义）。

### 行为

- 告警触发：`dv=O`（结果有效）且 `dl>=2`。**attention_level 未上 wire**
  （板端 payload 仅 `dv/dl/ev/es/mv/ms/ab/dg` 8 键），第一版不判，等字段上 wire 再接。
- 去抖：同一告警 30 秒内不重复发。
- 断线重连：指数退避 1s→2s→4s→8s→16s→30s（上限 30s），连上即复位。
- 脱敏日志：每条发送打 `type + 长度 + 目标 id`（msg 只打前 20 字符 + 总长度，
  密码永不出现），写入**当前工作目录** `lanchat-sink.log` 并同步 qInfo。

### 单机验证（不需要板子）

```bash
py tools/fake_lanchat.py                 # 假服务端：struct.unpack 独立解帧，打印收到的 JSON
py tools/replay_frames.py --dv O --dl 2  # 真实抓包帧等长替换 dv/dl 后注入 9000
```

`tools/replay_frames.py` 只重写信封 seq/ts 与可选的 dv/dl 单字符，帧结构来自
`testdata/frames_fixed.bin` 真实抓包。headless 回归注入：

```bash
./build/robomind-headless.exe --port 9000 --duration 30 --expect 100 --min-fps 8 --max-fps 12 &
py tools/replay_frames.py --repeat 10 --ts-step 100 --pace 0.1   # 100 帧，ts 跨度 9.9s → fps=10
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
├── assets/
│   ├── gen_icon.py     # 图标生成器（纯 Pillow 绘制，无外部位图素材）
│   ├── app.rc.in       # 资源脚本模板，由 CMake 注入 app.ico 绝对路径
│   ├── app.ico         # 7 尺寸内嵌（16/24/32/48/64/128/256）
│   └── icon-*.png      # 各尺寸预览，便于肉眼验收
├── src/
│   ├── main.cpp        # M1 入口
│   ├── mainwindow.cpp  # M1：QTcpServer:9000 + 连接三态 + PPM 面板
│   ├── frame_stream.h  # 流式拆帧（粘包/半包/坏长度头）
│   ├── headless.cpp    # 无窗口接收端逻辑（供 robomind-headless 复用）
│   ├── headless_main.cpp
│   ├── framecheck.cpp  # 用真实 MessageCodec 校验抓包文件
│   └── selfcheck.cpp   # 6 组向量 + 帧解析器自检
├── testdata/           # buggy 必失败 / fixed 必通过的对照帧文件
└── README.md
```

---

## 应用图标

图标是**代码画出来的**，不是位图素材：改颜色、改比例重跑一次 `python assets/gen_icon.py` 即可，
没有"找不到源文件"的问题。

```bash
python assets/gen_icon.py     # 需要 Pillow
```

### 两个必须知道的坑

**坑 1：每个尺寸单独绘制，不能让一份大图降采样。**
v1 版本用 1024 母版缩到 16 px，53 px 的描边缩完只剩 0.83 px —— 低于 1 像素，
16 px 图标里整只眼睛直接消失。现在 `render(size)` 按尺寸分支：≥48 px 走
"描边 + 虹膜环 + 瞳孔"，≤32 px 走"实心眼形挖空瞳孔"，小尺寸反而更清楚。

**坑 2：不能用 `enable_language(RC)` + `target_sources(app.rc)`。**
MinGW 的 `windres` 把 `-I` 路径转交预处理器 `cc1` 时**不加引号**，而本工程路径含空格
（`E:/Github Project/...`），于是 `cc1` 收到 `-IE:/Github` 和一个孤立的 `Project/...`：

```
cc1.exe: fatal error: Project/RoboMind-Console/...: No such file or directory
windres.exe: preprocessing failed.
```

实测**完全不给 `-I` 时成功**（`.rc` 里只有一条 `ICON` 语句，不需要任何 `#include`），
所以 CMakeLists 里自建 `add_custom_command` 调 `windres`，并把产物标成
`EXTERNAL_OBJECT` 直接进链接行。

### 怎么验证图标真的进去了

`windres` 失败经常只打印一行 warning 而**不中断链接** —— exe 照常产出、图标静默缺失，
"构建成功"完全没有说服力。要实测：

```bash
objdump -h build/dist/RoboMindConsole.exe | grep rsrc    # 应有 .rsrc 段，约 34 KB
```

进一步证明可解 PE 资源树，`RT_ICON(3)` 与 `RT_GROUP_ICON(14)` 都应存在 —— 缺
`RT_GROUP_ICON` 时资源管理器仍显示默认图标（只有 256 单帧能被部分场景接受）。

### 改图标为什么必须重新链接

`.ico` 是**编译进 exe 资源段**的，不是运行时读取的旁挂文件。
改了 `app.ico` 却只 `windeployqt` 而不重新构建，图标不会变。

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
