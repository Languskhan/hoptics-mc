# A stand-in for CoreScope's /ws: handshake, then packets shaped like its broadcasts, a ping (must get a pong),
# a fragmented message, one too big to keep, a non-packet message, then a close. Prints what the client sent.
import socket, base64, hashlib, struct, json, sys, time, os
mode = sys.argv[1] if len(sys.argv) > 1 else "ok"
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 8765)); s.listen(1)
def frame(op, data, fin=True):
    h = bytes([(0x80 if fin else 0) | op]); n = len(data)
    h += bytes([n]) if n < 126 else (bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
    return h + data
def pkt(raw, obs, name, snr):
    p = {"id": 1, "raw_hex": raw, "hash": "x", "observer_id": obs, "observer_name": name, "snr": snr, "rssi": -100, "path_json": "[]"}
    m = dict(p); m["decoded"] = {"header": {"payloadType": 5}, "payload": {"text": "x" * 200}}; m["packet"] = p
    return json.dumps({"type": "packet", "data": m}).encode()
def accept():
    conn, _ = s.accept()
    req = b""
    while b"\r\n\r\n" not in req: req += conn.recv(1024)
    print("REQUEST:", req.decode().split("\r\n")[0], "|", [l for l in req.decode().split("\r\n") if l.startswith(("Host", "User-Agent", "Sec-WebSocket-Version"))])
    return conn, req
conn, req = accept()
if mode == "moved":  # the old address forwards to the new one, as corescope.sacmesh.org does to scope.sacmesh.org
    conn.sendall(b"HTTP/1.1 301 Moved Permanently\r\nLocation: ws://localhost:8765/ws\r\nContent-Length: 0\r\n\r\n"); time.sleep(0.3); conn.close()
    conn, req = accept()
if mode == "refuse":
    conn.sendall(b"HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n"); time.sleep(1); conn.close(); sys.exit()
key = [l.split(": ")[1] for l in req.decode().split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC11B87").encode()).digest()).decode()
conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
conn.sendall(frame(1, pkt("15011234AABBCCDD", "A1B2C3D4E5F60011", "Folsom Ridge", 7.5)))
conn.sendall(frame(9, b"hello"))  # ping
whole = pkt("1602AAAA0102030405", "0102030405060708", "S2 Observer", -3.25)
conn.sendall(frame(1, whole[:100], fin=False) + frame(0, whole[100:300], fin=False) + frame(9, b"mid") + frame(0, whole[300:], fin=True))
conn.sendall(frame(1, b'{"type":"packet","data":{"raw_hex":"' + b"AB" * 9000 + b'"}}'))  # 18 KB: skipped
conn.sendall(frame(1, json.dumps({"type": "stats", "data": {"n": 5}}).encode()))
for i in range(30):  # a burst bigger than the ring (16): the oldest drop
    conn.sendall(frame(1, pkt("1501%02x00112233" % i, "FFEE000000000000", "Burst", 1)))
conn.sendall(frame(1, pkt("1501BEEF", "1111111111111111", "Last", 2)))
time.sleep(0.5)
conn.setblocking(False)
got = b""
try:
    while True:
        d = conn.recv(4096)
        if not d: break
        got += d
except BlockingIOError: pass
i = 0
while i + 6 <= len(got):  # decode the client's (masked) frames
    op = got[i] & 15; n = got[i + 1] & 127; masked = got[i + 1] & 128; mk = got[i + 2:i + 6]
    data = bytes(b ^ mk[j % 4] for j, b in enumerate(got[i + 6:i + 6 + n]))
    print("CLIENT FRAME op=%x masked=%d data=%r" % (op, masked >> 7, data)); i += 6 + n
time.sleep(0.3)
if mode == "drop": conn.close(); time.sleep(2); sys.exit()
conn.sendall(frame(8, b"\x03\xe8")); time.sleep(0.5); conn.close()
