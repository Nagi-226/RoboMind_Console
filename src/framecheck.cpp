// framecheck —— 用**真实的 MessageCodec::Decode** 校验从板子抓到的帧文件
//
// 为什么需要它：
//   "PC 收到 N 帧、残留 0 字节"只能证明**字节到达了且长度头自洽**，
//   不能证明能被解开。板端第一版的双层包头 bug 就是这样漏过的：
//   外层头写的 108 让解析器"成功"吃掉 104 字节正文 + 下一帧的头，帧数和残留都正常。
//
// 用法：framecheck <frames.bin>
//   文件内容 = 若干 [u32 大端长度][文本] 拼接（板端 send 的原样字节）
//   退出码 0 = 全部帧 Decode 成功且字段合规

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "interconnect/message_codec.hpp"
#include "interconnect/message_envelope.hpp"

namespace {

using vr::core::ErrorCode;
using vr::interconnect::MessageCodec;
using vr::interconnect::MessageEnvelope;

int g_fail = 0;

void Fail(const std::string& what, const std::string& detail = "")
{
    ++g_fail;
    std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : " -> ", detail.c_str());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("用法: %s <frames.bin>\n", argv[0]);
        return 2;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::printf("打不开文件: %s\n", argv[1]);
        return 2;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

    std::printf("=== framecheck: %s（%zu 字节）===\n", argv[1], raw.size());

    size_t off = 0;
    size_t frames = 0;
    size_t decoded = 0;
    bool printed_first = false;

    while (off + 4 <= raw.size()) {
        const size_t text_len = (static_cast<size_t>(static_cast<unsigned char>(raw[off])) << 24) |
                                (static_cast<size_t>(static_cast<unsigned char>(raw[off + 1])) << 16) |
                                (static_cast<size_t>(static_cast<unsigned char>(raw[off + 2])) << 8) |
                                static_cast<size_t>(static_cast<unsigned char>(raw[off + 3]));
        if (text_len == 0 || text_len > 4096 || off + 4 + text_len > raw.size()) {
            Fail("长度头越界", "off=" + std::to_string(off) + " len=" + std::to_string(text_len));
            break;
        }
        const std::string text(raw.data() + off + 4, text_len);
        off += 4 + text_len;
        ++frames;

        // ★ 这里用的是板端契约的真实实现，不是我自己写的解析
        MessageEnvelope env;
        const ErrorCode rc = MessageCodec::Decode(text, &env);
        if (rc != ErrorCode::kOk) {
            Fail("Decode 失败", "frame#" + std::to_string(frames) +
                                    " rc=" + std::to_string(static_cast<int>(rc)));
            continue;
        }

        // 11 字段的语义校验（Decode 成功之后才谈得上"字段对不对"）
        if (env.schema_version != 1U) {
            Fail("schema_version != 1", std::to_string(env.schema_version));
            continue;
        }
        if (env.ttl_ms != 300U) {
            Fail("ttl_ms != 300", std::to_string(env.ttl_ms));
            continue;
        }
        if (env.source.rfind("ppm-", 0) != 0 || env.source.size() != 13) {
            Fail("source 不是 ppm-XXXX-XXXX 格式", env.source);
            continue;
        }
        if (env.topic != "ppm/state") {
            Fail("topic != ppm/state", env.topic);
            continue;
        }
        if (env.target != "vvos") {
            Fail("target != vvos", env.target);
            continue;
        }

        if (!printed_first) {
            printed_first = true;
            std::printf("\n--- 首帧逐字段（MessageCodec::Decode 解出）---\n");
            std::printf("  schema_version = %u\n", env.schema_version);
            std::printf("  sequence       = %llu\n", static_cast<unsigned long long>(env.sequence));
            std::printf("  timestamp_ms   = %llu\n",
                        static_cast<unsigned long long>(env.timestamp_ms));
            std::printf("  ttl_ms         = %u\n", env.ttl_ms);
            std::printf("  source         = [%s]\n", env.source.c_str());
            std::printf("  target         = [%s]\n", env.target.c_str());
            std::printf("  topic          = [%s]\n", env.topic.c_str());
            std::printf("  trace_id       = [%s]\n", env.trace_id.c_str());
            std::printf("  payload        = [%s]\n", env.payload.c_str());
        }
        ++decoded;
    }

    const size_t tail = raw.size() - off;
    std::printf("\n帧数=%zu  Decode 成功=%zu  失败=%d  残留字节=%zu\n", frames, decoded, g_fail, tail);
    if (tail != 0) {
        Fail("尾部有残留字节（帧边界不齐）", std::to_string(tail));
    }
    std::printf("=== 结果：%s ===\n", (g_fail == 0) ? "全部通过" : "存在失败");
    return (g_fail == 0) ? 0 : 1;
}
