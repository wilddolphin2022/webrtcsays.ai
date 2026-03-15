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

#pragma once

#include <fcntl.h>
#include <json/json.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <memory>

#include "direct.h"

class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
public:
    struct Config {
        std::string host;
        std::string port;
        bool use_ssl = false;
        std::string cert_file_path;
        std::string key_file_path;
        std::map<std::string, std::string> headers;
        int timeout_ms = 10000;
    };

    WebSocketClient();
    ~WebSocketClient();

    // Connection management
    bool connect(const Config& config);
    void disconnect();
    bool is_connected() const;

    // Returns true if listening loop is currently active (ping + async_read)
    bool is_running() const { return running_.load(); }

    // Message handling
    bool send_message(const std::string& message);
    void set_message_callback(std::function<void(const std::string&)> callback);
    void set_reconnect_callback(std::function<void()> cb) { reconnect_callback_ = std::move(cb); }
    void start_listening();
    void stop_listening();

    // WebSocket utilities
    std::string generate_websocket_key();
    std::string base64_encode(const std::string& in);

    // SSL context management
    void set_network_thread(rtc::Thread* thread);

    // Ping/Pong handling
    void send_ping();
    void send_pong_frame(const std::vector<unsigned char>& ping_payload);

    // Enable or disable automatic reconnect logic
    void set_allow_reconnect(bool value) { allow_reconnect_ = value; }

private:
    // Connection state
    int sockfd_;
    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    bool use_ssl_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<bool> read_in_progress_;

    // If set to false (e.g., after disconnect()), no further reconnect attempts will be made.
    std::atomic<bool> allow_reconnect_;

    // Track how many successive reconnect attempts have been made without success
    std::atomic<int> reconnect_attempts_{0};
    // Maximum number of successive reconnect attempts before giving up
    static constexpr int kMaxReconnectAttempts = 10;

    // Configuration
    Config config_;
    rtc::Thread* network_thread_;

    // Message handling
    std::string buffer_;
    std::mutex buffer_mutex_;
    std::function<void(const std::string&)> message_callback_;

    // Threading
    std::mutex ssl_mutex_;
    std::mutex thread_mutex_;

    // Internal methods
    bool create_socket_connection();
    bool setup_ssl_context();
    bool perform_ssl_handshake();
    bool send_http_handshake();
    
public:
    bool send_websocket_handshake_with_headers();
    
private:
    
    // Frame handling
    bool send_websocket_frame(const std::string& message);
    std::vector<std::string> process_websocket_frames();
    
    // Async operations
    void async_read();
    void force_sync_read(int timeout_ms = 5000);
    void attempt_reconnect();
    void attempt_reconnect_internal();

    // Utility methods
    void cleanup_connection();
    void log_ssl_error(const std::string& operation);

    // callback invoked after a successful automatic reconnect
    std::function<void()> reconnect_callback_;
};

class HttpClient {
public:
    struct Response {
        int status_code = 0;
        std::map<std::string, std::string> headers;
        std::string body;
        bool success = false;
    };

    static Response post(const std::string& host, int port, 
                        const std::string& path, 
                        const std::string& body,
                        const std::map<std::string, std::string>& headers = {});

private:
    static bool parse_http_response(const std::string& response, Response& result);
}; 