// FrameStream —— 把 TCP 字节流切成完整帧（处理粘包/半包）
//
// 板端发的格式：[u32 大端长度][文本]，一次 send 一帧，但 TCP 是字节流：
//   recv() 可能返回半个帧、也可能返回 3 个半帧。
// 所以必须自己按长度头切，并保留未成帧的残留字节到下次。
//
// 与 framecheck 的区别：framecheck 处理**完整文件**（假设已经切好），
// 这里处理**任意切分的流**。两者共用同一条契约（4 字节头 + 文本）。

#ifndef FRAME_STREAM_H
#define FRAME_STREAM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class FrameStream {
public:
    // 单帧文本上限。板端 kWireLineMax = 320，留余量防异常数据撑爆内存。
    static constexpr size_t kMaxText = 4096;

    // 喂入任意长度的字节；每切出一个完整帧就 append 到 out。
    // 返回本次切出的帧数。
    size_t Feed(const char* data, size_t len, std::vector<std::string>* out)
    {
        if (data != nullptr && len > 0) {
            buf_.insert(buf_.end(), data, data + len);
        }
        size_t produced = 0;
        size_t off = 0;
        while (buf_.size() - off >= 4) {
            const size_t n = (static_cast<size_t>(static_cast<unsigned char>(buf_[off])) << 24) |
                             (static_cast<size_t>(static_cast<unsigned char>(buf_[off + 1])) << 16) |
                             (static_cast<size_t>(static_cast<unsigned char>(buf_[off + 2])) << 8) |
                             static_cast<size_t>(static_cast<unsigned char>(buf_[off + 3]));
            if (n == 0 || n > kMaxText) {
                // 长度头非法：说明流已错位（或对端不是我们的协议）。
                // 丢弃整个缓冲，让下一次连接/下一段数据重新对齐 —— 保留只会持续产出垃圾。
                buf_.clear();
                bad_header_ = true;
                return produced;
            }
            if (buf_.size() - off < 4 + n) {
                break;   // 半包：等更多数据
            }
            if (out != nullptr) {
                out->emplace_back(reinterpret_cast<const char*>(buf_.data()) + off + 4, n);
            }
            off += 4 + n;
            ++produced;
        }
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(off));
        return produced;
    }

    void Reset()
    {
        buf_.clear();
        bad_header_ = false;
    }

    bool bad_header() const { return bad_header_; }
    size_t pending() const { return buf_.size(); }   // 尚未成帧的残留字节数

private:
    std::vector<uint8_t> buf_;
    bool bad_header_{false};
};

#endif  // FRAME_STREAM_H
