#!/usr/bin/env python3
"""WebSocket-to-TCP proxy for directcall.

Browsers connect via WebSocket (port 3457). Messages are forwarded to
directcall's TCP listener (port 3456) using the 2-byte big-endian
length-prefix framing. Responses from directcall are forwarded back
to the WebSocket client as JSON.

This replaces the signal.php + bridge_signal_tcp.py polling approach
with a real-time WebSocket connection.
"""
import asyncio
import json
import os
import ssl
import struct
import sys
import signal
import fcntl

WS_HOST = os.environ.get("WS_HOST", "0.0.0.0")
WS_PORT = int(os.environ.get("WS_PORT", "3457"))
DC_HOST = os.environ.get("DC_HOST", "127.0.0.1")
DC_PORT = int(os.environ.get("DC_PORT", "3456"))
CALLEE_ROOM = os.environ.get("CALLEE_ROOM", "room101")

try:
    import websockets
except ImportError:
    print("Installing websockets...", flush=True)
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "websockets"])
    import websockets


async def tcp_send(writer: asyncio.StreamWriter, msg: str):
    payload = msg.encode("utf-8")
    header = struct.pack("!H", len(payload))
    writer.write(header + payload)
    await writer.drain()


async def tcp_recv_messages(reader: asyncio.StreamReader):
    buf = b""
    while True:
        try:
            chunk = await asyncio.wait_for(reader.read(65536), timeout=0.05)
            if not chunk:
                return None
            buf += chunk
        except asyncio.TimeoutError:
            break

    messages = []
    while len(buf) >= 2:
        pkt_len = struct.unpack("!H", buf[:2])[0]
        if len(buf) < 2 + pkt_len:
            break
        msg = buf[2 : 2 + pkt_len].decode("utf-8", "replace")
        buf = buf[2 + pkt_len :]
        messages.append(msg)
    return messages


def parse_dc_message(msg: str) -> dict:
    if msg.startswith("ANSWER:"):
        return {"type": "answer", "sdp": msg[len("ANSWER:"):]}
    elif msg.startswith("ICE:"):
        payload = msg[len("ICE:"):]
        idx, cand = payload.split(":", 1)
        return {"type": "ice", "candidate": {"candidate": cand, "sdpMLineIndex": int(idx), "sdpMid": "0"}}
    elif msg.startswith("LLAMA:"):
        return {"type": "llama", "text": msg}
    elif msg.startswith("WAITING"):
        return {"type": "status", "status": "WAITING"}
    elif msg.startswith("200 OK"):
        return {"type": "status", "status": "200 OK"}
    elif msg.startswith("486"):
        return {"type": "status", "status": "486 Busy Here"}
    elif msg.startswith("BYE"):
        return {"type": "status", "status": "BYE"}
    else:
        return {"type": "status", "status": msg}


async def handle_client(websocket):
    peer = websocket.remote_address
    print(f"[ws] client connected: {peer}", flush=True)

    reader = writer = None
    try:
        reader, writer = await asyncio.open_connection(DC_HOST, DC_PORT)
        print(f"[ws] connected to directcall {DC_HOST}:{DC_PORT}", flush=True)

        await tcp_send(writer, "HELLO")
        await asyncio.sleep(0.05)
        await tcp_send(writer, f'INVITE:{{"agent":"audio","room_name":"{CALLEE_ROOM}"}}')
        print(f"[ws] sent HELLO + INVITE", flush=True)

        # Wait for WAITING (callee ready)
        ready = False
        for _ in range(300):  # 30 seconds max
            msgs = await tcp_recv_messages(reader)
            if msgs is None:
                print("[ws] directcall disconnected during init", flush=True)
                return
            for m in msgs:
                print(f"[ws] init << {m[:80]}", flush=True)
                parsed = parse_dc_message(m)
                await websocket.send(json.dumps(parsed))
                if "WAITING" in m:
                    ready = True
            if ready:
                break
            await asyncio.sleep(0.1)

        if not ready:
            print("[ws] timeout waiting for WAITING", flush=True)
            await websocket.send(json.dumps({"type": "status", "status": "timeout waiting for WAITING"}))
            return

        print("[ws] callee ready, proxying messages", flush=True)

        async def ws_to_tcp():
            async for raw in websocket:
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                t = msg.get("type")
                if t == "offer":
                    sdp = msg.get("sdp", "")
                    if sdp:
                        await tcp_send(writer, "OFFER:" + sdp)
                        print(f"[ws] >> OFFER ({len(sdp)} bytes)", flush=True)
                elif t == "ice":
                    c = msg.get("candidate") or {}
                    cand = c.get("candidate") if isinstance(c, dict) else None
                    idx = c.get("sdpMLineIndex", 0) if isinstance(c, dict) else 0
                    if cand:
                        await tcp_send(writer, f"ICE:{int(idx)}:{cand}")
                elif t == "face":
                    data = msg.get("data", "")
                    if data:
                        await tcp_send(writer, "FACE:" + data)
                elif t == "hangup":
                    await tcp_send(writer, "BYE")
                    return

        async def tcp_to_ws():
            while True:
                msgs = await tcp_recv_messages(reader)
                if msgs is None:
                    print("[ws] directcall closed connection", flush=True)
                    await websocket.close()
                    return
                for m in msgs:
                    parsed = parse_dc_message(m)
                    try:
                        await websocket.send(json.dumps(parsed))
                    except Exception:
                        return
                    if m.startswith("ANSWER:"):
                        print(f"[ws] << ANSWER ({len(m)} bytes)", flush=True)
                    elif m.startswith("LLAMA:"):
                        print(f"[ws] << {m[:60]}", flush=True)
                await asyncio.sleep(0.02)

        await asyncio.gather(ws_to_tcp(), tcp_to_ws())

    except websockets.exceptions.ConnectionClosed:
        print(f"[ws] client {peer} disconnected", flush=True)
    except ConnectionRefusedError:
        print(f"[ws] cannot connect to directcall {DC_HOST}:{DC_PORT}", flush=True)
        await websocket.send(json.dumps({"type": "status", "status": "directcall not available"}))
    except Exception as e:
        print(f"[ws] error: {e}", flush=True)
    finally:
        if writer:
            try:
                payload = "BYE".encode()
                writer.write(struct.pack("!H", len(payload)) + payload)
                await writer.drain()
            except Exception:
                pass
            writer.close()
        print(f"[ws] session ended for {peer}", flush=True)


async def main():
    print(f"[ws] WebSocket proxy starting on {WS_HOST}:{WS_PORT} -> {DC_HOST}:{DC_PORT}", flush=True)
    ssl_ctx = None
    cert = os.environ.get("WS_CERT", "/opt/directcall/cert.pem")
    key = os.environ.get("WS_KEY", "/opt/directcall/key.pem")
    if os.path.exists(cert) and os.path.exists(key):
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(cert, key)
        print(f"[ws] TLS enabled with {cert}", flush=True)
    async with websockets.serve(handle_client, WS_HOST, WS_PORT, ssl=ssl_ctx):
        proto = "wss" if ssl_ctx else "ws"
        print(f"[ws] listening on {proto}://{WS_HOST}:{WS_PORT}", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    lock_path = "/tmp/ws_proxy.lock"
    lock_fd = open(lock_path, "w")
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lock_fd.write(str(os.getpid()))
        lock_fd.flush()
    except IOError:
        print("[ws] Another instance running. Exiting.", flush=True)
        sys.exit(1)

    asyncio.run(main())
