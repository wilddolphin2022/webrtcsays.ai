/*
 *  (c) 2025, wilddolphin2022
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2022
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <unistd.h>

#include <string>
#include <vector>
#include <climits>
#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <array>
#include <algorithm>
#include <cctype>
#include <regex>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>


#include "direct.h"
#include "option.h"
#include "client.h"
//#include "room.h"

static volatile bool g_shutdown = false;
static volatile int g_shutdown_count = 0;

// Signal handler for Ctrl+C
void signalHandler(int signal) {
    if (signal == SIGINT) {
        g_shutdown_count++;
        std::cout << "\nCtrl+C received (" << g_shutdown_count << "/3), shutting down...\n";
        g_shutdown = true;
        
        // Force immediate exit after 3 Ctrl+C presses to handle stuck scenarios
        if (g_shutdown_count >= 3) {
            std::cout << "Force exit after multiple Ctrl+C signals\n";
            std::cout.flush();
            _exit(1);  // Use _exit for immediate termination
        }
        
        // After 2nd Ctrl+C, be more aggressive
        if (g_shutdown_count >= 2) {
            std::cout << "Aggressive shutdown mode activated\n";
            std::cout.flush();
            // Set a 2-second timeout to force exit
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::cout << "Force exit due to timeout\n";
                std::cout.flush();
                _exit(1);
            }).detach();
        }
    }
}

namespace {

static std::string Base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8) |
                 static_cast<uint32_t>(data[i + 2]);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back(kB64[n & 63]);
    i += 3;
  }
  const size_t rem = len - i;
  if (rem == 1) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

static std::array<uint8_t, 20> Sha1Digest(const std::string& msg) {
  auto rol = [](uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };
  uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
  std::vector<uint8_t> data(msg.begin(), msg.end());
  data.push_back(0x80);
  while ((data.size() % 64) != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xEFCDAB89;
  uint32_t h2 = 0x98BADCFE;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xC3D2E1F0;

  for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      size_t o = chunk + i * 4;
      w[i] = (static_cast<uint32_t>(data[o]) << 24) |
             (static_cast<uint32_t>(data[o + 1]) << 16) |
             (static_cast<uint32_t>(data[o + 2]) << 8) |
             static_cast<uint32_t>(data[o + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0, k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<uint8_t, 20> out{};
  auto wr = [&out](int idx, uint32_t v) {
    out[idx] = static_cast<uint8_t>((v >> 24) & 0xff);
    out[idx + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
    out[idx + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
    out[idx + 3] = static_cast<uint8_t>(v & 0xff);
  };
  wr(0, h0);
  wr(4, h1);
  wr(8, h2);
  wr(12, h3);
  wr(16, h4);
  return out;
}

class DirectWebSocketProxy {
 public:
  DirectWebSocketProxy(std::string tcp_host, int tcp_port, int ws_port)
      : tcp_host_(std::move(tcp_host)), tcp_port_(tcp_port), ws_port_(ws_port) {}

  ~DirectWebSocketProxy() { Stop(); }

  bool Start() {
    if (running_.load()) return true;
    running_.store(true);
    server_thread_ = std::thread([this]() { ServeLoop(); });
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
      shutdown(listen_fd_, SHUT_RDWR);
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (server_thread_.joinable()) server_thread_.join();
  }

 private:
  static std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
  }

  static bool ReadExactly(int fd, void* out, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
      ssize_t r = recv(fd, p + got, n - got, 0);
      if (r <= 0) return false;
      got += static_cast<size_t>(r);
    }
    return true;
  }

  static std::string ComputeWebSocketAccept(const std::string& key) {
    static constexpr const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string in = key + kGuid;
    const auto digest = Sha1Digest(in);
    return Base64Encode(digest.data(), digest.size());
  }

  static bool SendWsTextFrame(int fd, const std::string& text) {
    std::vector<uint8_t> frame;
    frame.reserve(2 + 8 + text.size());
    frame.push_back(0x81);  // FIN + text
    const size_t len = text.size();
    if (len <= 125) {
      frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
      frame.push_back(126);
      frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
      frame.push_back(static_cast<uint8_t>(len & 0xff));
    } else {
      frame.push_back(127);
      for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xff));
    }
    frame.insert(frame.end(), text.begin(), text.end());
    return send(fd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
  }

  static bool SendWsControlFrame(int fd, uint8_t opcode, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(2 + payload.size());
    frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0f)));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0x7f));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return send(fd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
  }

  static bool ReadWsFrame(int fd, uint8_t& opcode, std::string& text_payload) {
    uint8_t hdr[2];
    if (!ReadExactly(fd, hdr, 2)) return false;
    opcode = static_cast<uint8_t>(hdr[0] & 0x0f);
    const bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = static_cast<uint64_t>(hdr[1] & 0x7f);
    if (len == 126) {
      uint8_t ex[2];
      if (!ReadExactly(fd, ex, 2)) return false;
      len = (static_cast<uint64_t>(ex[0]) << 8) | static_cast<uint64_t>(ex[1]);
    } else if (len == 127) {
      uint8_t ex[8];
      if (!ReadExactly(fd, ex, 8)) return false;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint64_t>(ex[i]);
    }
    std::array<uint8_t, 4> mask{0, 0, 0, 0};
    if (masked) {
      if (!ReadExactly(fd, mask.data(), mask.size())) return false;
    }
    if (len > (1u << 20)) return false;  // limit to 1 MiB
    std::vector<uint8_t> payload(static_cast<size_t>(len));
    if (len > 0 && !ReadExactly(fd, payload.data(), payload.size())) return false;
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    }
    text_payload.assign(payload.begin(), payload.end());
    return true;
  }

  static bool ExtractJsonString(const std::string& json, const std::string& key, std::string& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\\\\\"])*)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re) || m.size() < 2) return false;
    out = m[1].str();
    return true;
  }

  static bool ExtractJsonInt(const std::string& json, const std::string& key, int& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (!std::regex_search(json, m, re) || m.size() < 2) return false;
    out = std::atoi(m[1].str().c_str());
    return true;
  }

  static std::string JsonToDirectMessage(const std::string& in) {
    std::string type;
    if (!ExtractJsonString(in, "type", type)) return in;
    if (type == "offer" || type == "answer") {
      std::string sdp;
      ExtractJsonString(in, "sdp", sdp);
      if (sdp.empty()) return "";
      return (type == "offer" ? "OFFER:" : "ANSWER:") + sdp;
    }
    if (type == "ice") {
      std::string candidate;
      int mline = 0;
      ExtractJsonString(in, "candidate", candidate);
      ExtractJsonInt(in, "sdpMLineIndex", mline);
      if (candidate.empty()) return "";
      return "ICE:" + std::to_string(mline) + ":" + candidate;
    }
    if (type == "hangup") return "BYE";
    if (type == "cancel") return "CANCEL";
    if (type == "face") {
      std::string data;
      ExtractJsonString(in, "data", data);
      return data.empty() ? "" : ("FACE:" + data);
    }
    if (type == "raw") {
      std::string msg;
      ExtractJsonString(in, "message", msg);
      return msg;
    }
    return in;
  }

  static std::string MaybeJsonToDirect(const std::string& in) {
    std::string msg = Trim(in);
    if (msg.empty()) return msg;
    if (!msg.empty() && msg.front() == '{') return JsonToDirectMessage(msg);
    return msg;
  }

  int ConnectToDirectTcp() const {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(tcp_port_));
    if (inet_pton(AF_INET, tcp_host_.c_str(), &sa.sin_addr) != 1) {
      close(fd);
      return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  bool PerformHandshake(int client_fd) const {
    std::string req;
    std::array<char, 4096> buf{};
    while (req.find("\r\n\r\n") == std::string::npos) {
      ssize_t n = recv(client_fd, buf.data(), buf.size(), 0);
      if (n <= 0) return false;
      req.append(buf.data(), static_cast<size_t>(n));
      if (req.size() > (1u << 20)) return false;
    }

    std::istringstream iss(req);
    std::string line;
    std::string ws_key;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto pos = line.find(':');
      if (pos == std::string::npos) continue;
      std::string h = line.substr(0, pos);
      std::string v = Trim(line.substr(pos + 1));
      std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (h == "sec-websocket-key") ws_key = v;
    }
    if (ws_key.empty()) return false;
    const std::string accept = ComputeWebSocketAccept(ws_key);
    std::ostringstream out;
    out << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
    const std::string resp = out.str();
    return send(client_fd, resp.data(), resp.size(), 0) == static_cast<ssize_t>(resp.size());
  }

  void HandleClient(int client_fd) {
    if (!PerformHandshake(client_fd)) {
      close(client_fd);
      return;
    }

    int direct_fd = ConnectToDirectTcp();
    if (direct_fd < 0) {
      SendWsTextFrame(client_fd, "480 Temporarily Unavailable");
      close(client_fd);
      return;
    }

    std::vector<uint8_t> direct_buf;
    direct_buf.reserve(65536);
    while (running_.load() && !g_shutdown) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(client_fd, &rfds);
      FD_SET(direct_fd, &rfds);
      const int maxfd = std::max(client_fd, direct_fd);
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      const int sel = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
      if (sel < 0) break;
      if (sel == 0) continue;

      if (FD_ISSET(client_fd, &rfds)) {
        uint8_t opcode = 0;
        std::string ws_text;
        if (!ReadWsFrame(client_fd, opcode, ws_text)) break;
        if (opcode == 0x8) break;  // close
        if (opcode == 0x9) {       // ping
          std::vector<uint8_t> empty;
          SendWsControlFrame(client_fd, 0xA, empty);
          continue;
        }
        if (opcode != 0x1) continue;  // text only
        std::string direct_msg = MaybeJsonToDirect(ws_text);
        if (direct_msg.empty()) continue;
        if (direct_msg.size() > 65535) continue;
        uint8_t hdr[2] = {
            static_cast<uint8_t>((direct_msg.size() >> 8) & 0xff),
            static_cast<uint8_t>(direct_msg.size() & 0xff)};
        if (send(direct_fd, hdr, sizeof(hdr), 0) != static_cast<ssize_t>(sizeof(hdr))) break;
        if (send(direct_fd, direct_msg.data(), direct_msg.size(), 0) != static_cast<ssize_t>(direct_msg.size())) break;
      }

      if (FD_ISSET(direct_fd, &rfds)) {
        std::array<char, 65536> b{};
        const ssize_t n = recv(direct_fd, b.data(), b.size(), 0);
        if (n <= 0) break;
        direct_buf.insert(direct_buf.end(), b.begin(), b.begin() + n);
        while (direct_buf.size() >= 2) {
          const uint16_t mlen = (static_cast<uint16_t>(direct_buf[0]) << 8) |
                                static_cast<uint16_t>(direct_buf[1]);
          if (direct_buf.size() < static_cast<size_t>(2 + mlen)) break;
          std::string msg(direct_buf.begin() + 2, direct_buf.begin() + 2 + mlen);
          if (!SendWsTextFrame(client_fd, msg)) {
            close(direct_fd);
            close(client_fd);
            return;
          }
          direct_buf.erase(direct_buf.begin(), direct_buf.begin() + 2 + mlen);
        }
      }
    }

    close(direct_fd);
    close(client_fd);
  }

  void ServeLoop() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(ws_port_));
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    if (listen(listen_fd_, 16) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    std::cerr << "WebSocket signaling listening on ws://0.0.0.0:" << ws_port_
              << " -> " << tcp_host_ << ":" << tcp_port_ << "\n";

    while (running_.load() && !g_shutdown) {
      int cfd = accept(listen_fd_, nullptr, nullptr);
      if (cfd < 0) {
        if (!running_.load()) break;
        continue;
      }
      std::thread([this, cfd]() { HandleClient(cfd); }).detach();
    }
  }

  std::string tcp_host_;
  int tcp_port_ = 3456;
  int ws_port_ = 3457;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread server_thread_;
};

}  // namespace

int main(int argc, char* argv[]) {

  Options opts;
  std::string options;
  if (argc == 1) {
    opts.help = true;
  } else {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const auto& piece : args)
      options += (piece + " ");
    opts = parseOptions(options.c_str());
  }

  if (opts.help) {
    auto usage = opts.help_string;
    // Print usage to stderr instead for command-line tools
    fprintf(stderr, "%s\n", usage.c_str());
    return 1;
  }

  // Install signal handler for Ctrl+C
  signal(SIGINT, signalHandler);

  //DirectSetLoggingLevel(AS_INFO);
  DirectApplication::rtcInitialize();

  // Parse server address and room from options
  std::string room_name = opts.room_name.empty() ? "room101" : opts.room_name;  // Use provided room or default
  // Ensure opts.room_name is set so that callee/caller register correctly
  opts.room_name = room_name;
   
  // Validate required parameters for name-based calling
  if (opts.user_name.empty()) {
    fprintf(stderr, "Error: --user_name is required for name-based calling\n");
    return 1;
  }
  
  if (opts.mode == "caller" && opts.target_name.empty()) {
    fprintf(stderr, "Error: --target_name is required when mode is caller\n");
    return 1;
  }

  std::shared_ptr<DirectCalleeClient> callee;
  std::shared_ptr<DirectCallerClient> caller;
  std::unique_ptr<DirectWebSocketProxy> ws_proxy;

  if (opts.websocket_signaling && (opts.mode == "callee" || opts.mode == "both")) {
    std::string callee_ip;
    int callee_port = 3456;
    if (!opts.address.empty()) {
      std::string parsed_ip;
      int parsed_port = 0;
      if (ParseIpAndPort(opts.address, parsed_ip, parsed_port) && parsed_port > 0) {
        callee_port = parsed_port;
      }
    }
    ws_proxy = std::make_unique<DirectWebSocketProxy>("127.0.0.1", callee_port, opts.websocket_port);
    ws_proxy->Start();
  }
  
  if (opts.mode == "callee" or opts.mode == "both") {
    int session_count = 0;
    while (!g_shutdown) {
      if (!callee) {
        fprintf(stderr, "Callee loop entered, g_shutdown=%d\n", g_shutdown);
        session_count++;
        fprintf(stderr, "Starting callee session #%d\n", session_count);
        callee = std::make_shared<DirectCalleeClient>(opts);

        if (!callee->Initialize()) {
          fprintf(stderr, "failed to initialize callee\n");
          return 1;
        }
        if (!callee->StartListening()) {
          fprintf(stderr, "Failed to start listening\n");
          return 1;
        }
        callee->RunOnBackgroundThread();

        // Preload AI model files into OS page cache on a background thread
        // so the first call doesn't stall on disk I/O
        if (session_count == 1) {
          std::thread([&opts]() {
            auto preload = [](const std::string& path, const char* name) {
              if (path.empty()) return;
              FILE* f = fopen(path.c_str(), "rb");
              if (!f) return;
              char buf[1 << 20];
              while (fread(buf, 1, sizeof(buf), f) > 0) {}
              fclose(f);
              fprintf(stderr, "Preloaded %s into page cache: %s\n", name, path.c_str());
            };
            preload(opts.llama_model, "llama");
            preload(opts.whisper_model, "whisper");
          }).detach();
        }
      }

      // Prepare new session and wait for CANCEL or Ctrl+C
      callee->ResetConnectionClosedEvent();
      fprintf(stderr, "Callee ready for incoming connections in room %s...\n", room_name.c_str());
      
      while (!g_shutdown) {
        // Check shutdown more frequently with shorter timeout
        if (callee->WaitUntilConnectionClosed(200)) {
          fprintf(stderr, "Callee session #%d ended (connection closed/failed), restarting listener\n", session_count);
          break;
        }
        
        // Check shutdown flag multiple times per second
        if (g_shutdown) {
          fprintf(stderr, "Shutdown requested during callee session #%d\n", session_count);
          break;
        }
      }

      if (!g_shutdown) {
        fprintf(stderr, "Keeping callee listener alive for next connection\n");
        callee->ResetConnectionClosedEvent();
        continue;
      }

      // Signal quit immediately if shutdown is requested
      if (g_shutdown && callee) {
        fprintf(stderr, "Shutdown requested - signaling callee quit\n");
        callee->SignalQuit();
      }
      
      // Always signal internal threads to quit before destroying the object
      if (callee) {
        fprintf(stderr, "Signaling callee quit (session teardown)\n");
        callee->SignalQuit();
      }
      
      callee.reset();
      
      if (!g_shutdown) {
        fprintf(stderr, "Preparing to restart callee in 2 seconds...\n");
        // Check for shutdown during sleep as well
        for (int i = 0; i < 20 && !g_shutdown; i++) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    int caller_session_count = 0;
    while (!g_shutdown) {
      caller_session_count++;
      fprintf(stderr, "Starting caller session #%d\n", caller_session_count);
      
      caller = std::make_shared<DirectCallerClient>(opts);
      
      // Set the target user to call
      if (!opts.target_name.empty()) {
        caller->SetTargetUser(opts.target_name);
        fprintf(stderr, "Caller will target user: %s\n", opts.target_name.c_str());
      }
      
      if (!caller->Initialize()) {
        fprintf(stderr, "failed to initialize caller\n");
        return 1;
      }
      if (!caller->Connect()) {
        fprintf(stderr, "failed to connect caller to room %s\n", room_name.c_str());
        return 1;
      }
      caller->RunOnBackgroundThread();
      fprintf(stderr, "Caller connected to room %s\n", room_name.c_str());
      
      // Wait for connection to close or shutdown signal
      while (!g_shutdown) {
        // Check shutdown more frequently with shorter timeout
        if (caller->WaitUntilConnectionClosed(200)) {
          fprintf(stderr, "Caller session #%d ended (connection closed/failed), restarting connection\n", caller_session_count);
          break;
        }
        
        // Check shutdown flag multiple times per second
        if (g_shutdown) {
          fprintf(stderr, "Shutdown requested during caller session #%d\n", caller_session_count);
          break;
        }
      }
      
      // Signal quit immediately if shutdown is requested
      if (g_shutdown && caller) {
        fprintf(stderr, "Shutdown requested - signaling caller quit\n");
        caller->Disconnect();
      }
      
      caller.reset();
      
      if (!g_shutdown) {
        fprintf(stderr, "Preparing to restart caller in 2 seconds...\n");
        // Check for shutdown during sleep as well
        for (int i = 0; i < 20 && !g_shutdown; i++) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
  }

  // Both callee and caller now have their own loops, so we just wait for shutdown
  if (opts.mode != "caller" && opts.mode != "callee" && opts.mode != "both") {
    while (!g_shutdown) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      
      // Additional shutdown check for stuck scenarios
      if (g_shutdown_count >= 2) {
        fprintf(stderr, "Aggressive shutdown - breaking main loop\n");
        break;
      }
    }
  }

  // Cleanup phase
  fprintf(stderr, "Starting cleanup...\n");
  
  if(opts.mode == "caller" or opts.mode == "both") {
    if (caller) {
      caller->Disconnect();
      // Shorter timeout for cleanup, don't wait too long
      int cleanup_timeout = g_shutdown_count >= 2 ? 1000 : 5000;
      if (caller->WaitUntilConnectionClosed(cleanup_timeout)) {
        fprintf(stderr, "Caller connection closed\n");
      } else {
        fprintf(stderr, "Caller connection not closed after timeout\n");
      }
    }
  }

  if(opts.mode == "callee" or opts.mode == "both") {
    if (callee) {
      fprintf(stderr, "Signaling callee quit...\n");
      callee->SignalQuit();
      callee.reset();
      fprintf(stderr, "Callee cleanup complete\n");
    }
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    if (caller) {
      fprintf(stderr, "Signaling caller quit...\n");
      caller->Disconnect();
      caller.reset();
      fprintf(stderr, "Caller cleanup complete\n");
    }
  }

  // Allow some time for threads to process quit, but not too long in aggressive mode
  int sleep_time = g_shutdown_count >= 2 ? 10000 : 50000;
  fprintf(stderr, "Waiting %d microseconds for thread cleanup...\n", sleep_time);
  usleep(sleep_time);

  DirectApplication::rtcCleanup();
  if (ws_proxy) {
    ws_proxy->Stop();
  }
  return 0;
}
