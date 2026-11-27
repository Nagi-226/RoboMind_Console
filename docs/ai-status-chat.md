# LanChat 文字自由问答

2026-11-27：Console 可让 `RoboMind-S3_DS` 与 `RoboMind-S3_GPT` 同时登录。DeepSeek 官方 `deepseek-flash`、微链 `gpt-6.2-sol` 已通过真实 S3 状态问答。各自的私聊、模型上下文与调用计数独立，共用一条板端连接。

## 使用

登录 LanChat 后，在 **左下角齿轮 → 设置 → 板端连接 → AI 状态问答** 保存并验证各模型配置；在同页填写 **GPT 板端账号 ID / 密码** 并点 **保存并连接**。两个账号分别从联系人进入私聊。配置步骤见 [LanChat AI 设置](https://github.com/Nagi-226/LanChat-Next/blob/master/docs/ai-status-chat.md)。

- 私聊可直接输入“现在感知情况怎么样”“还有多少内存”“当前链路需要排查什么”，可继续追问。
- 配置群中可向主账号发 `/AI 问题` 或 `/问 问题`；GPT 目前只处理私聊，机器人间消息不会触发模型。
- `/AI 清空` 清除当前发送者、当前私聊或群的模型上下文；有回答进行中时等待完成再清空。
- 感知 / 链路 / 设备 / 帮助和 `/日志` 仍由原确定性查询处理，不消耗模型调用。

## 状态依据与职责

每次问题先经共享的 `StatusQuery` 校验感知、链路和设备三类快照。为适配固件同一时刻仅处理一项查询的限制，Console 将板端查询串行发送；并发的同类查询共用一次尚在处理的采样，回复各自返回发起方。板端离线、旧固件、无效、过期、未计算及超时均明确写入本轮上下文。历史回答用于理解追问，本轮设备状态以本次快照为准。

Console 同时提供连接情况、解码失败 / 非法帧头累计及 USB 日志采集状态 / 缓存数量。**不提供摄像头图像、完整日志、附件或 API 凭据**。模型没有设备控制、文件、命令或网络工具；用户的控制要求只能得到解释或建议。

AI 完整回答返回原 LanChat 会话，不回传到容量有限的 S3 收件箱。原始文字下行和显示回执继续独立处理。S3 继续只运行终端、真实感知与 VVOS；没有在 MCU 上运行聊天大模型。

双账号在线时，收到有消息 ID 的私聊文字便请求切到该账号的板端角色。字幕和语音等待目标画面确认；若用户已切向另一角色，迟到的回答只留在原私聊。板端没有切换能力时，仍可以正常收发 PC 私聊。

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

2026-11-23 曾记录商汤渠道的 `deepseek-flash` HTTP 429：客户端提示暂停 60 秒，服务商具体限流规则未确认。后续的 DeepSeek 官方渠道成功验证，与该历史记录是两组配置。

独立运行可设 `ROBOMIND_AI_MAX_TOKENS`、`ROBOMIND_AI_TIMEOUT_SECONDS`、`ROBOMIND_AI_DAILY_LIMIT`（0=不限）、`ROBOMIND_AI_INTERVAL_SECONDS`、`ROBOMIND_AI_CONCURRENCY`、`ROBOMIND_AI_USAGE_FILE`。测试使用隔离目录，避免修改生产计数。新回归 `tools/test_ai_limits.py` 全部使用本机模拟接口，不产生模型费用。
- 每个模型实例默认最多 4 个回答任务，同会话最多 1 个；默认最短间隔 2 秒。费用随提供方和实际使用量计算，当前没有账户级金额上限。
- 模型上下文仅留 Console 内存，最多三轮 / 8 KiB；10 分钟无活动过期，最多约 16 个会话。群内按发送者隔离，不共享私聊上下文。
- **LanChat 聊天记录仍按既有规则持久化**，清空模型上下文不会删除聊天记录。
- 模型解释可能出错；原确定性查询保留用于核对。没有时间序列时不能据单次内存快照判定泄漏，没有有效感知结果时不能断言正常。

单独运行 Console 可设置 `ROBOMIND_AI_ENABLED=1`、`ROBOMIND_AI_PROVIDER`、`ROBOMIND_AI_ENDPOINT`、`ROBOMIND_AI_MODEL`、`ROBOMIND_AI_KEY`。仅配置服务商环境变量不会自动开启 AI。

独立启动第二个账号时可设置 `ROBOMIND_GPT_USER_ID`、`ROBOMIND_GPT_PASSWORD`、`ROBOMIND_GPT_ENABLED=1`、`ROBOMIND_GPT_PROVIDER`、`ROBOMIND_GPT_ENDPOINT`、`ROBOMIND_GPT_MODEL`、`ROBOMIND_GPT_KEY`，以及可选的 `ROBOMIND_GPT_REASONING_EFFORT` 和 `ROBOMIND_GPT_USAGE_FILE`。必须和主账号使用不同的 ID；通常由 LanChat 填好并传给子进程。

## 验证

2026-11-27 双账号回归覆盖同一板端的并发查询、上下文与计数隔离、早到回复等待角色回执、迟到回复不抢屏，以及连接板端和语音工作器时正常退出：

```powershell
py -3.12 tools/test_dual_ai_chat.py --server '<LanChat 服务端 exe>'
```

生产账号的 `/链路` 私聊点验收到两条实板回复，报告角色依次为 `whale`、`dragon`。双 AI 群内互动待实现与验证。

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
