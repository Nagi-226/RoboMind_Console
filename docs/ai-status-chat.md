# LanChat 文字自由问答

2026-11-24：文字问答继续由 PC / Console 提供；用户官方 DeepSeek `deepseek-flash` 已通过真实 S3 状态问答。沉浸模式新增 Windows 中文 TTS、有界 PCM 与逐句字幕，麦克风和小智协议留后续。

## 使用

登录 LanChat 后，在 **左下角齿轮 → 设置 → 板端连接 → AI 状态问答** 设置协议、接口地址、模型和密钥，勾选启用，点击 **保存并连接**。点击 **在聊天中询问板端状态** 打开专用板端账号的私聊。

- 私聊可直接输入“现在感知情况怎么样”“还有多少内存”“当前链路需要排查什么”，可继续追问。
- 配置群中用 `/AI 问题` 或 `/问 问题`；普通群聊不触发模型。
- `/AI 清空` 清除当前发送者、当前私聊或群的模型上下文；有回答进行中时等待完成再清空。
- 感知 / 链路 / 设备 / 帮助和 `/日志` 仍由原确定性查询处理，不消耗模型调用。

## 状态依据与职责

每次问题先经原 `StatusQuery` 校验感知、链路、设备三类快照，5 秒内分别采集；板子离线、旧固件、无效、过期、未计算、超时均明确写入本轮模型上下文。模型解释必须以本轮快照为准，历史回答只用于理解追问。回答带 PC 快照采集时刻。

Console 同时提供连接情况、解码失败 / 非法帧头累计及 USB 日志采集状态 / 缓存数量。**不提供摄像头图像、完整日志、附件或 API 凭据**。模型没有设备控制、文件、命令或网络工具；用户的控制要求只能得到解释或建议。

AI 完整回答返回原 LanChat 会话，不回传到容量有限的 S3 收件箱。原始文字下行和显示回执继续独立处理。S3 继续只运行终端、真实感知与 VVOS；没有在 MCU 上运行聊天大模型。

## 接口与边界

- `anthropic`：基地址自动补 `/v1/messages`；支持 DeepSeek Anthropic 兼容接口，发送 Bearer 认证和 `anthropic-version: 2023-06-01`。
- `openai`：基地址自动补 `/v1/chat/completions`，发送 Bearer 认证。
- OpenAI 兼容渠道可选 `ROBOMIND_AI_REASONING_EFFORT` / LanChat 的“思考强度”；为空不发送，仅 OpenAI 请求带该可选字段。商汤 `https://token.sensenova.cn/v1` / `deepseek-flash` 使用 none 已通过真实接口与真实 LanChat 服务端的单次回复验证；官方标注模型为 DeepSeek V4.1 Flash 1017。通用知识问题可回答，真实设备状态仍经原快照校验。
- 远端要求 HTTPS；本机 `localhost` / loopback 可使用 HTTP 连接本地模型。重定向不自动携带密钥继续请求。
- 密钥由 LanChat 使用当前 Windows 用户 DPAPI 加密保存，仅通过子进程环境交给 Console，不写入桥 INI、状态快照或日志。安装包不含用户密钥。
- 每个问题最多 2048 字节，回答最多 4096 字节；默认每次 1200 个输出 token / 45 秒，可由 LanChat 设置调整。达到模型输出上限时明确提示可继续追问，失败不自动重试付费请求。

### 调用限额与 429

LanChat 登录后 **设置 → AI 与会话** 配置输出 token、超时、每日调用次数、全局间隔和最大并发；默认每日 100 次 / 间隔 2 秒 / 最大 4 个任务，每会话仍最多一个。失败计入次数，计数在生成请求前保存到本机 `ai-usage.json`，跨日恢复、重启保留；该文件无密钥或对话。输入 / 输出 token 使用响应 usage，本机统计不代表余额或账单。

HTTP 429 根据提供方错误分类为限流 / 额度不足 / 原因不明确，正文不直接回传。遵守 Retry-After 秒数 / HTTP 日期，缺省 60 秒，限制 1–3600 秒；冷却落盘、重启保留，不自动重试。冷却和次数上限不影响原设备 / 日志查询。旧 21:30 记录没有保留服务商错误详情，不能据此确定额度或频率原因。

2026-11-23 本机部署 2026.11.23.2 后，最新生产状态将 `deepseek-flash` 的 429 归类为 `rate_limit`，提示暂停 60 秒；复核时冷却已结束。服务商具体限流规则尚未确认；模型列表 GET 成功不代表生成请求已恢复，目前没有新的成功生成记录。本轮没有主动重发生产生成请求。

下一次按用户决定改用自己的 DeepSeek 官方 API；已有 OpenAI 兼容接口可配置该渠道，但模型 ID、权限和真实回答仍需联调。先使用模型默认思考配置，本轮没有切换生产凭据或进行官方 API 调用。鲸鱼娘角色与语音继续待办。

独立运行可设 `ROBOMIND_AI_MAX_TOKENS`、`ROBOMIND_AI_TIMEOUT_SECONDS`、`ROBOMIND_AI_DAILY_LIMIT`（0=不限）、`ROBOMIND_AI_INTERVAL_SECONDS`、`ROBOMIND_AI_CONCURRENCY`、`ROBOMIND_AI_USAGE_FILE`。测试使用隔离目录，避免修改生产计数。新回归 `tools/test_ai_limits.py` 全部使用本机模拟接口，不产生模型费用。
- 全局最多 4 个回答任务，同会话最多 1 个；同会话至少间隔 2 秒。费用随提供方和实际使用量计算，当前没有账户级金额上限。
- 模型上下文仅留 Console 内存，最多三轮 / 8 KiB；10 分钟无活动过期，最多约 16 个会话。群内按发送者隔离，不共享私聊上下文。
- **LanChat 聊天记录仍按既有规则持久化**，清空模型上下文不会删除聊天记录。
- 模型解释可能出错；原确定性查询保留用于核对。没有时间序列时不能据单次内存快照判定泄漏，没有有效感知结果时不能断言正常。

单独运行 Console 可设置 `ROBOMIND_AI_ENABLED=1`、`ROBOMIND_AI_PROVIDER`、`ROBOMIND_AI_ENDPOINT`、`ROBOMIND_AI_MODEL`、`ROBOMIND_AI_KEY`。仅配置服务商环境变量不会自动开启 AI。

## 验证

2026-11-24 沉浸语音链路新增 TCP_NODELAY，配合 S3 预缓冲 / DMA 和首尾渐变完成固定音源、网络停顿与实际 LAN / 本地 VITS / S3 回归。用户已确认“音效合格”，优化随本轮合入 `master`；诊断、固件哈希与测试边界见 [S3 音质记录](https://github.com/Nagi-226/RoboMind-S3/blob/master/docs/audio-quality-20261124.md)。本次发布没有新增模型调用或重新构建安装器。

2026.11.23.2 完整安装包在移除开发 Qt PATH 后，使用安装目录 Console 完成 Anthropic / OpenAI 两种模拟器检查；安装、重复安装、卸载和个人文件保留通过。对应 LanChat / Console 功能提交为 `61f4ec9` / `e00eba7`，详细产物与证据见 [LanChat 发布记录](https://github.com/Nagi-226/LanChat-Next/blob/master/docs/windows-release-20261123.md)。本次发布没有新增付费生成或实板验收。

```powershell
py -3.12 tools/test_ai_status_chat.py
# 指定安装目录中的 Console，核对随包运行库与两种接口
py -3.12 tools/test_ai_status_chat.py --console '<安装目录>/robomind-console/RoboMindConsole.exe'
py -3.12 tools/test_status_queries.py
py -3.12 tools/test_real_ai_status_chat.py --server '../LanChat-Next/build/server-mingw/src/server/lanchat_server_next.exe' --output build/ai-validation/real-s3.json
```

前两项使用本机模型模拟器 / 独立 TCP peers；真实模型测试需要用户授权的 API 配置、物理 S3 以及空闲的板端监听端口，使用临时账号和隔离数据库。模拟器覆盖两种协议、状态刷新 / 过期 / 离线、追问 / 清空 / 隔离、群路由、判重、忙碌与 HTTP 错误、凭据不进入正文和日志。

Qt 客户端验证包括密钥加密恢复、桥 INI 不含凭据、问答导航按钮和已有聊天 / 日志 / 窗口回归。真实模型测试的回答用于核对快照语义，不能作为模型准确率或感知算法准确度结论。

## 后续

1. 语音输出已完成 Windows 中文 / 本地 VITS 少女声线 → ES8388；后续增加麦克风 / ASR / 唤醒、更多音色及精准口型，与文字问答共用会话。当前见 `LanChat-Next/docs/whale-immersion-plan.md`。
2. 小智通用 AI：核对 `atk-dnesp32s3` 板型、OV5640 相机差异、协议 / 激活服务与 SDK，逐项复用音频、MCP 工具；保持现有五页终端。
3. 通用项目工具：由各仓适配器提供真实状态；控制能力需要单独定义权限与执行确认。
