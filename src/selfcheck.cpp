// M2 · selfcheck：逐字节/逐字段验证 VVOS MessageCodec 的 6 组向量
//
// 设计要点：
//   1. 不依赖 Qt —— 解码正确性与 GUI 无关，分开才能单独定位故障（架构师的设计意图）
//   2. 任一向量失败 ⇒ 退出码非 0，便于 CI/脚本判定
//   3. 向量来源：VVOS docs/u02-u03-implementation-spec-2026-11-13.md §3.8
//
// 帧格式（与固件 U03 编码器一致）：[u32 大端 长度] + 文本；长度不含头 4 字节。

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "interconnect/message_codec.hpp"
#include "interconnect/message_envelope.hpp"

namespace {

using vr::core::ErrorCode;
using vr::interconnect::MessageCodec;
using vr::interconnect::MessageEnvelope;

int g_fail = 0;

void Check(bool ok, const std::string& what, const std::string& detail = "")
{
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
        return;
    }
    ++g_fail;
    std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  -> ", detail.c_str());
}

// ---------------------------------------------------------------- 帧解析器
// 模拟 TCP 流：按 [u32 BE 长度] 切帧，处理粘包/半包。
class FrameDecoder {
public:
    void Feed(const uint8_t* data, size_t len)
    {
        buf_.insert(buf_.end(), data, data + len);
    }

    // 取出一个完整帧的文本；不足一帧返回 false（= 半包，等待更多数据）
    bool Next(std::string* out)
    {
        if (buf_.size() < 4) {
            return false;
        }
        const size_t text_len = (static_cast<size_t>(buf_[0]) << 24) |
                                (static_cast<size_t>(buf_[1]) << 16) |
                                (static_cast<size_t>(buf_[2]) << 8) |
                                static_cast<size_t>(buf_[3]);
        if (buf_.size() < 4 + text_len) {
            return false;
        }
        out->assign(reinterpret_cast<const char*>(buf_.data()) + 4, text_len);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(4 + text_len));
        return true;
    }

private:
    std::vector<uint8_t> buf_;
};

std::vector<uint8_t> MakeFrame(const std::string& text)
{
    std::vector<uint8_t> f;
    const uint32_t n = static_cast<uint32_t>(text.size());
    f.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
    f.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    f.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    f.push_back(static_cast<uint8_t>(n & 0xFF));
    f.insert(f.end(), text.begin(), text.end());
    return f;
}

// ---------------------------------------------------------------- 向量 1
void Vector1()
{
    const std::string text =
        "1|telemetry|best_effort|42|987654321|300|ppm-a3f1-7b2c|vvos|ppm/state||"
        "dv=O;dl=2;ev=O;es=1;mv=N;ms=-;ab=350;dg=1";
    std::printf("\n向量 1 · 典型（期望文本 112 B / 帧 116 B / 头 00 00 00 70）\n");
    std::printf("  文本长度 = %zu （期望 112）\n", text.size());
    Check(text.size() == 112, "文本长度 == 112");

    MessageEnvelope env;
    const ErrorCode rc = MessageCodec::Decode(text, &env);
    Check(rc == ErrorCode::kOk, "Decode == kOk");
    Check(env.schema_version == 1U, "schema_version == 1");
    Check(env.sequence == 42U, "sequence == 42");
    Check(env.timestamp_ms == 987654321ULL, "timestamp_ms == 987654321");
    Check(env.ttl_ms == 300U, "ttl_ms == 300");
    Check(env.source == "ppm-a3f1-7b2c", "source 还原");
    Check(env.target == "vvos", "target 还原");
    Check(env.topic == "ppm/state", "topic 还原");
    Check(env.trace_id.empty(), "trace_id 为空");
    Check(env.payload == "dv=O;dl=2;ev=O;es=1;mv=N;ms=-;ab=350;dg=1", "payload 逐字还原");

    const std::vector<uint8_t> frame = MakeFrame(text);
    std::printf("  帧长度 = %zu （期望 116）\n", frame.size());
    Check(frame.size() == 116, "帧长度 == 116");
    Check(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0x70,
          "长度头 == 00 00 00 70");
}

// ---------------------------------------------------------------- 向量 2
void Vector2()
{
    const std::string text =
        "1|telemetry|best_effort|1|1000|300|ppm-a3f1-7b2c|vvos|ppm/state||"
        "dv=I;dl=0;ev=I;es=-;mv=I;ms=-;ab=65535;dg=5";
    std::printf("\n向量 2 · 降级+饱和（期望文本 108 B / 帧 112 B / 头 00 00 00 6C）\n");
    std::printf("  文本长度 = %zu （期望 108）\n", text.size());
    Check(text.size() == 108, "文本长度 == 108");

    MessageEnvelope env;
    const ErrorCode rc = MessageCodec::Decode(text, &env);
    Check(rc == ErrorCode::kOk, "Decode == kOk");
    Check(env.payload == "dv=I;dl=0;ev=I;es=-;mv=I;ms=-;ab=65535;dg=5", "payload 逐字还原");

    const std::vector<uint8_t> frame = MakeFrame(text);
    std::printf("  帧长度 = %zu （期望 112）\n", frame.size());
    Check(frame.size() == 112, "帧长度 == 112");
    Check(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0x6C,
          "长度头 == 00 00 00 6C");
}

// ---------------------------------------------------------------- 向量 3
// ⚠️ 规格 §3.8 明确标注：转义是"经同路径验证"，不是逐字段字面验证。
// 这里对 source 与 topic 两个字段都做一次往返，比规格要求更严一点。
void Vector3()
{
    std::printf("\n向量 3 · 转义（`|` 前加 `\\`，解码后须还原）\n");

    // 3a：转义出现在 source（规格实际验证的就是这条路径）
    {
        const std::string text =
            "1|telemetry|best_effort|1|1000|300|ppm/sta\\|te|vvos|ppm/state||dv=O;dl=0;"
            "ev=O;es=0;mv=O;ms=0;ab=0;dg=0";
        MessageEnvelope env;
        const ErrorCode rc = MessageCodec::Decode(text, &env);
        Check(rc == ErrorCode::kOk, "source 含转义：Decode == kOk");
        Check(env.source == "ppm/sta|te", "source 还原为 ppm/sta|te",
              "实际=[" + env.source + "]");
    }

    // 3b：转义出现在 topic（规格字面要求，但固件侧 topic 是编译期常量，无法真发）
    {
        const std::string text =
            "1|telemetry|best_effort|1|1000|300|ppm-a3f1-7b2c|vvos|ppm/sta\\|te||dv=O;dl=0;"
            "ev=O;es=0;mv=O;ms=0;ab=0;dg=0";
        MessageEnvelope env;
        const ErrorCode rc = MessageCodec::Decode(text, &env);
        Check(rc == ErrorCode::kOk, "topic 含转义：Decode == kOk");
        Check(env.topic == "ppm/sta|te", "topic 还原为 ppm/sta|te",
              "实际=[" + env.topic + "]");
    }
}

// ---------------------------------------------------------------- 向量 4
void Vector4()
{
    const std::string text =
        "1|telemetry|best_effort|1|1000|300|ppm-a3f1-7b2c|vvos|||dv=O;dl=0;ev=O;es=0;"
        "mv=O;ms=0;ab=0;dg=0";
    std::printf("\n向量 4 · 空字段（topic 与 trace_id 均空，不得变成 \"null\" 或 \"\\\"\\\"\"）\n");

    MessageEnvelope env;
    const ErrorCode rc = MessageCodec::Decode(text, &env);
    Check(rc == ErrorCode::kOk, "Decode == kOk");
    Check(env.topic.empty(), "topic == \"\"", "实际=[" + env.topic + "]");
    Check(env.trace_id.empty(), "trace_id == \"\"", "实际=[" + env.trace_id + "]");
}

// ---------------------------------------------------------------- 向量 5
void Vector5()
{
    std::printf("\n向量 5 · 拒收（负向，全部应为 kInvalidParam）\n");

    struct Case {
        const char* name;
        std::string text;
    };
    const std::string tail = "|1000|300|ppm-a3f1-7b2c|vvos|ppm/state||dv=O";

    const std::vector<Case> cases = {
        {"schema_version = 0", "0|telemetry|best_effort|1" + tail},
        {"channel 带尾空格", "1|telemetry |best_effort|1" + tail},
        {"qos 用连字符", "1|telemetry|at-least-once|1" + tail},
        {"sequence = 0x1F", "1|telemetry|best_effort|0x1F" + tail},
        {"sequence = 42abc", "1|telemetry|best_effort|42abc" + tail},
        {"sequence = +5", "1|telemetry|best_effort|+5" + tail},
        {"sequence = -0", "1|telemetry|best_effort|-0" + tail},
        {"ttl_ms 前导空格", "1|telemetry|best_effort|1|1000| 300|ppm-a3f1-7b2c|vvos|ppm/state||dv=O"},
        {"字段数 = 9", "1|telemetry|best_effort|1|1000|300|ppm-a3f1-7b2c|vvos|ppm/state"},
    };

    for (const auto& c : cases) {
        MessageEnvelope env;
        const ErrorCode rc = MessageCodec::Decode(c.text, &env);
        Check(rc == ErrorCode::kInvalidParam, c.name,
              "实际 rc=" + std::to_string(static_cast<int>(rc)));
    }
}

// ---------------------------------------------------------------- 向量 6
// 粘包/半包：同一帧拆成 3 段喂入，结果必须与一次喂入完全相同。
// 这是 PC 端**唯一**会真实遇到的协议边界（TCP 是字节流，没有消息边界）。
void Vector6()
{
    const std::string text =
        "1|telemetry|best_effort|42|987654321|300|ppm-a3f1-7b2c|vvos|ppm/state||"
        "dv=O;dl=2;ev=O;es=1;mv=N;ms=-;ab=350;dg=1";
    const std::vector<uint8_t> frame = MakeFrame(text);
    std::printf("\n向量 6 · 粘包/半包（帧 %zu B 拆 3 段）\n", frame.size());

    // 基准：一次喂入
    MessageEnvelope once;
    {
        FrameDecoder d;
        d.Feed(frame.data(), frame.size());
        std::string got;
        Check(d.Next(&got), "一次喂入：取到完整帧");
        Check(MessageCodec::Decode(got, &once) == ErrorCode::kOk, "一次喂入：Decode == kOk");
    }

    // 拆 3 段：4 / 一半 / 余下（第一段只够长度头，必然半包）
    const size_t c1 = 4;
    const size_t c2 = 4 + (frame.size() - 4) / 2;
    FrameDecoder d;
    std::string got;
    d.Feed(frame.data(), c1);
    Check(!d.Next(&got), "第 1 段（仅长度头）：应判为半包，不出帧");
    d.Feed(frame.data() + c1, c2 - c1);
    Check(!d.Next(&got), "第 2 段（一半）：应仍为半包");
    d.Feed(frame.data() + c2, frame.size() - c2);
    const bool got_full = d.Next(&got);
    Check(got_full, "第 3 段后：应取出完整帧");

    MessageEnvelope split;
    Check(MessageCodec::Decode(got, &split) == ErrorCode::kOk, "拆包：Decode == kOk");

    // 逐字段比对
    Check(split.sequence == once.sequence, "拆包 sequence 一致");
    Check(split.timestamp_ms == once.timestamp_ms, "拆包 timestamp_ms 一致");
    Check(split.payload == once.payload, "拆包 payload 一致");
    Check(split.source == once.source && split.topic == once.topic, "拆包 source/topic 一致");

    // 残留：不应还有半帧
    std::string extra;
    Check(!d.Next(&extra), "拆包后缓冲区无残留帧");
}

}  // namespace

int main()
{
    std::printf("=== RoboMind Console M2 selfcheck ===\n");
    std::printf("来源：VVOS docs/u02-u03-implementation-spec-2026-11-13.md §3.8\n");

    Vector1();
    Vector2();
    Vector3();
    Vector4();
    Vector5();
    Vector6();

    std::printf("\n=== 结果：%s（失败 %d 项）===\n", (g_fail == 0) ? "全部通过" : "存在失败", g_fail);
    return (g_fail == 0) ? 0 : 1;
}
