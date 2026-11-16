"""回放 testdata/frames_fixed.bin 到 Console 的 TCP 端口（默认 9000）。

帧结构来自真实抓包，本工具只做两类受控改写：
  1. 信封 seq/ts（第 4/5 字段）重写为单调递增 —— headless 的 fps 判据按 ts 跨度算，
     原样重放 10 遍 ts 会回绕，fps 判据无意义。
  2. 可选等长替换 payload 里的 dv/dl 单字符（--dv O --dl 2），用于触发告警路径。
     替换前后帧长度不变，其余字段逐字节保留。

用法示例：
  py tools/replay_frames.py --repeat 10 --ts-step 100 --pace 0.1   # 100 帧 fps=10，headless 回归
  py tools/replay_frames.py --dv O --dl 2                          # 10 帧告警帧，去抖验证
"""
import argparse
import re
import socket
import struct
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=9000)
ap.add_argument("--file", default="testdata/frames_fixed.bin")
ap.add_argument("--repeat", type=int, default=1)
ap.add_argument("--ts-step", type=int, default=100, help="每帧 ts 增量(ms)")
ap.add_argument("--pace", type=float, default=0.1, help="每帧发送间隔(s)")
ap.add_argument("--dv", default=None, help="等长替换 dv 值，如 O")
ap.add_argument("--dl", default=None, help="等长替换 dl 值，如 2")
args = ap.parse_args()

data = open(args.file, "rb").read()
frames = []
off = 0
while off + 4 <= len(data):
    n = struct.unpack(">I", data[off:off + 4])[0]
    frames.append(data[off + 4:off + 4 + n])
    off += 4 + n
assert off == len(data), "帧文件有残留字节"
print(f"loaded {len(frames)} frames from {args.file}")

sock = socket.create_connection((args.host, args.port))
print(f"connected {args.host}:{args.port}")
seq = 0
ts = 7278   # 原抓包首帧 ts，保持量级一致
sent = 0
for _ in range(args.repeat):
    for payload in frames:
        text = payload.decode("utf-8")
        parts = text.split("|", 5)   # 前 5 字段单独取，其余（含 payload）整体保留
        parts[3] = str(seq)
        parts[4] = str(ts)
        body = "|".join(parts)
        # seq/ts 重写允许变长（长度头会重算）；dv/dl 替换必须等长
        base_len = len(body.encode("utf-8"))
        if args.dv is not None:
            body = re.sub(r"\bdv=[A-Z]", f"dv={args.dv}", body)
        if args.dl is not None:
            body = re.sub(r"\bdl=\d", f"dl={args.dl}", body)
        out = body.encode("utf-8")
        assert len(out) == base_len, "dv/dl 替换改变了帧长度"
        sock.sendall(struct.pack(">I", len(out)) + out)
        seq += 1
        ts += args.ts_step
        sent += 1
        time.sleep(args.pace)
sock.close()
print(f"sent {sent} frames, seq 0..{seq - 1}, ts {7278}..{ts - args.ts_step}")