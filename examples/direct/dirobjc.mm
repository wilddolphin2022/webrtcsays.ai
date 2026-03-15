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

#include "direct.h"
#include "option.h"

#if TARGET_OS_IOS && defined(__OBJC__)
#include <memory>

#import "sdk/objc/base/RTCVideoCapturer.h"
#import "sdk/objc/components/renderer/metal/RTCMTLVideoView.h"
#import "sdk/objc/native/src/objc_video_track_source.h"
#import "sdk/objc/native/src/objc_video_renderer.h"

// Convenience shim: take an Obj-C RTCVideoCapturer and wrap it into WebRTC's
// ObjCVideoTrackSource, then inject into DirectApplication.
void DirectApplication::SetVideoCapturer(RTCVideoCapturer* capturer) {
  // Create adapter that implements RTCVideoCapturerDelegate
  RTCObjCVideoSourceAdapter* adapter = [[RTCObjCVideoSourceAdapter alloc] init];
  // Point capturer at our adapter
  capturer.delegate = adapter;
  // Wrap adapter in a native VideoTrackSource
  auto native_source = rtc::make_ref_counted<webrtc::ObjCVideoTrackSource>(adapter);
  // Hand it to the C++ engine
  RTC_LOG(LS_INFO) << "SetVideoCapturer:: SetVideoSource by ObjCVideoTrackSource";
  SetVideoSource(native_source);
}

void DirectApplication::SetVideoRenderer(RTCMTLVideoView* renderer) {
  // Wrap the provided Obj-C view into a native VideoSink
  std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> native_sink =
      std::make_unique<webrtc::ObjCVideoRenderer>(renderer);
  // Hand it to the C++ engine
  RTC_LOG(LS_INFO) << "SetVideoRenderer:: SetVideoSink by ObjCVideoRenderer";
  SetVideoSink(std::move(native_sink));
}

RTCVideoTrack* DirectApplication::GetLocalVideoTrack() {
  // Note: This method should not be used directly as C++ and Obj-C WebRTC APIs are separate
  // The video tracks should be managed at the Objective-C wrapper level
  RTC_LOG(LS_WARNING) << "GetLocalVideoTrack called - C++ to Obj-C video track conversion not supported";
  return nil;
}

RTCVideoTrack* DirectApplication::GetRemoteVideoTrack() {
  // Note: This method should not be used directly as C++ and Obj-C WebRTC APIs are separate
  // The video tracks should be managed at the Objective-C wrapper level
  RTC_LOG(LS_WARNING) << "GetRemoteVideoTrack called - C++ to Obj-C video track conversion not supported";
  return nil;
}
#endif  // #if TARGET_OS_IOS && defined(__OBJC__)

#if TARGET_OS_IOS || TARGET_OS_OSX

// --- Bonjour (mDNS/DNS-SD) advertisement and discovery for C++ ---
// Only works on macOS/iOS. Used by AdvertiseBonjourService/DiscoverBonjourService in bonjour.h

#include "bonjour.h"
#include <dns_sd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <iostream>
#include <condition_variable>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <string>

// Global handle for advertisement (for simplicity, only one at a time)
static DNSServiceRef g_advert_ref = nullptr;

// Struct to hold IP address and Wi-Fi flag
struct IPAddressResult {
    std::string ip;
    bool isWiFi;
};

IPAddressResult GetLocalIPAddress() {
    struct ifaddrs *ifaddr, *ifa;
    char ip[INET_ADDRSTRLEN] = {0};
    bool isWiFi = false;
    std::string selectedInterface;

    if (getifaddrs(&ifaddr) == -1) {
        RTC_LOG(LS_ERROR) << "getifaddrs failed: " << strerror(errno);
        return {"", false};
    }

    // Debug: Log all interfaces
    RTC_LOG(LS_INFO) << "Available interfaces:";
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, INET_ADDRSTRLEN);
            RTC_LOG(LS_INFO) << "Interface: " << ifa->ifa_name << ", IP: " << ip
                      << ", Flags: " << (ifa->ifa_flags & IFF_LOOPBACK ? "LOOPBACK" : "")
                      << (ifa->ifa_flags & IFF_UP ? "UP" : "");
            ip[0] = '\0';
        }
    }

    // Helper function to check if interface name looks like a network interface
    auto isNetworkInterface = [](const char* name) {
        // Check for common network interface patterns: en0, en1, en2, etc.
        return (strncmp(name, "en", 2) == 0 && strlen(name) >= 3 && isdigit(name[2]));
    };

    // First pass: Look for preferred network interfaces (en0, en1, etc.)
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET &&
            !(ifa->ifa_flags & IFF_LOOPBACK) &&
            (ifa->ifa_flags & IFF_UP) &&
            isNetworkInterface(ifa->ifa_name)) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
            isWiFi = true;  // Assume en* interfaces are Wi-Fi/Ethernet (suitable for Bonjour)
            selectedInterface = ifa->ifa_name;
            RTC_LOG(LS_INFO) << "Selected interface: " << ifa->ifa_name << ", IP: " << ip << " (Network)";
            break;
        }
    }

    // Second pass: Fallback to any non-loopback, active interface
    if (ip[0] == '\0') {
        for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET &&
                !(ifa->ifa_flags & IFF_LOOPBACK) &&
                (ifa->ifa_flags & IFF_UP)) {
                struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
                isWiFi = false;
                selectedInterface = ifa->ifa_name;
                RTC_LOG(LS_INFO) << "Selected fallback interface: " << ifa->ifa_name << ", IP: " << ip << " (Fallback)";
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    if (ip[0] == '\0') {
        RTC_LOG(LS_ERROR) << "No valid IP found";
    }
    return {ip, isWiFi};
}

// Callback for DNSServiceRegister
static void DNSSD_API register_callback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    DNSServiceErrorType errorCode,
    const char *name,
    const char *regtype,
    const char *domain,
    void *context)
{
    if (errorCode == kDNSServiceErr_NoError) {
        RTC_LOG(LS_INFO) << "[Register] Service registered successfully: " << (name ? name : "null") 
                         << " (" << (regtype ? regtype : "null") << ") in domain: " << (domain ? domain : "null");
    } else {
        RTC_LOG(LS_ERROR) << "[Register] Registration failed with error: " << errorCode;
    }
}

bool DIRECT_API AdvertiseBonjourService(const std::string& name, int port) {
    if (g_advert_ref) {
        RTC_LOG(LS_INFO) << "Stopping existing Bonjour advertisement to update with current IP";
        DNSServiceRefDeallocate(g_advert_ref);
        g_advert_ref = nullptr;
        // Longer delay to ensure the service is properly deregistered and cache cleared
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }

    // Get the local IP and Wi-Fi flag
    IPAddressResult ipResult = GetLocalIPAddress();
    if (ipResult.ip.empty()) {
        RTC_LOG(LS_ERROR) << "Failed to retrieve a valid IP address for Bonjour advertising";
        return false;
    }

    // Prefer network interfaces for Bonjour, but allow fallback
    if (!ipResult.isWiFi) {
        RTC_LOG(LS_WARNING) << "Warning: Using fallback interface for Bonjour advertising (may have limited discoverability)";
    }

    // Sanitize service name
    std::string service_name = name;
    // Remove invalid characters (keep alphanumeric, hyphen, underscore)
    for (char& c : service_name) {
        if (!std::isalnum(c) && c != '-' && c != '_') {
            c = '_';
        }
    }
    // Trim to 63 bytes (Bonjour limit)
    if (service_name.length() > 63) {
        service_name = service_name.substr(0, 63);
    }
    // Fallback if empty or invalid
    if (service_name.empty()) {
        RTC_LOG(LS_ERROR) << "Service name is empty or invalid after sanitization";
        return false;
    }

    // Get system hostname for logging purposes
    char sys_hostname[256];
    if (gethostname(sys_hostname, sizeof(sys_hostname)) != 0) {
        RTC_LOG(LS_ERROR) << "gethostname failed: " << strerror(errno);
        sys_hostname[0] = '\0';
    }
    
    std::string sys_hostname_str(sys_hostname);
    RTC_LOG(LS_INFO) << "System hostname: " << (sys_hostname_str.empty() ? "empty" : sys_hostname_str);

    // Create TXT record with IP and timestamp as separate key-value pairs
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    unsigned char txt_buffer[256] = {0};
    uint16_t txt_len = 0;
    
    // Add ip=x.x.x.x
    std::string ip_record = "ip=" + ipResult.ip;
    if (ip_record.length() < 255) {
        txt_buffer[txt_len] = static_cast<unsigned char>(ip_record.length());
        txt_len++;
        memcpy(txt_buffer + txt_len, ip_record.c_str(), ip_record.length());
        txt_len += ip_record.length();
    }
    
    // Add ts=timestamp
    std::string ts_record = "ts=" + std::to_string(now);
    if (txt_len + ts_record.length() + 1 < 256) {
        txt_buffer[txt_len] = static_cast<unsigned char>(ts_record.length());
        txt_len++;
        memcpy(txt_buffer + txt_len, ts_record.c_str(), ts_record.length());
        txt_len += ts_record.length();
    }
    
    std::string txt_summary = ip_record + "," + ts_record;

    // Log parameters for debugging
    RTC_LOG(LS_INFO) << "Registering Bonjour service:";
    RTC_LOG(LS_INFO) << "  Name: " << service_name;
    RTC_LOG(LS_INFO) << "  Type: _webrtcsays._tcp";
    RTC_LOG(LS_INFO) << "  Port: " << port;
    RTC_LOG(LS_INFO) << "  Host: (system default)";
    RTC_LOG(LS_INFO) << "  TXT: " << (txt_len ? txt_summary : "none");

    // Advertise as _webrtcsays._tcp.local.
    // Use kDNSServiceFlagsUnique to force replacement of existing records
    // Use nullptr for hostname to let mDNS use the system's resolvable hostname
    DNSServiceErrorType err = DNSServiceRegister(
        &g_advert_ref,
        kDNSServiceFlagsUnique, // flags - force unique registration (replaces existing)
        0, // interface index (0 = all)
        service_name.c_str(),
        "_webrtcsays._tcp",
        nullptr, // domain (default local)
        nullptr, // hostname (use system default)
        htons(port),
        txt_len,
        txt_len ? txt_buffer : nullptr,
        register_callback, // callback function
        nullptr  // context
    );

    if (err != kDNSServiceErr_NoError) {
        RTC_LOG(LS_ERROR) << "Bonjour advertise failed: " << err;
        switch (err) {
            case kDNSServiceErr_BadParam:
                RTC_LOG(LS_ERROR) << " (Bad parameter)"; 
                break;
            case kDNSServiceErr_NameConflict:
                RTC_LOG(LS_ERROR) << " (Name conflict)";
                break;
            case kDNSServiceErr_NoMemory:
                RTC_LOG(LS_ERROR) << " (Out of memory)";
                break;
            default:
                RTC_LOG(LS_ERROR) << " (Unknown error)";
                break;
        }
        RTC_LOG(LS_ERROR) << "Parameters: Name=" << service_name << ", Type=_webrtcsays._tcp, Port=" << port
                  << ", Host=(system default), TXT=" << (txt_len ? txt_summary : "none");
        return false;
    }

    RTC_LOG(LS_INFO) << "Bonjour advertised as '" << service_name << "' on port " << port;
    RTC_LOG(LS_INFO) << " Advertising on IP: " << ipResult.ip << " (" << (ipResult.isWiFi ? "Wi-Fi" : "Non-Wi-Fi") << ")";
    RTC_LOG(LS_INFO) << " TXT record: " << txt_summary;

    // Run the service in a background thread
    std::thread([]{
        if (g_advert_ref) {
            DNSServiceProcessResult(g_advert_ref);
        }
    }).detach();
    return true;
}

// Helper for discovery
struct BonjourDiscoveryContext {
    std::string target_name;
    std::string found_ip;
    int found_port = 0;
    std::atomic<bool> found{false};
    std::mutex mtx;
    std::condition_variable cv;
};

static void DNSSD_API resolve_callback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char *fullname,
    const char *hosttarget,
    uint16_t port,
    uint16_t txtLen,
    const unsigned char *txtRecord,
    void *context)
{
    BonjourDiscoveryContext* ctx = static_cast<BonjourDiscoveryContext*>(context);
    if (errorCode == kDNSServiceErr_NoError) {
        RTC_LOG(LS_INFO) << "[Resolve] Full name: " << (fullname ? fullname : "null");
        RTC_LOG(LS_INFO) << "[Resolve] Host target: " << (hosttarget ? hosttarget : "null");
        RTC_LOG(LS_INFO) << "[Resolve] Port: " << ntohs(port);
        
        // First try to extract IP from TXT record
        std::string ip_from_txt;
        if (txtLen > 0 && txtRecord) {
            // Parse TXT record to find ip= field
            const unsigned char* ptr = txtRecord;
            const unsigned char* end = txtRecord + txtLen;
            while (ptr < end) {
                uint8_t len = *ptr++;
                if (ptr + len <= end) {
                    std::string entry(reinterpret_cast<const char*>(ptr), len);
                    if (entry.substr(0, 3) == "ip=") {
                        std::string full_value = entry.substr(3);
                        // Extract IP part before any comma (in case of ip=x.x.x.x,ts=timestamp)
                        size_t comma_pos = full_value.find(',');
                        ip_from_txt = (comma_pos != std::string::npos) ? full_value.substr(0, comma_pos) : full_value;
                        RTC_LOG(LS_INFO) << "[Resolve] Found IP in TXT: " << ip_from_txt << " (full entry: " << entry << ")";
                        break;
                    }
                }
                ptr += len;
            }
        }
        
        if (!ip_from_txt.empty()) {
            // Use IP from TXT record
            ctx->found_ip = ip_from_txt;
            ctx->found_port = ntohs(port);
            ctx->found = true;
            ctx->cv.notify_all();
            RTC_LOG(LS_INFO) << "[Resolve] Success via TXT: " << ctx->found_ip << ":" << ctx->found_port;
        } else if (hosttarget) {
            // Fallback: resolve hosttarget to IP
            RTC_LOG(LS_INFO) << "[Resolve] Attempting to resolve hostname: " << hosttarget;
            struct addrinfo hints = {0, AF_INET, SOCK_STREAM, 0, 0, 0, 0, nullptr};
            // hints.ai_family = AF_INET;
            // hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* res = nullptr;
            if (getaddrinfo(hosttarget, nullptr, &hints, &res) == 0 && res) {
                char ipstr[INET_ADDRSTRLEN] = {0};
                struct sockaddr_in* s = (struct sockaddr_in*)res->ai_addr;
                inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
                ctx->found_ip = ipstr;
                ctx->found_port = ntohs(port);
                freeaddrinfo(res);
                ctx->found = true;
                ctx->cv.notify_all();
                RTC_LOG(LS_INFO) << "[Resolve] Success via hostname: " << ctx->found_ip << ":" << ctx->found_port;
            } else {
                RTC_LOG(LS_ERROR) << "[Resolve] Failed to resolve hostname: " << hosttarget;
            }
        }
    } else {
        RTC_LOG(LS_ERROR) << "[Resolve] Error: " << errorCode;
    }
}

static void DNSSD_API browse_callback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char *serviceName,
    const char *regtype,
    const char *replyDomain,
    void *context)
{
    BonjourDiscoveryContext* ctx = static_cast<BonjourDiscoveryContext*>(context);
    
    if (errorCode != kDNSServiceErr_NoError) {
        RTC_LOG(LS_ERROR) << "[Browse] Error: " << errorCode;
        return;
    }
    
    if (!ctx || !serviceName) {
        RTC_LOG(LS_ERROR) << "[Browse] Invalid context or service name";
        return;
    }
    
    RTC_LOG(LS_INFO) << "[Browse] Found service: '" << serviceName << "' in domain: " << (replyDomain ? replyDomain : "null");
    RTC_LOG(LS_INFO) << "[Browse] Looking for: '" << ctx->target_name << "'";
    
    // Check if this is the service we're looking for
    bool isTargetService = (ctx->target_name == serviceName);
    
    if (isTargetService || ctx->target_name.empty()) {
        RTC_LOG(LS_INFO) << "[Browse] " << (isTargetService ? "Target service found" : "Resolving first available service");
        
        DNSServiceRef resolveRef = nullptr;
        DNSServiceErrorType err = DNSServiceResolve(
            &resolveRef,
            kDNSServiceFlagsForceMulticast, // Force multicast to bypass cache
            interfaceIndex,
            serviceName,
            regtype,
            replyDomain,
            resolve_callback,
            context);
            
        if (err == kDNSServiceErr_NoError) {
            RTC_LOG(LS_INFO) << "[Browse] Starting resolution...";
            // Process the resolve operation with timeout
            auto start_time = std::chrono::steady_clock::now();
            while (!ctx->found && 
                   std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
                DNSServiceErrorType processErr = DNSServiceProcessResult(resolveRef);
                if (processErr != kDNSServiceErr_NoError) {
                    RTC_LOG(LS_ERROR) << "[Browse] Process result error: " << processErr;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            DNSServiceRefDeallocate(resolveRef);
            
            if (ctx->found) {
                RTC_LOG(LS_INFO) << "[Browse] Resolution completed successfully";
            } else {
                RTC_LOG(LS_ERROR) << "[Browse] Resolution timed out";
            }
        } else {
            RTC_LOG(LS_ERROR) << "[Browse] DNSServiceResolve failed: " << err;
        }
    } else {
        RTC_LOG(LS_INFO) << "[Browse] Skipping service (not target): " << serviceName;
    }
}

bool DIRECT_API DiscoverBonjourService(const std::string& name, std::string& out_ip, int& out_port, int timeout_seconds) {
    RTC_LOG(LS_INFO) << "[Discovery] Starting Bonjour discovery for: '" << name << "' (timeout: " << timeout_seconds << "s)";
    
    BonjourDiscoveryContext ctx;
    ctx.target_name = name;
    DNSServiceRef browseRef = nullptr;
    
    DNSServiceErrorType err = DNSServiceBrowse(
        &browseRef,
        kDNSServiceFlagsForceMulticast, // Force multicast to bypass cache
        0,
        "_webrtcsays._tcp",
        nullptr,
        browse_callback,
        &ctx);
        
    if (err != kDNSServiceErr_NoError) {
        RTC_LOG(LS_ERROR) << "[Discovery] Bonjour browse failed: " << err;
        return false;
    }
    
    RTC_LOG(LS_INFO) << "[Discovery] Browse started, waiting for results...";
    
    // Wait for result or timeout
    int effective_timeout = timeout_seconds > 0 ? timeout_seconds : 15;
    auto start = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::seconds(effective_timeout);
    
    while (!ctx.found && std::chrono::steady_clock::now() - start < timeout_duration) {
        DNSServiceErrorType processErr = DNSServiceProcessResult(browseRef);
        if (processErr != kDNSServiceErr_NoError) {
            RTC_LOG(LS_ERROR) << "[Discovery] Process result error: " << processErr;
            break;
        }
        
        // Short sleep to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Check if we found something
        if (ctx.found) {
            RTC_LOG(LS_INFO) << "[Discovery] Service found and resolved!";
            break;
        }
    }
    
    DNSServiceRefDeallocate(browseRef);
    
    if (ctx.found) {
        out_ip = ctx.found_ip;
        out_port = ctx.found_port;
        RTC_LOG(LS_INFO) << "[Discovery] Success: " << out_ip << ":" << out_port;
        return true;
    } else {
        RTC_LOG(LS_ERROR) << "[Discovery] Timeout: No service found within " << effective_timeout << " seconds";
        return false;
    }
}

#endif // #if TARGET_OS_IOS || TARGET_OS_OSX