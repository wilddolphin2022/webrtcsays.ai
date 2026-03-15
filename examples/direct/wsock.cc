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

#include "wsock.h"

const absl::string_view base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

WebSocketClient::WebSocketClient()
    : sockfd_(-1),
      ssl_ctx_(nullptr),
      ssl_(nullptr),
      use_ssl_(false),
      running_(false),
      connected_(false),
      read_in_progress_(false),
      allow_reconnect_(true),
      reconnect_attempts_(0),
      network_thread_(nullptr) {
  // Initialize OpenSSL
  SSL_library_init();
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();
  ERR_load_crypto_strings();
}

WebSocketClient::~WebSocketClient() {
  disconnect();
  cleanup_connection();
  
  // Clean up OpenSSL
  ERR_free_strings();
  EVP_cleanup();
}

bool WebSocketClient::connect(const Config& config) {
  config_ = config;
  use_ssl_ = config.use_ssl;

  APP_LOG(AS_INFO) << "Connecting to WebSocket server at " << config.host << ":" << config.port;

  if (!create_socket_connection()) {
    return false;
  }

  if (use_ssl_ && !setup_ssl_context()) {
    cleanup_connection();
    return false;
  }

  if (use_ssl_ && !perform_ssl_handshake()) {
    cleanup_connection();
    return false;
  }

  connected_ = true;
  reconnect_attempts_ = 0; // reset counter on successful connection
  return true;
}

void WebSocketClient::disconnect() {
  running_ = false;
  connected_ = false;
  message_callback_ = nullptr;
  stop_listening();

  // Wait until any in-flight async_read finishes so no callbacks can fire on a
  // partially-destroyed object.  This is a lightweight spin-wait because
  // async_read completes quickly once running_ is false.
  while (read_in_progress_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool WebSocketClient::is_connected() const {
  if (sockfd_ == -1) return false;

  // Check if socket is still valid
  int error = 0;
  socklen_t len = sizeof(error);
  int retval = getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &error, &len);

  if (retval != 0 || error != 0) {
    APP_LOG(AS_ERROR) << "Socket error detected: " << strerror(error);
    return false;
  }

  return connected_.load();
}

bool WebSocketClient::send_message(const std::string& message) {
  return send_websocket_frame(message);
}

void WebSocketClient::set_message_callback(std::function<void(const std::string&)> callback) {
  message_callback_ = callback;
}

void WebSocketClient::start_listening() {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  APP_LOG(AS_INFO) << "WebSocketClient::start_listening: Starting, running_: " << running_.load();
  
  if (running_.exchange(true)) {
    APP_LOG(AS_WARNING) << "WebSocketClient::start_listening: Already running";
    return;
  }
  
  if (!network_thread_) {
    APP_LOG(AS_ERROR) << "WebSocketClient::start_listening: Network thread not set";
    running_ = false;
    return;
  }
  
  buffer_.clear();
  APP_LOG(AS_INFO) << "WebSocketClient::start_listening: Scheduling initial async_read";

  // Capture shared ownership so the object stays alive until callbacks finish.
  auto self = shared_from_this();

  // Start keep-alive pings (first fire after 10 s, then every 5 s).
  network_thread_->PostDelayedTask(
      [self]() {
        if (self->running_.load() && self->is_connected()) {
          self->send_ping();
          self->network_thread_->PostDelayedTask(
              [self]() {
                if (self->running_.load()) {
                  self->start_listening();  // schedules next ping cycle / read
                }
              },
              webrtc::TimeDelta::Seconds(5));
        }
      },
      webrtc::TimeDelta::Seconds(10));

  network_thread_->PostTask([self]() {
    APP_LOG(AS_INFO) << "WebSocketClient::start_listening: Initial async_read posted";
    self->async_read();
  });
}

void WebSocketClient::stop_listening() {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  APP_LOG(AS_INFO) << "WebSocketClient: Stopping listener, current running state: " << running_.load();
  
  if (running_.exchange(false)) {
    APP_LOG(AS_INFO) << "WebSocketClient: Stopped WebSocket listener";

    // Ensure any async_read that might still be executing has completed so
    // that no further callbacks access internal buffers after we return.
    while (read_in_progress_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void WebSocketClient::set_network_thread(rtc::Thread* thread) {
  network_thread_ = thread;
}

std::string WebSocketClient::generate_websocket_key() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  std::string key(16, '\0');
  for (int i = 0; i < 16; ++i) {
    key[i] = static_cast<char>(dis(gen));
  }
  return base64_encode(key);
}

std::string WebSocketClient::base64_encode(const std::string& in) {
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(base64_chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) {
    out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
  }
  while (out.size() % 4) {
    out.push_back('=');
  }
  return out;
}

bool WebSocketClient::create_socket_connection() {
  // DNS resolution
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int status = getaddrinfo(config_.host.c_str(), config_.port.c_str(), &hints, &res);
  if (status != 0) {
    APP_LOG(AS_ERROR) << "getaddrinfo failed: " << gai_strerror(status);
    return false;
  }

  // Create socket
  sockfd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sockfd_ < 0) {
    APP_LOG(AS_ERROR) << "Socket creation failed: " << strerror(errno);
    freeaddrinfo(res);
    return false;
  }
  APP_LOG(AS_INFO) << "Socket created: " << sockfd_;

  // Connect to server
  if (::connect(sockfd_, res->ai_addr, res->ai_addrlen) < 0) {
    APP_LOG(AS_ERROR) << "Socket connect failed: " << strerror(errno);
    close(sockfd_);
    sockfd_ = -1;
    freeaddrinfo(res);
    return false;
  }
  APP_LOG(AS_INFO) << "TCP connection established to " << config_.host << ":" << config_.port;

  freeaddrinfo(res);
  return true;
}

bool WebSocketClient::setup_ssl_context() {
  // Create SSL context with proper method and options
  ssl_ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ssl_ctx_) {
    log_ssl_error("Failed to create SSL context");
    return false;
  }

  // Set verification mode to NONE - accept self-signed certificates
  SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

  // Set modern cipher list
  if (SSL_CTX_set_cipher_list(ssl_ctx_, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4") != 1) {
    log_ssl_error("Failed to set cipher list");
  }

  // Enable TLS extensions and SNI
  SSL_CTX_set_options(ssl_ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

  // Load client certificate if provided
  if (!config_.cert_file_path.empty()) {
    APP_LOG(AS_INFO) << "Loading client certificate from: " << config_.cert_file_path;
    if (SSL_CTX_use_certificate_file(ssl_ctx_, config_.cert_file_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
      log_ssl_error("Failed to load certificate file");
      // Continue anyway, certificate might not be required
    }
  }

  // Load private key if provided
  if (!config_.key_file_path.empty()) {
    APP_LOG(AS_INFO) << "Loading private key from: " << config_.key_file_path;
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, config_.key_file_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
      log_ssl_error("Failed to load private key file");
      // Continue anyway, key might not be required
    }

    // Check key and certificate compatibility
    if (SSL_CTX_check_private_key(ssl_ctx_) <= 0) {
      log_ssl_error("Private key does not match the certificate");
      // Continue anyway, might still work
    }
  }

  return true;
}

bool WebSocketClient::perform_ssl_handshake() {
  // Create new SSL object
  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    log_ssl_error("Failed to create SSL object");
    return false;
  }

  // Set socket to SSL object
  if (SSL_set_fd(ssl_, sockfd_) != 1) {
    log_ssl_error("Failed to set SSL file descriptor");
    SSL_free(ssl_);
    ssl_ = nullptr;
    return false;
  }

  // Set SNI hostname
  if (SSL_set_tlsext_host_name(ssl_, config_.host.c_str()) != 1) {
    log_ssl_error("Failed to set SNI hostname");
    // Continue anyway, SNI is not critical
  }

  // Perform SSL handshake
  int connect_result = SSL_connect(ssl_);
  if (connect_result <= 0) {
    int ssl_err = SSL_get_error(ssl_, connect_result);
    APP_LOG(AS_ERROR) << "SSL connection failed with error " << ssl_err;
    log_ssl_error("SSL handshake failed");
    SSL_free(ssl_);
    ssl_ = nullptr;
    return false;
  }

  // Log successful SSL connection
  APP_LOG(AS_INFO) << "SSL connection established with " << SSL_get_cipher(ssl_)
                   << ", protocol: " << SSL_get_version(ssl_);

  // Verify certificate (optional, we're using SSL_VERIFY_NONE)
  X509* cert = SSL_get_peer_certificate(ssl_);
  if (cert) {
    APP_LOG(AS_INFO) << "Server certificate verified";
    X509_free(cert);
  } else {
    APP_LOG(AS_WARNING) << "No server certificate received";
  }

  return true;
}

bool WebSocketClient::send_http_handshake() {
  std::string path = "/";
  if (config_.headers.find("path") != config_.headers.end()) {
    path = config_.headers.at("path");
  }

  std::ostringstream request;
  request << "GET " << path << " HTTP/1.1\r\n";
  request << "Host: " << config_.host << ":" << config_.port << "\r\n";
  request << "Connection: Upgrade\r\n";
  request << "Pragma: no-cache\r\n";
  request << "Cache-Control: no-cache\r\n";
  request << "User-Agent: WebSocketClient/1.0\r\n";
  request << "Upgrade: websocket\r\n";
  request << "Sec-WebSocket-Version: 13\r\n";
  request << "Sec-WebSocket-Key: " << generate_websocket_key() << "\r\n";

  // Add custom headers
  for (const auto& [key, value] : config_.headers) {
    if (key != "path") {  // Skip path as it's handled above
      request << key << ": " << value << "\r\n";
    }
  }

  request << "\r\n";

  std::string request_str = request.str();
  APP_LOG(AS_INFO) << "Sending HTTP handshake: " << request_str;

  int bytes_written;
  if (use_ssl_) {
    bytes_written = SSL_write(ssl_, request_str.c_str(), request_str.length());
    if (bytes_written <= 0) {
      log_ssl_error("SSL_write failed in handshake");
      return false;
    }
  } else {
    bytes_written = send(sockfd_, request_str.c_str(), request_str.length(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed in handshake: " << strerror(errno);
      return false;
    }
  }
  APP_LOG(AS_INFO) << "Sent " << bytes_written << " bytes";

  // Read response
  char buffer[4096];
  std::string response;
  int total_bytes_read = 0;
  int timeout_ms = config_.timeout_ms;
  int elapsed_ms = 0;
  const int poll_interval_ms = 100;

  while (elapsed_ms < timeout_ms) {
    int bytes_read;
    if (use_ssl_) {
      bytes_read = SSL_read(ssl_, buffer + total_bytes_read,
                            sizeof(buffer) - total_bytes_read - 1);
      if (bytes_read <= 0) {
        int err = SSL_get_error(ssl_, bytes_read);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
          std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
          elapsed_ms += poll_interval_ms;
          continue;
        }
        log_ssl_error("SSL_read failed in handshake");
        return false;
      }
    } else {
      bytes_read = recv(sockfd_, buffer + total_bytes_read,
                        sizeof(buffer) - total_bytes_read - 1, 0);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
          elapsed_ms += poll_interval_ms;
          continue;
        }
        APP_LOG(AS_ERROR) << "recv failed in handshake: " << strerror(errno);
        return false;
      } else if (bytes_read == 0) {
        APP_LOG(AS_ERROR) << "Connection closed by server during handshake";
        return false;
      }
    }

    total_bytes_read += bytes_read;
    buffer[total_bytes_read] = '\0';
    response = std::string(buffer, total_bytes_read);
    APP_LOG(AS_INFO) << "Received handshake data: " << response;
    
    if (response.find("\r\n\r\n") != std::string::npos) {
      APP_LOG(AS_INFO) << "Full handshake response: " << response;
      if (response.find("101 Switching Protocols") != std::string::npos) {
        // For raw WebSocket connections, we don't need to process Socket.IO handshake data
        // Just return success after successful WebSocket handshake
        APP_LOG(AS_INFO) << "Raw WebSocket handshake successful";
        return true;
      } else {
        APP_LOG(AS_ERROR) << "WebSocket handshake failed: " << response;
        return false;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    elapsed_ms += poll_interval_ms;
  }

  APP_LOG(AS_ERROR) << "Handshake timed out after " << timeout_ms << "ms, received: " << response;
  return false;
}

bool WebSocketClient::send_websocket_handshake_with_headers() {
  return send_http_handshake();
}

bool WebSocketClient::send_websocket_frame(const std::string& message) {
  APP_LOG(AS_VERBOSE) << "Sending WebSocket message: " << message;

  std::vector<unsigned char> frame;
  frame.push_back(0x81);  // FIN + text frame

  size_t length = message.length();
  if (length <= 125) {
    frame.push_back(0x80 | length);  // Masked + length
  } else if (length <= 65535) {
    frame.push_back(0x80 | 126);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  } else {
    frame.push_back(0x80 | 127);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back((length >> 24) & 0xFF);
    frame.push_back((length >> 16) & 0xFF);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  }

  // Generate random mask key
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  std::vector<unsigned char> mask(4);
  for (int i = 0; i < 4; ++i) {
    mask[i] = dis(gen);
    frame.push_back(mask[i]);
  }

  // Apply mask to payload
  std::vector<unsigned char> masked_payload(length);
  for (size_t i = 0; i < length; ++i) {
    masked_payload[i] = message[i] ^ mask[i % 4];
  }

  // Add masked payload to frame
  frame.insert(frame.end(), masked_payload.begin(), masked_payload.end());

  std::lock_guard<std::mutex> lock(ssl_mutex_);
  if (sockfd_ == -1) {
    APP_LOG(AS_ERROR) << "Cannot send frame: socket is invalid (closed)";
    running_ = false;
    return false;
  }

  int bytes_written;
  if (use_ssl_) {
    if (!ssl_) {
      APP_LOG(AS_ERROR) << "SSL object is null";
      return false;
    }
    bytes_written = SSL_write(ssl_, frame.data(), frame.size());
    if (bytes_written <= 0) {
      log_ssl_error("SSL_write failed");
      return false;
    }
  } else {
    bytes_written = send(sockfd_, frame.data(), frame.size(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed: " << strerror(errno) << " (errno: " << errno << ")";
      if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
        APP_LOG(AS_INFO) << "Connection broken or invalid, marking as disconnected";
        running_ = false;
      }
      return false;
    }
  }

  APP_LOG(AS_VERBOSE) << "Sent WebSocket frame successfully, bytes written: " << bytes_written;

  // Force an immediate read to get the response
  if (network_thread_) {
    auto self = shared_from_this();
    network_thread_->PostTask([self]() {
      if (self->running_.load() && !self->read_in_progress_.load()) {
        APP_LOG(AS_VERBOSE) << "Scheduling immediate read after sending frame";
        self->async_read();
      }
    });
  }

  return true;
}

std::vector<std::string> WebSocketClient::process_websocket_frames() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  std::vector<std::string> messages;
  std::string accumulated_payload;
  bool is_continuation = false;

  if (buffer_.empty()) {
    APP_LOG(AS_VERBOSE) << "process_websocket_frames: Buffer empty";
    return messages;
  }

  APP_LOG(AS_VERBOSE) << "process_websocket_frames: Buffer size: " << buffer_.size();

  size_t pos = 0;
  while (pos + 1 < buffer_.size()) {
    uint8_t first_byte = buffer_[pos];
    bool fin = (first_byte & 0x80) != 0;
    uint8_t opcode = first_byte & 0x0F;

    if (opcode > 0xA || (opcode > 0x2 && opcode < 0x8)) {
      APP_LOG(AS_ERROR) << "Invalid opcode 0x" << std::to_string((int)opcode) << " at pos " << pos;
      pos++;
      continue;
    }

    uint8_t second_byte = buffer_[pos + 1];
    bool masked = (second_byte & 0x80) != 0;
    uint64_t length = second_byte & 0x7F;
    size_t header_size = 2;

    if (length == 126) {
      if (pos + 4 > buffer_.size()) {
        APP_LOG(AS_INFO) << "Incomplete 16-bit length field at pos " << pos;
        break;
      }
      length = (static_cast<uint64_t>(buffer_[pos + 2] & 0xFF) << 8) |
               static_cast<uint64_t>(buffer_[pos + 3] & 0xFF);
      header_size = 4;
    } else if (length == 127) {
      if (pos + 10 > buffer_.size()) {
        APP_LOG(AS_INFO) << "Incomplete 64-bit length field at pos " << pos;
        break;
      }
      length = (static_cast<uint64_t>(buffer_[pos + 2] & 0xFF) << 56) |
               (static_cast<uint64_t>(buffer_[pos + 3] & 0xFF) << 48) |
               (static_cast<uint64_t>(buffer_[pos + 4] & 0xFF) << 40) |
               (static_cast<uint64_t>(buffer_[pos + 5] & 0xFF) << 32) |
               (static_cast<uint64_t>(buffer_[pos + 6] & 0xFF) << 24) |
               (static_cast<uint64_t>(buffer_[pos + 7] & 0xFF) << 16) |
               (static_cast<uint64_t>(buffer_[pos + 8] & 0xFF) << 8) |
               static_cast<uint64_t>(buffer_[pos + 9] & 0xFF);
      header_size = 10;
    }

    size_t mask_offset = masked ? 4 : 0;
    if (pos + header_size + mask_offset > buffer_.size()) {
      APP_LOG(AS_INFO) << "Incomplete header or mask at pos " << pos;
      break;
    }

    uint64_t total_frame_size = header_size + mask_offset + length;

    if (pos + total_frame_size > buffer_.size()) {
      APP_LOG(AS_VERBOSE) << "Incomplete frame at pos " << pos << ", need "
                          << (pos + total_frame_size - buffer_.size()) << " more bytes";
      break;
    }

    if (opcode == 0x01 || opcode == 0x00) {
      std::string payload(buffer_.begin() + pos + header_size + mask_offset,
                          buffer_.begin() + pos + total_frame_size);
      if (masked) {
        unsigned char mask_key[4];
        for (int i = 0; i < 4; i++)
          mask_key[i] = buffer_[pos + header_size + i];
        for (size_t i = 0; i < payload.length(); i++) {
          payload[i] ^= mask_key[i % 4];
        }
      }

      if (opcode == 0x00) {
        if (!is_continuation) {
          APP_LOG(AS_ERROR) << "Continuation frame without start, ignoring";
          pos += total_frame_size;
          continue;
        }
        accumulated_payload += payload;
      } else if (is_continuation && fin) {
        accumulated_payload += payload;
        APP_LOG(AS_INFO) << "Extracted complete fragmented text payload: "
                         << accumulated_payload.substr(0, 100)
                         << (accumulated_payload.length() > 100 ? "..." : "");
        messages.push_back(accumulated_payload);
        accumulated_payload.clear();
        is_continuation = false;
      } else if (!is_continuation && fin) {
        APP_LOG(AS_VERBOSE) << "Extracted single text payload: " << payload.substr(0, 100)
                            << (payload.length() > 100 ? "..." : "");
        messages.push_back(payload);
      } else {
        accumulated_payload = payload;
        is_continuation = true;
      }
    } else if (opcode == 0x09) {
      std::vector<unsigned char> ping_payload(
          buffer_.begin() + pos + header_size + mask_offset,
          buffer_.begin() + pos + total_frame_size);
      if (masked) {
        unsigned char mask_key[4];
        for (int i = 0; i < 4; i++)
          mask_key[i] = buffer_[pos + header_size + i];
        for (size_t i = 0; i < ping_payload.size(); i++) {
          ping_payload[i] ^= mask_key[i % 4];
        }
      }
      APP_LOG(AS_INFO) << "Received ping frame, length: " << length;
      send_pong_frame(ping_payload);
    } else if (opcode == 0x08) {
      APP_LOG(AS_INFO) << "Received close frame";
      running_ = false;
      messages.push_back("DISCONNECTED");
    } else {
      APP_LOG(AS_WARNING) << "Skipping unhandled opcode: 0x" << std::to_string((int)opcode);
    }

    pos += total_frame_size;
  }

  if (pos > 0) {
    APP_LOG(AS_VERBOSE) << "Erasing " << pos << " bytes from buffer";
    buffer_.erase(buffer_.begin(), buffer_.begin() + pos);
  }
  return messages;
}

void WebSocketClient::send_ping() {
  std::vector<unsigned char> frame;
  frame.push_back(0x89);  // FIN + Ping frame
  frame.push_back(0x00);  // Unmasked, length 0 (no payload)

  std::lock_guard<std::mutex> lock(ssl_mutex_);
  int bytes_written;
  if (use_ssl_) {
    if (!ssl_) {
      APP_LOG(AS_ERROR) << "SSL object is null in send_ping";
      return;
    }
    bytes_written = SSL_write(ssl_, frame.data(), frame.size());
    if (bytes_written <= 0) {
      log_ssl_error("SSL_write failed in send_ping");
      return;
    }
  } else {
    bytes_written = send(sockfd_, frame.data(), frame.size(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed in send_ping: " << strerror(errno);
      return;
    }
  }
  APP_LOG(AS_INFO) << "Sent ping frame";
}

void WebSocketClient::send_pong_frame(const std::vector<unsigned char>& ping_payload) {
  std::vector<unsigned char> frame;
  frame.push_back(0x8A);  // FIN + Pong frame

  size_t length = ping_payload.size();
  if (length <= 125) {
    frame.push_back(0x00 | length);  // Unmasked + length
  } else if (length <= 65535) {
    frame.push_back(0x00 | 126);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  } else {
    frame.push_back(0x00 | 127);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back((length >> 24) & 0xFF);
    frame.push_back((length >> 16) & 0xFF);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  }

  frame.insert(frame.end(), ping_payload.begin(), ping_payload.end());

  std::lock_guard<std::mutex> lock(ssl_mutex_);
  int bytes_written;
  if (use_ssl_) {
    if (!ssl_) {
      APP_LOG(AS_ERROR) << "SSL object is null in send_pong_frame";
      return;
    }
    bytes_written = SSL_write(ssl_, frame.data(), frame.size());
    if (bytes_written <= 0) {
      log_ssl_error("SSL_write failed in send_pong_frame");
      return;
    }
  } else {
    bytes_written = send(sockfd_, frame.data(), frame.size(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed in send_pong_frame: " << strerror(errno);
      return;
    }
  }
  APP_LOG(AS_INFO) << "Sent pong frame in response to ping";
}

void WebSocketClient::async_read() {
  APP_LOG(AS_INFO) << "WebSocketClient::async_read: Starting, running_: " << running_.load()
                   << ", read_in_progress_: " << read_in_progress_.load()
                   << ", sockfd: " << sockfd_;

  if (!running_.load()) {
    APP_LOG(AS_ERROR) << "WebSocketClient::async_read: Not running, skipping";
    read_in_progress_ = false;
    return;
  }

  if (read_in_progress_.exchange(true)) {
    APP_LOG(AS_VERBOSE) << "WebSocketClient::async_read: Read already in progress, skipping";
    return;
  }

  if (!network_thread_) {
    APP_LOG(AS_ERROR) << "WebSocketClient::async_read: Network thread not set";
    read_in_progress_ = false;
    return;
  }

  if (sockfd_ == -1 || !is_connected()) {
    APP_LOG(AS_ERROR) << "WebSocketClient::async_read: Socket closed or invalid, attempting reconnect";
    running_ = false;
    read_in_progress_ = false;
    if (message_callback_)
      message_callback_("DISCONNECTED");
    {
      auto self = shared_from_this();
      network_thread_->PostTask([self]() { self->attempt_reconnect(); });
    }
    return;
  }

  char buffer[8192];
  int total_bytes_read = 0;
  int attempts = 0;
  const int max_attempts = 5;
  bool no_more_data = false;

  do {
    int bytes_read = 0;
    if (use_ssl_) {
      if (!ssl_) {
        APP_LOG(AS_ERROR) << "WebSocketClient::async_read: SSL object is null";
        running_ = false;
        read_in_progress_ = false;
        if (message_callback_)
          message_callback_("DISCONNECTED");
        return;
      }
      bytes_read = SSL_read(ssl_, buffer + total_bytes_read,
                            sizeof(buffer) - total_bytes_read - 1);

      if (bytes_read <= 0) {
        int ssl_err = SSL_get_error(ssl_, bytes_read);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
          no_more_data = true;
          attempts++;
          if (attempts < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
          }
          break;
        } else {
          log_ssl_error("SSL_read failed in async_read");
          running_ = false;
          read_in_progress_ = false;
          if (message_callback_)
            message_callback_("DISCONNECTED");
          {
            auto self = shared_from_this();
            network_thread_->PostTask([self]() { self->attempt_reconnect(); });
          }
          return;
        }
      }
    } else {
      bytes_read = recv(sockfd_, buffer + total_bytes_read,
                        sizeof(buffer) - total_bytes_read - 1, 0);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          no_more_data = true;
          attempts++;
          if (attempts < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
          }
          break;
        } else {
          APP_LOG(AS_ERROR) << "WebSocketClient::async_read: Connection lost: " << strerror(errno);
          running_ = false;
          read_in_progress_ = false;
          if (message_callback_)
            message_callback_("DISCONNECTED");
          {
            auto self = shared_from_this();
            network_thread_->PostTask([self]() { self->attempt_reconnect(); });
          }
          return;
        }
      }
    }

    if (bytes_read > 0) {
      total_bytes_read += bytes_read;
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.append(buffer + total_bytes_read - bytes_read, bytes_read);
        APP_LOG(AS_VERBOSE) << "WebSocketClient::async_read: Buffer size now: " << buffer_.size();
      }
      std::vector<std::string> messages = process_websocket_frames();
      for (const auto& message : messages) {
        if (message_callback_ && !message.empty()) {
          APP_LOG(AS_VERBOSE) << "WebSocketClient::async_read: Calling callback with: "
                              << message.substr(0, 100) << (message.length() > 100 ? "..." : "");
          message_callback_(message);
        }
      }
      attempts = 0;
    }
  } while (running_.load() && total_bytes_read < static_cast<int>(sizeof(buffer) - 1) && attempts < max_attempts);

  read_in_progress_ = false;
  if (running_.load()) {
    auto self = shared_from_this();
    network_thread_->PostDelayedTask(
        [self]() {
          if (self->running_.load())
            self->async_read();
        },
        webrtc::TimeDelta::Millis(no_more_data ? 100 : 10));
  }
}

void WebSocketClient::force_sync_read(int timeout_ms) {
  APP_LOG(AS_INFO) << "WebSocketClient::force_sync_read: Attempting synchronous read";

  char temp[8192];
  int total_bytes_read = 0;
  int attempts = 0;
  const int max_attempts = timeout_ms / 100;

  while (attempts < max_attempts) {
    int bytes_read = 0;
    if (use_ssl_) {
      if (ssl_) {
        bytes_read = SSL_read(ssl_, temp + total_bytes_read, sizeof(temp) - total_bytes_read);
        if (bytes_read <= 0) {
          int ssl_err = SSL_get_error(ssl_, bytes_read);
          if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            attempts++;
            continue;
          } else {
            log_ssl_error("SSL_read error in force_sync_read");
            break;
          }
        }
      } else {
        APP_LOG(AS_ERROR) << "WebSocketClient::force_sync_read: SSL object is null";
        break;
      }
    } else {
      bytes_read = recv(sockfd_, temp + total_bytes_read, sizeof(temp) - total_bytes_read, 0);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          attempts++;
          continue;
        } else {
          APP_LOG(AS_ERROR) << "WebSocketClient::force_sync_read: Read failed, errno: " << strerror(errno);
          break;
        }
      } else if (bytes_read == 0) {
        APP_LOG(AS_INFO) << "WebSocketClient::force_sync_read: Connection closed";
        running_ = false;
        if (message_callback_)
          message_callback_("DISCONNECTED");
        break;
      }
    }

    if (bytes_read > 0) {
      total_bytes_read += bytes_read;
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.append(temp, bytes_read);
      }
      std::vector<std::string> messages = process_websocket_frames();
      for (const auto& message : messages) {
        if (message_callback_ && !message.empty()) {
          message_callback_(message);
        }
      }
    }
    attempts++;
  }
}

void WebSocketClient::attempt_reconnect() {
  if (!allow_reconnect_) {
    APP_LOG(AS_INFO) << "WebSocketClient: Reconnect aborted – not allowed after disconnect.";
    return;
  }
  // Limit the number of successive reconnect attempts to avoid infinite loops
  int current_attempt = ++reconnect_attempts_;
  if (current_attempt > kMaxReconnectAttempts) {
    APP_LOG(AS_ERROR) << "WebSocketClient: Reached maximum reconnect attempts (" << kMaxReconnectAttempts << "), giving up.";
    allow_reconnect_ = false;
    if (message_callback_) {
      message_callback_("RECONNECT_FAILED");
    }
    return;
  }

  // Apply simple linear back-off: wait current_attempt * 2 seconds (capped below)
  int backoff_seconds = std::min(current_attempt * 2, 30); // cap to 30 s
  if (current_attempt > 1) {
    APP_LOG(AS_INFO) << "WebSocketClient: Reconnect attempt " << current_attempt << " will start in " << backoff_seconds << " seconds";
  }

  // Schedule the actual reconnect logic on the network thread after back-off delay
  if (network_thread_) {
    auto self = shared_from_this();
    network_thread_->PostDelayedTask([self]() {
      self->attempt_reconnect_internal();
    }, webrtc::TimeDelta::Seconds(backoff_seconds));
  }
}

// Helper containing the original reconnect logic – invoked after back-off
void WebSocketClient::attempt_reconnect_internal() {
  APP_LOG(AS_INFO) << "WebSocketClient: Attempting to reconnect (attempt " << reconnect_attempts_.load() << ")";
  cleanup_connection();
  
  if (connect(config_)) {
    APP_LOG(AS_INFO) << "WebSocketClient: Reconnected successfully";
    start_listening();

    // Reset attempts counter so future disconnects get a fresh budget
    reconnect_attempts_ = 0;

    // Notify owner that connection is alive again so it can re-register.
    if (reconnect_callback_) {
      reconnect_callback_();
    }
  } else {
    APP_LOG(AS_ERROR) << "WebSocketClient: Reconnect failed, scheduling another attempt";
    if (network_thread_) {
      auto self = shared_from_this();
      network_thread_->PostTask([self]() { self->attempt_reconnect(); });
    }
  }
}

void WebSocketClient::cleanup_connection() {
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  if (sockfd_ != -1) {
    ::close(sockfd_);
    sockfd_ = -1;
  }
}

void WebSocketClient::log_ssl_error(const std::string& operation) {
  APP_LOG(AS_ERROR) << operation << ": " << ERR_error_string(ERR_get_error(), nullptr);
}

// HttpClient implementation
HttpClient::Response HttpClient::post(const std::string& host, int port, 
                                     const std::string& path, 
                                     const std::string& body,
                                     const std::map<std::string, std::string>& headers) {
  Response response;
  
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    APP_LOG(AS_ERROR) << "HttpClient: Failed to create socket: " << strerror(errno);
    return response;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  
  if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
    APP_LOG(AS_ERROR) << "HttpClient: Invalid server address: " << host;
    close(sockfd);
    return response;
  }

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    APP_LOG(AS_ERROR) << "HttpClient: Failed to connect to server: " << strerror(errno);
    close(sockfd);
    return response;
  }

  // Build HTTP request
  std::ostringstream http_request;
  http_request << "POST " << path << " HTTP/1.1\r\n";
  http_request << "Host: " << host << ":" << port << "\r\n";
  http_request << "Content-Length: " << body.length() << "\r\n";
  http_request << "Connection: close\r\n";

  // Add custom headers
  for (const auto& [key, value] : headers) {
    http_request << key << ": " << value << "\r\n";
  }

  http_request << "\r\n";
  http_request << body;

  std::string request_str = http_request.str();
  
  // Send HTTP request
  if (send(sockfd, request_str.c_str(), request_str.length(), 0) < 0) {
    APP_LOG(AS_ERROR) << "HttpClient: Failed to send HTTP request: " << strerror(errno);
    close(sockfd);
    return response;
  }

  // Read HTTP response
  char buffer[4096] = {0};
  int bytes_read = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
  close(sockfd);

  if (bytes_read <= 0) {
    APP_LOG(AS_ERROR) << "HttpClient: Failed to read HTTP response";
    return response;
  }

  std::string response_str(buffer, bytes_read);
  if (parse_http_response(response_str, response)) {
    response.success = true;
  }

  return response;
}

bool HttpClient::parse_http_response(const std::string& response, Response& result) {
  // Find the end of headers
  size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    APP_LOG(AS_ERROR) << "HttpClient: Invalid HTTP response format";
    return false;
  }

  std::string headers_str = response.substr(0, header_end);
  result.body = response.substr(header_end + 4);

  // Parse status line
  size_t first_line_end = headers_str.find("\r\n");
  if (first_line_end == std::string::npos) {
    APP_LOG(AS_ERROR) << "HttpClient: Invalid status line";
    return false;
  }

  std::string status_line = headers_str.substr(0, first_line_end);
  
  // Extract status code (format: "HTTP/1.1 200 OK")
  size_t first_space = status_line.find(' ');
  size_t second_space = status_line.find(' ', first_space + 1);
  if (first_space != std::string::npos && second_space != std::string::npos) {
    std::string status_code_str = status_line.substr(first_space + 1, second_space - first_space - 1);
    result.status_code = std::stoi(status_code_str);
  }

  // Parse headers
  std::istringstream header_stream(headers_str.substr(first_line_end + 2));
  std::string line;
  while (std::getline(header_stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      std::string key = line.substr(0, colon_pos);
      std::string value = line.substr(colon_pos + 1);
      
      // Trim whitespace
      key.erase(0, key.find_first_not_of(" \t"));
      key.erase(key.find_last_not_of(" \t") + 1);
      value.erase(0, value.find_first_not_of(" \t"));
      value.erase(value.find_last_not_of(" \t") + 1);
      
      result.headers[key] = value;
    }
  }

  return true;
} 