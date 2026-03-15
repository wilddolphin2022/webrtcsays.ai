#!/usr/bin/env python3
"""Local HTTP signal relay + bridge to directcall.
Serves the demo page AND handles signaling - all in one process."""

import http.server
import json
import os
import socket
import threading
import time
import struct

PORT = 38081
CALLEE_HOST = "127.0.0.1"
CALLEE_PORT = 3456
DEFAULT_ROOM = "room-local-avatar"
RAW_ANSWER_PATH = "last_answer_raw.sdp"
NORMALIZED_ANSWER_PATH = "last_answer_normalized.sdp"

messages = {}
lock = threading.Lock()
callee_sock = None
callee_buf = b""
offer_sent = False
ice_buffer = []
recv_lock = threading.Lock()
active_room = DEFAULT_ROOM
STATUS_PREFIXES = ("200 ", "400 ", "480 ", "486 ")
handshake_active = False
state_lock = threading.Lock()

def room_messages(room):
    room = room or DEFAULT_ROOM
    if room not in messages:
        messages[room] = {"caller": [], "callee": []}
    return messages[room]

def enqueue_message(room, role, payload):
    with lock:
        room_messages(room)[role].append(payload)

def drain_messages(room, role):
    with lock:
        queue = room_messages(room)[role]
        drained = list(queue)
        room_messages(room)[role] = []
    return drained

def reset_messages(room=None):
    with lock:
        if room:
            messages[room] = {"caller": [], "callee": []}
        else:
            messages.clear()

def normalize_sdp(sdp):
    # Browsers are happiest with CRLF-delimited SDP lines.
    lines = sdp.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    lines = [line for line in lines if line != ""]
    return '\r\n'.join(lines) + '\r\n'

def write_debug_file(path, content):
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
    except Exception as e:
        print(f"[signal] Failed to write {path}: {e}", flush=True)

def send_to_callee(msg):
    global callee_sock
    if not callee_sock:
        return False
    try:
        payload = msg.encode()
        header = len(payload).to_bytes(2, 'big')
        callee_sock.sendall(header + payload)
        print(f"  -> callee ({len(payload)}B): {msg[:70]}")
        return True
    except Exception as e:
        print(f"  -> callee SEND ERROR: {e}")
        return False

def close_callee_socket():
    global callee_sock, callee_buf
    if callee_sock:
        try:
            callee_sock.close()
        except Exception:
            pass
    callee_sock = None
    callee_buf = b""

def recv_from_callee():
    global callee_sock, callee_buf
    if not callee_sock:
        return []
    recv_lock.acquire()
    try:
        data = callee_sock.recv(16384)
        if data:
            callee_buf += data
    except socket.timeout:
        pass
    except Exception:
        recv_lock.release()
        return []

    msgs = []
    while len(callee_buf) >= 2:
        pkt_len = int.from_bytes(callee_buf[0:2], 'big')
        if pkt_len == 0 or pkt_len > 65535:
            callee_buf = callee_buf[1:]
            continue
        if len(callee_buf) < 2 + pkt_len:
            break
        msg = callee_buf[2:2+pkt_len].decode('utf-8', errors='replace').strip()
        callee_buf = callee_buf[2+pkt_len:]
        if msg:
            msgs.append(msg)
    recv_lock.release()
    return msgs

def connect_callee():
    global callee_sock
    close_callee_socket()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect((CALLEE_HOST, CALLEE_PORT))
        s.settimeout(0.1)
        callee_sock = s
        print(f"[signal] Connected to directcall at {CALLEE_HOST}:{CALLEE_PORT}")
        return True
    except Exception as e:
        print(f"[signal] Connect to directcall failed: {e}")
        return False

def set_handshake_active(value):
    global handshake_active
    with state_lock:
        handshake_active = value

def is_handshake_active():
    with state_lock:
        return handshake_active

def enqueue_error(room, stage, status_or_error):
    enqueue_message(room, "callee", {
        "type": "status",
        "stage": stage,
        "status": status_or_error
    })

def protocol_send_with_ack(send_msg, expected, timeout_s, room, stage):
    send_to_callee(send_msg)
    response = wait_for_response(expected, timeout_s)
    if not response:
        enqueue_error(room, stage, f"timeout waiting for {expected}")
        return None
    if expected in response:
        return response
    enqueue_error(room, stage, response)
    return None

def handle_browser_offer(room, sdp):
    global offer_sent, ice_buffer, active_room
    offer_sent = False
    ice_buffer = []
    active_room = room or DEFAULT_ROOM
    set_handshake_active(True)

    try:
        # Retry HELLO handshake so immediate redial does not fail when callee is
        # still finishing teardown from the prior call.
        hello_ok = False
        for attempt in range(1, 11):
            if not connect_callee():
                time.sleep(0.2)
                continue
            hello_response = protocol_send_with_ack("HELLO", "200 OK", 1.5, active_room, "HELLO")
            if hello_response and hello_response.startswith("200 OK"):
                hello_ok = True
                break
            if hello_response:
                print(f"  [warn] HELLO attempt {attempt} got '{hello_response[:60]}', retrying")
            close_callee_socket()
            time.sleep(0.25)

        if not hello_ok:
            print("[signal] Could not get 200 OK from callee; aborting offer")
            return

        invite = json.dumps({"agent":"audio","encryption":True,"video":True,"room_name":room or DEFAULT_ROOM})
        waiting_response = protocol_send_with_ack(
            f"INVITE:{invite}", "WAITING", 5, active_room, "INVITE"
        )
        if not waiting_response or not waiting_response.startswith("WAITING"):
            print("[signal] INVITE did not reach WAITING state; aborting offer")
            close_callee_socket()
            return

        send_to_callee(f"OFFER:{sdp}")
        offer_sent = True
        for c in ice_buffer:
            send_to_callee(c)
        ice_buffer = []
    finally:
        set_handshake_active(False)

def wait_for_response(expected, timeout_s):
    """Block until callee sends the expected response"""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        msgs = recv_from_callee()
        for m in msgs:
            print(f"  <- callee: {m[:60]}")
            if expected in m:
                return m
            if m.startswith(STATUS_PREFIXES):
                return m
            # Queue non-matching messages for the recv thread
            handle_callee_msg_internal(m)
        time.sleep(0.05)
    print(f"  [warn] Timeout waiting for '{expected}'")
    return None

def handle_callee_msg_internal(msg):
    """Process a callee message received during wait_for_response"""
    if msg.startswith("ANSWER:"):
        sdp = msg[7:]
        normalized_sdp = normalize_sdp(sdp)
        write_debug_file(RAW_ANSWER_PATH, sdp)
        write_debug_file(NORMALIZED_ANSWER_PATH, normalized_sdp)
        for line in sdp.split('\n'):
            if 'candidate' in line.lower():
                print(f"  SDP candidate: {line.strip()}")
        enqueue_message(active_room, "callee", {"type":"answer","sdp":normalized_sdp})
        print(f"  <- callee: ANSWER ({len(sdp)} bytes raw, {len(normalized_sdp)} bytes normalized) -> queued for browser", flush=True)
    elif msg.startswith("ICE:"):
        parts = msg[4:].split(":", 1)
        if len(parts) == 2:
            mline = int(parts[0])
            enqueue_message(active_room, "callee", {
                "type":"ice",
                "candidate":{"candidate":parts[1],"sdpMLineIndex":mline,"sdpMid":str(mline)}
            })
            print(f"  <- callee: ICE -> queued for browser")
    elif msg.startswith(STATUS_PREFIXES):
        enqueue_message(active_room, "callee", {
            "type": "status",
            "stage": "callee",
            "status": msg
        })
    elif msg.startswith("LLAMA["):
        enqueue_message(active_room, "callee", {
            "type": "llama",
            "text": msg
        })

def handle_browser_ice(candidate):
    global offer_sent, ice_buffer
    if isinstance(candidate, dict):
        sdp_str = candidate.get('candidate', '')
        mline = candidate.get('sdpMLineIndex', 0)
        if sdp_str:
            msg = f"ICE:{mline}:{sdp_str}"
            if offer_sent:
                send_to_callee(msg)
            else:
                ice_buffer.append(msg)

def handle_browser_cancel():
    global offer_sent, ice_buffer
    offer_sent = False
    ice_buffer = []
    if not callee_sock:
        return
    send_to_callee("CANCEL")
    wait_for_response("BYE", 1.0)
    close_callee_socket()
    print("[signal] Browser cancel -> callee CANCEL + socket close")

def handle_browser_hangup():
    global offer_sent, ice_buffer
    offer_sent = False
    ice_buffer = []
    if not callee_sock:
        return
    send_to_callee("BYE")
    wait_for_response("200 OK", 1.0)
    close_callee_socket()
    print("[signal] Browser hangup -> callee BYE + socket close")

def handle_browser_whisper(language, text):
    if not callee_sock:
        enqueue_error(active_room, "WHISPER", "no active callee socket")
        return
    lang = (language or "en").strip()
    send_to_callee(f"WHISPER[{lang}]{text}")

def callee_recv_thread():
    while True:
        if is_handshake_active():
            time.sleep(0.05)
            continue
        msgs = recv_from_callee()
        if msgs:
            print(f"  [recv] Got {len(msgs)} message(s)")
        for msg in msgs:
            if msg.startswith("200 OK") or msg.startswith("WAITING"):
                print(f"  <- callee: {msg}")
            elif msg.startswith("ANSWER:"):
                sdp = msg[7:]
                normalized_sdp = normalize_sdp(sdp)
                write_debug_file(RAW_ANSWER_PATH, sdp)
                write_debug_file(NORMALIZED_ANSWER_PATH, normalized_sdp)
                for line in sdp.split('\n'):
                    if 'candidate' in line.lower():
                        print(f"  SDP candidate: {line.strip()}")
                enqueue_message(active_room, "callee", {"type":"answer","sdp":normalized_sdp})
                print(f"  <- callee: ANSWER ({len(sdp)} bytes raw, {len(normalized_sdp)} bytes normalized) -> queued for browser", flush=True)
            elif msg.startswith("ICE:"):
                parts = msg[4:].split(":", 1)
                if len(parts) == 2:
                    mline = int(parts[0])
                    enqueue_message(active_room, "callee", {
                        "type":"ice",
                        "candidate":{"candidate":parts[1],"sdpMLineIndex":mline,"sdpMid":str(mline)}
                    })
                    print(f"  <- callee: ICE -> queued for browser")
            elif msg.startswith(STATUS_PREFIXES):
                enqueue_message(active_room, "callee", {
                    "type": "status",
                    "stage": "callee",
                    "status": msg
                })
                print(f"  <- callee status: {msg}")
            elif msg.startswith("LLAMA["):
                enqueue_message(active_room, "callee", {
                    "type": "llama",
                    "text": msg
                })
                print(f"  <- callee llama: {msg[:60]}")
            else:
                print(f"  <- callee (other): {msg[:60]}")
        time.sleep(0.05)

class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/' or self.path.startswith('/local-demo'):
            page_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'local-demo.html')
            try:
                with open(page_path, 'rb') as f:
                    body = f.read()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as e:
                self.send_response(500)
                self.send_header('Content-Type', 'text/plain; charset=utf-8')
                self.end_headers()
                self.wfile.write(f'Failed to load demo page: {e}'.encode('utf-8'))
            return
        if '/signal' not in self.path:
            self.send_response(404)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.end_headers()
            self.wfile.write(b'Not Found')
            return
        
        from urllib.parse import urlparse, parse_qs
        params = parse_qs(urlparse(self.path).query)
        action = params.get('action', [''])[0]
        role = params.get('role', [''])[0]
        room = params.get('room', [DEFAULT_ROOM])[0]

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()

        if action == 'poll':
            other = 'callee' if role == 'caller' else 'caller'
            msgs = drain_messages(room, other)
            self.wfile.write(json.dumps({"messages": msgs}).encode())
        elif action == 'reset':
            reset_messages(room)
            self.wfile.write(b'{"ok":true}')
        elif action == 'status':
            with lock:
                room_state = room_messages(room)
            self.wfile.write(json.dumps({
                "room": room,
                "caller": len(room_state.get("caller",[])),
                "callee": len(room_state.get("callee",[]))
            }).encode())
        else:
            self.wfile.write(b'{"ok":true}')

    def do_POST(self):
        from urllib.parse import urlparse, parse_qs
        params = parse_qs(urlparse(self.path).query)
        action = params.get('action', [''])[0]
        role = params.get('role', [''])[0]
        room = params.get('room', [DEFAULT_ROOM])[0]
        
        length = int(self.headers.get('Content-Length', 0))
        body = json.loads(self.rfile.read(length)) if length else {}

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()

        if action == 'post' and role == 'caller':
            msg_type = body.get('type')
            if msg_type == 'offer':
                print(f"[signal] Browser OFFER received ({len(body.get('sdp',''))} bytes)")
                threading.Thread(target=handle_browser_offer, args=(room, body['sdp']), daemon=True).start()
            elif msg_type == 'ice':
                handle_browser_ice(body.get('candidate'))
            elif msg_type == 'hangup':
                handle_browser_hangup()
            elif msg_type == 'cancel':
                handle_browser_cancel()
            elif msg_type == 'whisper':
                handle_browser_whisper(body.get('language', 'en'), body.get('text', ''))

        self.wfile.write(b'{"ok":true}')

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET,POST,OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def log_message(self, fmt, *args):
        if '/signal' in str(args):
            return  # quiet polling noise

if __name__ == '__main__':
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    threading.Thread(target=callee_recv_thread, daemon=True).start()
    print(f"[signal] Local server at http://localhost:{PORT}")
    print(f"[signal] Open http://localhost:{PORT} and click 'Call AI'")
    http.server.HTTPServer(('', PORT), Handler).serve_forever()
