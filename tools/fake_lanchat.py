import socket, struct, json
HOST, PORT = "127.0.0.1", 9001
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT)); srv.listen(1)
print(f"listening {HOST}:{PORT}")
conn, _ = srv.accept()
buf = b""
while True:
    data = conn.recv(4096)
    if not data: break
    buf += data
    while len(buf) >= 4:
        n = struct.unpack(">I", buf[:4])[0]          # 独立实现，不复用被测代码
        if len(buf) < 4 + n: break
        payload = buf[4:4 + n]; buf = buf[4 + n:]
        print("[FRAME]", n, "bytes ->", json.loads(payload))