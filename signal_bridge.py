#!/usr/bin/env python3
"""
WebRTCsays.ai Signaling Bridge

Bridges the PHP signaling relay (HTTP polling) to the directcall callee (TCP).
Run this alongside directcall on the same machine.

Usage:
    python3 signal_bridge.py [--signal-url URL] [--callee HOST:PORT] [--room ROOM]

Flow:
    Browser -> PHP relay (HTTP) -> this bridge -> directcall (TCP)
    Browser <- PHP relay (HTTP) <- this bridge <- directcall (TCP)
"""

import argparse
import json
import socket
import threading
import time
import sys
import urllib.request
import urllib.error

RAW_ANSWER_PATH = "public_last_answer_raw.sdp"
NORMALIZED_ANSWER_PATH = "public_last_answer_normalized.sdp"
ICE_LOG_PATH = "public_last_ice.jsonl"
STATUS_PREFIXES = ("200 ", "400 ", "480 ", "486 ")

class SignalBridge:
    def __init__(self, signal_url, callee_host, callee_port, room):
        self.signal_url = signal_url.rstrip('/')
        self.callee_host = callee_host
        self.callee_port = callee_port
        self.room = room
        self.sock = None
        self.running = False
        self.recv_buffer = b""
        self.offer_sent = False
        self.ice_buffer = []
        self.handshake_active = False
        self.state_lock = threading.Lock()
        self.recv_lock = threading.Lock()

    def set_handshake_active(self, val):
        with self.state_lock:
            self.handshake_active = val
            
    def is_handshake_active(self):
        with self.state_lock:
            return self.handshake_active

    def normalize_sdp(self, sdp):
        lines = sdp.replace('\r\n', '\n').replace('\r', '\n').split('\n')
        lines = [line for line in lines if line != ""]
        return '\r\n'.join(lines) + '\r\n'

    def write_debug_file(self, path, content):
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(content)
        except Exception as e:
            pass

    def append_debug_line(self, path, content):
        try:
            with open(path, "a", encoding="utf-8") as f:
                f.write(content + "\n")
        except Exception as e:
            pass

    def http_request(self, action, role, data=None):
        url = f"{self.signal_url}?action={action}&role={role}&room={self.room}"
        req = urllib.request.Request(url)
        if data is not None:
            req.add_header('Content-Type', 'application/json')
            req.data = json.dumps(data).encode('utf-8')
        try:
            with urllib.request.urlopen(req, timeout=5) as response:
                return json.loads(response.read().decode('utf-8'))
        except Exception as e:
            print(f"[bridge] HTTP error for {action}: {e}", flush=True)
            return None

    def close_callee_socket(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None
        self.recv_buffer = b""

    def connect_to_callee(self):
        self.close_callee_socket()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        try:
            s.connect((self.callee_host, self.callee_port))
            s.settimeout(0.1)
            self.sock = s
            print(f"[bridge] Connected to directcall at {self.callee_host}:{self.callee_port}", flush=True)
            return True
        except Exception as e:
            print(f"[bridge] Connect failed: {e}", flush=True)
            return False

    def send_to_callee(self, msg):
        if not self.sock:
            return False
        try:
            payload = msg.encode()
            header = len(payload).to_bytes(2, 'big')
            self.sock.sendall(header + payload)
            print(f"[bridge] -> callee ({len(payload)}B): {msg[:80]}", flush=True)
            return True
        except Exception as e:
            print(f"[bridge] Send error: {e}", flush=True)
            return False

    def recv_from_callee(self):
        if not self.sock:
            return []
        self.recv_lock.acquire()
        try:
            data = self.sock.recv(16384)
            if data:
                self.recv_buffer += data
        except socket.timeout:
            pass
        except Exception:
            self.recv_lock.release()
            return []

        messages = []
        while len(self.recv_buffer) >= 2:
            pkt_len = int.from_bytes(self.recv_buffer[0:2], 'big')
            if pkt_len == 0 or pkt_len > 65535:
                self.recv_buffer = self.recv_buffer[1:]
                continue
            if len(self.recv_buffer) < 2 + pkt_len:
                break
            msg = self.recv_buffer[2:2+pkt_len].decode('utf-8', errors='replace').strip()
            self.recv_buffer = self.recv_buffer[2+pkt_len:]
            if msg:
                messages.append(msg)
        self.recv_lock.release()
        return messages

    def enqueue_error(self, stage, status_msg):
        self.http_request('post', 'callee', {
            "type": "status",
            "stage": stage,
            "status": status_msg
        })

    def protocol_send_with_ack(self, send_msg, expected, timeout_s, stage):
        self.send_to_callee(send_msg)
        response = self.wait_for_response(expected, timeout_s)
        if not response:
            self.enqueue_error(stage, f"timeout waiting for {expected}")
            return None
        if expected in response:
            return response
        self.enqueue_error(stage, response)
        return None

    def wait_for_response(self, expected, timeout_s=5):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            msgs = self.recv_from_callee()
            for m in msgs:
                print(f"[bridge] <- callee: {m[:60]}", flush=True)
                if expected in m:
                    return m
                if m.startswith(STATUS_PREFIXES):
                    return m
                self.handle_callee_message(m)
            time.sleep(0.05)
        print(f"[bridge] Timeout waiting for '{expected}'", flush=True)
        return None

    def handle_browser_offer(self, sdp):
        """Browser sent SDP offer -> strict protocol handshake"""
        self.offer_sent = False
        self.ice_buffer.clear()
        self.set_handshake_active(True)
        try:
            hello_ok = False
            for attempt in range(1, 11):
                if not self.connect_to_callee():
                    time.sleep(0.2)
                    continue
                hello_response = self.protocol_send_with_ack("HELLO", "200 OK", 1.5, "HELLO")
                if hello_response and hello_response.startswith("200 OK"):
                    hello_ok = True
                    break
                if hello_response:
                    print(f"[bridge] [warn] HELLO attempt {attempt} got '{hello_response[:60]}', retrying", flush=True)
                self.close_callee_socket()
                time.sleep(0.25)

            if not hello_ok:
                print("[bridge] Could not get 200 OK from callee; aborting offer", flush=True)
                return

            invite = json.dumps({"agent": "audio", "encryption": True, "video": True, "room_name": self.room})
            waiting_response = self.protocol_send_with_ack(f"INVITE:{invite}", "WAITING", 5, "INVITE")
            if not waiting_response or not waiting_response.startswith("WAITING"):
                print("[bridge] INVITE did not reach WAITING state; aborting offer", flush=True)
                self.close_callee_socket()
                return

            self.send_to_callee(f"OFFER:{sdp}")
            self.offer_sent = True
            for c in self.ice_buffer:
                self.send_to_callee(c)
            self.ice_buffer.clear()
        finally:
            self.set_handshake_active(False)

    def handle_browser_ice(self, candidate):
        """Browser sent ICE candidate"""
        if isinstance(candidate, dict):
            sdp_str = candidate.get('candidate', '')
            mline = candidate.get('sdpMLineIndex', 0)
            if sdp_str:
                msg = f"ICE:{mline}:{sdp_str}"
                if self.offer_sent:
                    self.send_to_callee(msg)
                else:
                    self.ice_buffer.append(msg)

    def handle_browser_hangup(self):
        self.offer_sent = False
        self.ice_buffer.clear()
        if not self.sock:
            return
        self.send_to_callee("BYE")
        self.wait_for_response("200 OK", 1.0)
        self.close_callee_socket()
        print("[bridge] Browser hangup -> callee BYE + socket close", flush=True)

    def handle_browser_cancel(self):
        self.offer_sent = False
        self.ice_buffer.clear()
        if not self.sock:
            return
        self.send_to_callee("CANCEL")
        self.wait_for_response("BYE", 1.0)
        self.close_callee_socket()
        print("[bridge] Browser cancel -> callee CANCEL + socket close", flush=True)

    def handle_browser_whisper(self, language, text):
        if not self.sock:
            self.enqueue_error("WHISPER", "no active callee socket")
            return
        lang = (language or "en").strip()
        self.send_to_callee(f"WHISPER[{lang}]{text}")

    def handle_callee_message(self, msg):
        """directcall sent a message -> forward to browser via PHP relay"""
        if msg.startswith("200 OK") or msg.startswith("WAITING"):
            print(f"[bridge] <- callee: {msg}", flush=True)
            return

        if msg.startswith("ANSWER:"):
            sdp = msg[7:]
            normalized_sdp = self.normalize_sdp(sdp)
            self.write_debug_file(RAW_ANSWER_PATH, sdp)
            self.write_debug_file(NORMALIZED_ANSWER_PATH, normalized_sdp)
            self.http_request('post', 'callee', {'type': 'answer', 'sdp': normalized_sdp})
            print(f"[bridge] Forwarded SDP answer to browser ({len(normalized_sdp)} bytes)", flush=True)

        elif msg.startswith("ICE:"):
            parts = msg[4:].split(":", 1)
            if len(parts) == 2:
                mline = int(parts[0])
                candidate_str = parts[1]
                self.http_request('post', 'callee', {
                    'type': 'ice',
                    'candidate': {
                        'candidate': candidate_str,
                        'sdpMLineIndex': mline,
                        'sdpMid': str(mline)
                    }
                })
        elif msg.startswith(STATUS_PREFIXES):
            self.http_request('post', 'callee', {
                "type": "status",
                "stage": "callee",
                "status": msg
            })
            print(f"[bridge] <- callee status: {msg}", flush=True)
        elif msg.startswith("LLAMA["):
            self.http_request('post', 'callee', {
                "type": "llama",
                "text": msg
            })
            print(f"[bridge] <- callee llama: {msg[:60]}", flush=True)
        else:
            print(f"[bridge] <- callee (ignored): {msg[:60]}", flush=True)

    def poll_loop(self):
        """Poll PHP relay for browser messages"""
        while self.running:
            data = self.http_request('poll', 'callee') # poll for 'caller' messages
            if data and data.get('messages'):
                for msg in data['messages']:
                    msg_type = msg.get('type')
                    if msg_type == 'offer':
                        print(f"[bridge] Browser offer received", flush=True)
                        threading.Thread(target=self.handle_browser_offer, args=(msg['sdp'],), daemon=True).start()
                    elif msg_type == 'ice':
                        self.handle_browser_ice(msg.get('candidate'))
                    elif msg_type == 'hangup':
                        self.handle_browser_hangup()
                    elif msg_type == 'cancel':
                        self.handle_browser_cancel()
                    elif msg_type == 'whisper':
                        self.handle_browser_whisper(msg.get('language', 'en'), msg.get('text', ''))
                    elif msg_type == 'face':
                        if self.sock:
                            self.send_to_callee(f"FACE:{msg.get('data', '')}")
                            print("[bridge] Forwarded FACE image to callee", flush=True)
            time.sleep(0.3)

    def recv_loop(self):
        """Read messages from directcall TCP"""
        while self.running:
            if self.is_handshake_active():
                time.sleep(0.05)
                continue
            messages = self.recv_from_callee()
            for msg in messages:
                self.handle_callee_message(msg)
            time.sleep(0.05)

    def run(self):
        print(f"[bridge] Signal URL: {self.signal_url}", flush=True)
        print(f"[bridge] Callee: {self.callee_host}:{self.callee_port}", flush=True)
        print(f"[bridge] Room: {self.room}", flush=True)

        self.http_request('reset', 'callee')
        self.running = True
        print("[bridge] Waiting for browser to call (no TCP connection until offer arrives)", flush=True)

        poll_thread = threading.Thread(target=self.poll_loop, daemon=True)
        recv_thread = threading.Thread(target=self.recv_loop, daemon=True)
        poll_thread.start()
        recv_thread.start()

        print(f"[bridge] Ready. Open https://www.wilddolphin.us/webrtcsays-demo.html?room={self.room}", flush=True)
        print("[bridge] Press Ctrl+C to stop", flush=True)

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n[bridge] Shutting down", flush=True)
            self.running = False
            self.close_callee_socket()

def main():
    parser = argparse.ArgumentParser(description='WebRTCsays.ai signaling bridge')
    parser.add_argument('--signal-url', default='https://www.wilddolphin.us/signal.php',
                        help='URL of the PHP signaling relay')
    parser.add_argument('--callee', default='127.0.0.1:3456',
                        help='directcall callee address (host:port)')
    parser.add_argument('--room', default='room101',
                        help='Room name for signaling')
    args = parser.parse_args()

    host, port = args.callee.rsplit(':', 1)
    bridge = SignalBridge(args.signal_url, host, int(port), args.room)
    bridge.run()

if __name__ == '__main__':
    main()
