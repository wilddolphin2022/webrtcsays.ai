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

#include <fstream>     // For file operations
#include <cerrno>      // For errno used with strtol

#include "api/jsep.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/system/rtc_export.h"
#include "rtc_base/crypto_random.h"

#include <json/json.h> // Use jsoncpp header
#include <algorithm>
#include <unordered_set>
#include <random>
#include <sstream>
#include <iomanip>

// Utility to remove surrounding single or double quotes from a string.
static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

#include "direct.h"
#include "option.h"

// Function to parse IP address and port from a string in the format "IP:PORT"
bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);
// String split
std::vector<std::string> stringSplit(std::string input, std::string delimiter);

// Helper function to expand ${HOME} in paths
namespace {
std::string expandHomePath(const std::string& path) {
    if (path.rfind("${HOME}", 0) == 0) { // Check if path starts with ${HOME}
        const char* home_dir = std::getenv("HOME");
        if (home_dir) {
            std::string expanded_path = home_dir;
            // Append the rest of the path, handling the optional separator
            if (path.length() > 7 && (path[7] == '/' || path[7] == '\\')) {
                expanded_path += path.substr(7); // Skip ${HOME} and the separator
            } else if (path.length() > 7) { 
                 expanded_path += "/"; // Add separator if missing
                 expanded_path += path.substr(7); // Skip ${HOME}
            } else { 
                 // Path was just "${HOME}", nothing to append
            }
            RTC_LOG(LS_INFO) << "Expanded path: " << expanded_path;
            return expanded_path;
        } else {
            RTC_LOG(LS_WARNING) << "HOME environment variable not set, cannot expand path: " << path;
            return path; // Return original path if HOME is not set
        }
    } 
    return path; // Return original path if it doesn't start with ${HOME}
}
} // namespace

// Function to parse IP address and port from a string in the format "IP:PORT"
bool DIRECT_API ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port) {
  size_t colon_pos = ip_port.find(':');
  if (colon_pos == std::string::npos) {
    RTC_LOG(LS_ERROR) << "Invalid IP:PORT format: " << ip_port;
    return false;
  }

  ip = ip_port.substr(0, colon_pos);
  std::string port_str = ip_port.substr(colon_pos + 1);
  
  // Use strtol for exception-safe conversion
  const char* start_ptr = port_str.c_str();
  char* end_ptr = nullptr;
  errno = 0; // Reset errno
  long port_val = strtol(start_ptr, &end_ptr, 10);

  // Check for conversion errors and that the whole string was consumed
  if (errno != 0 || end_ptr == start_ptr || *end_ptr != '\0') {
    RTC_LOG(LS_ERROR) << "Invalid port string: " << port_str;
    return false;
  }

  // Check port range
  if (port_val < 0 || port_val > 65535) {
    RTC_LOG(LS_ERROR) << "Invalid port range: " << port_val;
    return false;
  }

  port = static_cast<int>(port_val); // Assign the valid port
  return true;
}

// Basic split that honours quotes – only used for cmd-line string variant.
std::vector<std::string> stringSplit(std::string input, std::string delimiter)
{
    if (delimiter != " ") {
        // retain old simple behaviour for non-space delimiter callers.
        std::vector<std::string> tokens;
        size_t pos = 0;
        std::string token;
        while((pos = input.find(delimiter)) != std::string::npos){
            token = input.substr(0, pos);
            tokens.push_back(token);
            input.erase(0, pos + delimiter.size());
        }
        tokens.push_back(input);
        return tokens;
    }

    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '"') {
            in_quotes = !in_quotes;
            continue; // drop the quote char itself
        }
        if (c == ' ' && !in_quotes) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

DIRECT_API Options parseOptions(const char* argString) {
  std::vector<std::string> args = stringSplit(argString, " ");
  return parseOptions(args);
}

// Function to parse command line string to above options
Options parseOptions(const std::vector<std::string>& args) {
  Options opts;
  // Initialize defaults (ensure all relevant defaults are set here)
  opts.encryption = false;
  opts.whisper = false;
  opts.llama = false; // Initialize llama default
  opts.video = false; // Assuming default is false
  opts.mode = ""; 
  opts.webrtc_cert_path = "cert.pem";
  opts.webrtc_key_path = "key.pem";
  opts.webrtc_speech_initial_playout_wav = "play.wav";
  opts.help = false;
  // Restore original help string
  opts.help_string =
      "Usage:\n"
      "directcall [options] address [options]\n\n"
      "Options:\n"
      "  --config <path>                  Load options from JSON config file.\n"
      "                                     Command-line options override config file.\n"
      "  --mode <caller|callee>             Set operation mode (default: caller)\n"
      "  --encryption, --no-encryption      Enable/disable encryption (default: disabled)\n"
      "  --whisper, --no-whisper            Enable/disable whisper (default: no-whisper)\n"
      "  --video, --no-video                Enable/disable video (default: disabled)\n" // Added video help
      "  --whisper_model=<path>             Path to whisper model\n"
      "  --llama_model=<path>               Path to llama model\n"
      "  --llava_mmproj=<path>              Path to llava mmproj model\n"
      "  --llama_llava_yuv=<path>;<width>x<height>  Path to llava yuv model, width, height\n"
      "  --webrtc_cert_path=<path>          Path to WebRTC certificate (default: 'cert.pem')\n"
      "  --webrtc_key_path=<path>           Path to WebRTC key (default: 'key.pem')\n"
      "  --turns=<ip,username,password>     Secured turn server address, e.g. \n"
      "   'turns:global.relay.metered.ca:443?transport=tcp,<username>,<password>'\n"
      "  --vpn=<interface_name>             Specify VPN interface name\n" // Added VPN help
      "  --camera=<device_name>             Specify camera device name\n"
      "  --talking_face=<image_path>        Avatar image for animated AI face\n"
      "  --bonjour_name=<name>              Specify bonjour name\n" // Added bonjour name resolution
      "  --user_name=<name>                 Your user name for registration\n"
      "  --target_name=<name>               Target user name to call (caller mode)\n"
      "  --room_name=<name>                 Room name to join (default: room1)\n"
      "  --help                             Show this help message\n\n"
      "Examples (callee called first, encryption is recommended):\n"
      "  directcall --config settings.json\n"
      "  directcall --config settings.json --mode=callee --no-encryption\n"
      "  directcall --mode=callee --user_name=alice --room_name=meeting1 192.168.88.225:3456 --no-encryption\n"
      "  directcall --mode=callee --user_name=bob --room_name=meeting1 192.168.88.225:3456 --encryption --whisper"
      " --whisper_model=/path/to/model.bin --llama_model=/path/to/llama.gguf\n"
      "  directcall --mode=caller --user_name=charlie --target_name=alice --room_name=room101 192.168.88.225:3456 --encryption\n"
      ;

  // Set of known options (with and without =)
  const std::unordered_set<std::string> known_options = {
    "--config", "--mode", "--encryption", "--no-encryption", "--whisper", "--no-whisper",
    "--video", "--no-video", "--whisper_model", "--llama_model", "--llava_mmproj",
    "--webrtc_cert_path", "--webrtc_key_path", "--webrtc_speech_initial_playout_wav",
    "--llama_llava_yuv", "--turns", "--vpn", "--camera", "--bonjour_name",
    "--user_name", "--target_name", "--room_name", "--help"
  };

  // --- First pass: check for --config --- 
  for (size_t i = 0; i < args.size(); ++i) { // Use size_t for indexing vector
    const std::string& arg = args[i]; // Get argument by reference
    if (arg == "--config" && i + 1 < args.size()) {
      opts.config_path = args[i + 1];
      break; // Found config, stop first pass
    } else if (arg.find("--config=") == 0) {
      opts.config_path = arg.substr(9);
      break; // Found config, stop first pass
    } else if (arg == "--help") {
      opts.help = true;
      return opts;
    }
  }
  
  // --- Load from config file if specified ---
  // --- Re-enable JSON loading ---
  if (!opts.config_path.empty()) {
    // Use C-style file I/O to avoid potential std::ifstream issues
    FILE* fp = fopen(opts.config_path.c_str(), "rb");
    if (fp) {
      RTC_LOG(LS_VERBOSE) << "JSON C-IO: Config file opened successfully.";
      std::string contents;
      fseek(fp, 0, SEEK_END);
      contents.resize(ftell(fp));
      rewind(fp);
      size_t bytes_read = fread(&contents[0], 1, contents.size(), fp);
      fclose(fp);
      if(bytes_read == contents.size())
        RTC_LOG(LS_VERBOSE) << "JSON C-IO: Read " << contents.size() << " bytes.";

      Json::Value config_json;
      Json::CharReaderBuilder reader_builder;
      std::string errs;
      std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
      bool parsingSuccessful = reader->parse(
          contents.data(), contents.data() + contents.size(), 
          &config_json, &errs);
      RTC_LOG(LS_VERBOSE) << "JSON C-IO: Parsing result: " << (parsingSuccessful ? "Success" : "Failure"); // Log parse result

      if (parsingSuccessful) {
        RTC_LOG(LS_VERBOSE) << "JSON C-IO: Parsing succeeded. Checking members..."; // Log before member checks

        // Explicitly parse each option without macros
        // Strings
        if (config_json.isMember("mode") && config_json["mode"].isString()) {
             RTC_LOG(LS_INFO) << "Config mode: " << config_json["mode"].asString(); // Log value
             opts.mode = config_json["mode"].asString(); // Direct assignment (Uncommented)
        }
        if (config_json.isMember("whisper_model") && config_json["whisper_model"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found whisper_model: " << config_json["whisper_model"].asString(); // Log value
             opts.whisper_model = expandHomePath(config_json["whisper_model"].asString());
        }
        if (config_json.isMember("llama_model") && config_json["llama_model"].isString()) {
             RTC_LOG(LS_INFO) << "Config llama_model: " << config_json["llama_model"].asString(); // Log value
             opts.llama_model = expandHomePath(config_json["llama_model"].asString());
        }
        if (config_json.isMember("llava_mmproj") && config_json["llava_mmproj"].isString()) {
             RTC_LOG(LS_INFO) << "Config llava_mmproj: " << config_json["llava_mmproj"].asString(); // Log value
             opts.llava_mmproj = expandHomePath(config_json["llava_mmproj"].asString());
        }
        if (config_json.isMember("webrtc_cert_path") && config_json["webrtc_cert_path"].isString()) {
             RTC_LOG(LS_INFO) << "Config webrtc_cert_path: " << config_json["webrtc_cert_path"].asString(); // Log value
             opts.webrtc_cert_path = expandHomePath(config_json["webrtc_cert_path"].asString());
             RTC_LOG(LS_INFO) << "Expanded webrtc_cert_path: " << opts.webrtc_cert_path;
        }
        if (config_json.isMember("webrtc_key_path") && config_json["webrtc_key_path"].isString()) {
             RTC_LOG(LS_INFO) << "Config webrtc_key_path: " << config_json["webrtc_key_path"].asString(); // Log value
             opts.webrtc_key_path = expandHomePath(config_json["webrtc_key_path"].asString());
             RTC_LOG(LS_INFO) << "Expanded webrtc_key_path: " << opts.webrtc_key_path;
        }
        if (config_json.isMember("webrtc_speech_initial_playout_wav") && config_json["webrtc_speech_initial_playout_wav"].isString()) {
             RTC_LOG(LS_INFO) << "Config initial playout_wav: " << config_json["webrtc_speech_initial_playout_wav"].asString(); // Log value
             opts.webrtc_speech_initial_playout_wav = expandHomePath(config_json["webrtc_speech_initial_playout_wav"].asString());
        }
        if (config_json.isMember("llama_llava_yuv") && config_json["llama_llava_yuv"].isString()) {
             RTC_LOG(LS_INFO) << "Config llama_llava_yuv: " << config_json["llama_llava_yuv"].asString(); // Log value
             std::vector<std::string> yuv_dimensions = stringSplit(config_json["llama_llava_yuv"].asString(), ";");
             opts.llama_llava_yuv = expandHomePath(yuv_dimensions[0]);
             std::vector<std::string> yuv_width_height = stringSplit(yuv_dimensions[1], "x");
             opts.llama_llava_yuv_width = std::stoi(yuv_width_height[0]);
             opts.llama_llava_yuv_height = std::stoi(yuv_width_height[1]);
        }
        if (config_json.isMember("address") && config_json["address"].isString()) {
             RTC_LOG(LS_INFO) << "Config address: " << config_json["address"].asString(); // Log value
             opts.address = config_json["address"].asString();
        }
        if (config_json.isMember("turns")) {
            const Json::Value& t = config_json["turns"];
            if (t.isString()) {
                // Simple string "turn:host:port,user,pass"
                RTC_LOG(LS_INFO) << "Config has turns params (string)";
                opts.turns = t.asString();
            } else if (t.isArray()) {
                // Expect [ uri, username, password ]
                if (t.size() >= 3 && t[0].isString() && t[1].isString() && t[2].isString()) {
                    std::string joined;
                    for (Json::ArrayIndex i = 0; i < t.size(); ++i) {
                        if (!t[i].isString()) {
                            RTC_LOG(LS_WARNING) << "turns array element " << i << " is not string – skipping";
                            continue;
                        }
                        if (!joined.empty()) joined += ";";
                        joined += t[i].asString();
                    }
                    opts.turns = joined;
                    RTC_LOG(LS_INFO) << "Config turns array joined to: " << opts.turns;
                } else {
                    RTC_LOG(LS_WARNING) << "`turns` array has unexpected structure; expecting at least 3 string elements (uri, user, pass). Ignored.";
                }
            } else {
                RTC_LOG(LS_WARNING) << "`turns` is neither string nor array – ignored";
            }
        }
        if (config_json.isMember("vpn") && config_json["vpn"].isString()) {
             RTC_LOG(LS_INFO) << "Config vpn: " << config_json["vpn"].asString(); // Log value
             opts.vpn = config_json["vpn"].asString();
        }
        if (config_json.isMember("camera") && config_json["camera"].isString()) {
             RTC_LOG(LS_INFO) << "Config camera: " << config_json["camera"].asString();
             opts.camera = config_json["camera"].asString();
        }
        if (config_json.isMember("talking_face") && config_json["talking_face"].isString()) {
             opts.talking_face = expandHomePath(config_json["talking_face"].asString());
             opts.video = true;
        }
        if (config_json.isMember("bonjour_name") && config_json["bonjour_name"].isString()) {
             RTC_LOG(LS_INFO) << "Config bonjour_name: " << config_json["bonjour_name"].asString();
             opts.bonjour_name = config_json["bonjour_name"].asString();
        }
        if (config_json.isMember("user_name") && config_json["user_name"].isString()) {
             RTC_LOG(LS_INFO) << "Config user_name: " << config_json["user_name"].asString();
             opts.user_name = config_json["user_name"].asString();
        }
        if (config_json.isMember("target_name") && config_json["target_name"].isString()) {
             RTC_LOG(LS_INFO) << "Config target_name: " << config_json["target_name"].asString();
             opts.target_name = config_json["target_name"].asString();
        }
        if (config_json.isMember("room_name") && config_json["room_name"].isString()) {
             RTC_LOG(LS_INFO) << "Config room_name: " << config_json["room_name"].asString();
             opts.room_name = config_json["room_name"].asString();
        }
        if (config_json.isMember("language") && config_json["language"].isString()) {
             RTC_LOG(LS_INFO) << "Config language: " << config_json["language"].asString();
             opts.language = config_json["language"].asString();
        }
        if (config_json.isMember("whispers")) {
            const Json::Value& w = config_json["whispers"];

            if (w.isArray()) {                       // simple  ["foo","bar"]
                for (const auto& v : w) {
                    if (v.isString()) opts.whispers.push_back(v.asString());
                }
            } else if (w.isObject()) {               // i18n  { "en":[...], "zh":[...] }
                for (const auto& lang : w.getMemberNames()) {
                    const Json::Value& arr = w[lang];
                    if (!arr.isArray()) continue;
                    for (const auto& v : arr) {
                        if (v.isString()) opts.whispers.push_back(v.asString());
                    }
                }
            } else {
                RTC_LOG(LS_WARNING) << "`whispers` is neither array nor object – ignored";
            }
        }
        // Thread counts (0 = auto)
        if (config_json.isMember("whisper_threads") && config_json["whisper_threads"].isInt()) {
             opts.whisper_threads = config_json["whisper_threads"].asInt();
             RTC_LOG(LS_INFO) << "Config whisper_threads: " << opts.whisper_threads;
        }
        if (config_json.isMember("llama_threads") && config_json["llama_threads"].isInt()) {
             opts.llama_threads = config_json["llama_threads"].asInt();
             RTC_LOG(LS_INFO) << "Config llama_threads: " << opts.llama_threads;
        }
        if (config_json.isMember("tts_threads") && config_json["tts_threads"].isInt()) {
             opts.tts_threads = config_json["tts_threads"].asInt();
             RTC_LOG(LS_INFO) << "Config tts_threads: " << opts.tts_threads;
        }
        // Booleans
        if (config_json.isMember("encryption") && config_json["encryption"].isBool()) {
             RTC_LOG(LS_INFO) << "Config encryption: " << config_json["encryption"].asBool(); // Log value
             opts.encryption = config_json["encryption"].asBool();
        }
        if (config_json.isMember("whisper") && config_json["whisper"].isBool()) {
             RTC_LOG(LS_INFO) << "Config whisper: " << config_json["whisper"].asBool(); // Log value
             opts.whisper = config_json["whisper"].asBool();
        }
        if (config_json.isMember("llama") && config_json["llama"].isBool()) {
             RTC_LOG(LS_INFO) << "Config llama: " << config_json["llama"].asBool(); // Log value
             opts.llama = config_json["llama"].asBool();
        }
        if (config_json.isMember("video") && config_json["video"].isBool()) {
             RTC_LOG(LS_INFO) << "Config video: " << config_json["video"].asBool(); // Log value
             opts.video = config_json["video"].asBool();
        }

        RTC_LOG(LS_INFO) << "Loaded options from config file: " << opts.config_path;
      } else {
        RTC_LOG(LS_ERROR) << "Failed to parse config file " << opts.config_path << " from buffer: " << errs;
      }
    } else {
      RTC_LOG(LS_WARNING) << "Could not open config file with fopen: " << opts.config_path;
    }
  }
  // End of JSON loading

  // Helper function to check if string is an address
  auto isAddress = [](const std::string& str) {
    // Allow simple port numbers like ":3478"
    if (str.length() > 1 && str[0] == ':') {
        const char* start_ptr = str.c_str() + 1;
        char* end_ptr = nullptr;
        errno = 0; // Reset errno before calling strtol
        long port_val = strtol(start_ptr, &end_ptr, 10);
        // Check for conversion errors (errno != 0), 
        // ensure the whole string was consumed (*end_ptr == '\0'),
        // and check if the port is in the valid range.
        if (errno == 0 && end_ptr != start_ptr && *end_ptr == '\0' && port_val >= 0 && port_val <= 65535) {
            return true; // Looks like a valid port number
        } else {
            return false; // Conversion failed or invalid port range
        }
    }
    // Check for standard IP:PORT or hostname:PORT
    size_t colon_pos = str.find(':');
    return colon_pos != std::string::npos && colon_pos > 0 && colon_pos < str.length() - 1;
  };

  // --- Second pass: parse command-line arguments (overriding config) ---
  for (size_t i = 0; i < args.size(); ++i) { // Use size_t for indexing vector
    const std::string& arg = args[i]; // Get argument by reference

    // Skip the --config argument itself in the second pass
    if (arg == "--config" && i + 1 < args.size()) {
      i++; // Skip the path value as well
      continue;
    } else if (arg.find("--config=") == 0) {
      continue;
    }

    // Handle parameters with values
    if (arg.find("--mode=") == 0) {
      opts.mode = arg.substr(7);
    } else if (arg.find("--whisper_model=") == 0) {
      opts.whisper_model = arg.substr(16);  // Length of "--whisper_model="
    } else if (arg.find("--llama_model=") == 0) {
      opts.llama_model = arg.substr(14);  // Length of "--llama_model="
    } else if (arg.find("--llava_mmproj=") == 0) {
      opts.llava_mmproj = arg.substr(14);  // Length of "--llava_mmproj="
    } else if (arg.find("--webrtc_cert_path=") == 0) {
      opts.webrtc_cert_path = arg.substr(19);
    } else if (arg.find("--webrtc_key_path=") == 0) {
      opts.webrtc_key_path = arg.substr(18);
    } else if (arg.find("--webrtc_speech_initial_playout_wav=") == 0) {
      opts.webrtc_speech_initial_playout_wav = arg.substr(36);
    } else if (arg.find("--llama_llava_yuv=") == 0) {
      opts.llama_llava_yuv = arg.substr(16);
    } else if (arg.find("--turns=") == 0) {
      opts.turns = arg.substr(8);
      // Remove single quotes if present
      opts.turns.erase(remove(opts.turns.begin(), opts.turns.end(), '\''), opts.turns.end());
    } else if (arg.find("--vpn=") == 0) { 
        opts.vpn = arg.substr(6);
    } else if (arg.find("--camera=") == 0) {
        opts.camera = stripQuotes(arg.substr(9));
    } else if (arg.find("--talking_face=") == 0) {
        opts.talking_face = expandHomePath(stripQuotes(arg.substr(15)));
        opts.video = true;
    } else if (arg.find("--bonjour_name=") == 0) {
        opts.bonjour_name = arg.substr(15);
    } else if (arg.find("--user_name=") == 0) {
        opts.user_name = arg.substr(12);
    } else if (arg.find("--target_name=") == 0) {
        opts.target_name = arg.substr(14);
    } else if (arg.find("--room_name=") == 0) {
        opts.room_name = arg.substr(12);
    }
    // Handle flags
    if (arg == "--encryption") {
      RTC_LOG(LS_INFO) << "Args set encryption on";
      opts.encryption = true;
    } else if (arg == "--no-encryption") {
      RTC_LOG(LS_INFO) << "Args set encryption off";
      opts.encryption = false;
    } else if (arg == "--whisper") {
      RTC_LOG(LS_INFO) << "Args set whisper on";
      opts.whisper = true;
    } else if (arg == "--no-whisper") {
      RTC_LOG(LS_INFO) << "Args set whisper off";
      opts.whisper = false;
    } else if (arg == "--llama") {
      RTC_LOG(LS_INFO) << "Args set llama on";
      opts.llama = true;
    } else if (arg == "--no-llama") {
      RTC_LOG(LS_INFO) << "Args set llama off";
      opts.llama = false;
    } else if (arg == "--video") { 
      RTC_LOG(LS_INFO) << "Args set video on";
      opts.video = true;
    } else if (arg == "--no-video") { 
      RTC_LOG(LS_INFO) << "Args set video off";
      opts.video = false; 
    } else if (arg == "--bonjour") { 
      RTC_LOG(LS_INFO) << "Args set bonjour on";
      opts.video = true;
    } else if (arg == "--no-bonjour") { 
      RTC_LOG(LS_INFO) << "Args set bonjour off";
      opts.video = false;
    }
    // Handle address in any position (must not be another known flag/option)
    else if (arg.rfind("--", 0) != 0 && isAddress(arg)) {
       opts.address = arg;
       // Only guess mode if not explicitly set or loaded from config
       if (opts.mode.empty()) {
           // Guess caller if it contains '.', otherwise callee (including simple port)
           opts.mode = (arg.find('.') != std::string::npos) ? "caller" : "callee";
       }
    } else if (arg.rfind("--", 0) == 0) {
        // Only warn if not a known option
        std::string opt_name = arg.substr(0, arg.find('=') != std::string::npos ? arg.find('=') : arg.length());
        if (known_options.find(opt_name) == known_options.end()) {
            RTC_LOG(LS_WARNING) << "Unknown option: " << arg;
        }
    } else {
       // Treat as address if not starting with -- and isAddress passed? (Covered above)
       // Or potentially handle positional arguments if needed
    }
  }
  
  RTC_LOG(LS_VERBOSE) << "After cmd line pass - opts.whisper: " << opts.whisper; // Log after 2nd pass

  // Final check: If mode is still empty after parsing everything, default to caller
  if(opts.mode.empty()) {
      opts.mode = "caller";
  }

  // Auto-generate user_name if missing
  if (opts.user_name.empty()) {
    static const char* names[] = {"alice", "bob", "carl", "dave", "eve", "frank", "grace", "heidi", "ivan", "judy"};
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> name_dist(0, sizeof(names)/sizeof(names[0]) - 1);
    std::uniform_int_distribution<> hex_dist(0, 15);
    std::ostringstream suffix;
    suffix << "-";
    for (int i = 0; i < 4; ++i) suffix << std::hex << hex_dist(gen);
    opts.user_name = std::string(names[name_dist(gen)]) + suffix.str();
    RTC_LOG(LS_INFO) << "Auto-generated user_name: " << opts.user_name;
  }

  // Restore environment variable handling
  // Load environment variables if paths not provided by command line or config
  // (Note: env vars are now lowest priority)
  if (opts.webrtc_cert_path.empty() || opts.webrtc_cert_path == "cert.pem") { // Check against default too
    if (const char* env_cert = std::getenv("WEBRTC_CERT_PATH")) {
      opts.webrtc_cert_path = env_cert;
    }
  }
  if (opts.webrtc_key_path.empty() || opts.webrtc_key_path == "key.pem") { // Check against default too
    if (const char* env_key = std::getenv("WEBRTC_KEY_PATH")) {
      opts.webrtc_key_path = env_key;
    }
  }
  if (opts.webrtc_speech_initial_playout_wav.empty() || opts.webrtc_speech_initial_playout_wav == "play.wav") { // Check against default too
    if (const char* env_wav =
            std::getenv("WEBRTC_SPEECH_INITIAL_PLAYOUT_WAV")) {
      opts.webrtc_speech_initial_playout_wav = env_wav;
    }
  }
  if (opts.whisper_model.empty()) {
    if (const char* env_whisper = std::getenv("WHISPER_MODEL")) {
      opts.whisper_model = env_whisper;
    }
  }
  // Set env var if whisper model is set (potentially from config or cmd line)
  if (!opts.whisper_model.empty()) {
    setenv("WHISPER_MODEL", opts.whisper_model.c_str(), 1); // Use 1 to overwrite
  }

  if (opts.llama_model.empty()) {
    if (const char* env_llama = std::getenv("LLAMA_MODEL")) {
      opts.llama_model = env_llama;
    }
  }
   // Set env var if llama model is set (potentially from config or cmd line)
  if (!opts.llama_model.empty()) {
    setenv("LLAMA_MODEL", opts.llama_model.c_str(), 1); // Use 1 to overwrite
  }
  if (opts.vpn.empty()) { 
    if (const char* env_vpn = std::getenv("VPN")) {
      opts.vpn = env_vpn;
    }
  }

  RTC_LOG(LS_INFO) << "Mode used " << opts.mode; // Log final mode value
  return opts;
}

std::string getUsage(const Options opts) {
  std::stringstream usage;

  usage << "\\n--- Current Settings ---\\n"; // Header for clarity
  usage << "Mode: " << opts.mode << "\\n";
  usage << "Address: " << opts.address << "\\n";
  usage << "Encryption: " << (opts.encryption ? "enabled" : "disabled") << "\\n";
  usage << "Video: " << (opts.video ? "enabled" : "disabled") << "\\n";
  usage << "Whisper: " << (opts.whisper ? "enabled" : "disabled") << "\\n";
  if (opts.whisper) { // Only show model paths if whisper is enabled
      usage << "Whisper Model: " << (!opts.whisper_model.empty() ? opts.whisper_model : "(not set)") << "\\n";
      usage << "Llama Model: " << (!opts.llama_model.empty() ? opts.llama_model : "(not set)") << "\\n";
      usage << "Llava MMProj: " << (!opts.llava_mmproj.empty() ? opts.llava_mmproj : "(not set)") << "\\n";
  }
  usage << "WebRTC Cert Path: " << opts.webrtc_cert_path << "\\n";
  usage << "WebRTC Key Path: " << opts.webrtc_key_path << "\\n";
  usage << "WebRTC Speech Initial Playout WAV: "
        << opts.webrtc_speech_initial_playout_wav << "\\n";
  usage << "TURN Server: " << (!opts.turns.empty() ? opts.turns : "(not set)") << "\\n";
  usage << "VPN Interface: " << (!opts.vpn.empty() ? opts.vpn : "(not set)") << "\\n";
  usage << "Camera: " << (!opts.camera.empty() ? opts.camera : "(not set)") << "\\n";
  if (!opts.config_path.empty()) {
      usage << "Config File Used: " << opts.config_path << "\\n";
  }
   usage << "------------------------\\n"; // Footer

  return usage.str();
}

// LOGGING

void DirectSetLoggingLevel(LoggingSeverity level) {
  rtc::LogMessage::LogToDebug(static_cast<rtc::LoggingSeverity>(level));
}

// CERTIFICATES

// Function to create a self-signed certificate
rtc::scoped_refptr<rtc::RTCCertificate> DirectCreateCertificate() {
  auto key_params = rtc::KeyParams::RSA(2048);  // Use RSA with 2048-bit key
  auto identity = rtc::SSLIdentity::Create("webrtc", key_params);
  if (!identity) {
    RTC_LOG(LS_ERROR) << "Failed to create SSL identity";
    return nullptr;
  }
  return rtc::RTCCertificate::Create(std::move(identity));
}

// Function to read a file into a string
std::string DirectReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    RTC_LOG(LS_ERROR) << "Failed to open file: " << path;
    return "";
  }
  std::ostringstream oss;
  oss << file.rdbuf();
  return oss.str();
}

// Function to load a certificate from PEM files
rtc::scoped_refptr<rtc::RTCCertificate> DirectLoadCertificate(
    const std::string& cert_path,
    const std::string& key_path) {
  // Read the certificate and key files
  std::string cert_pem = DirectReadFile(cert_path);
  std::string key_pem = DirectReadFile(key_path);

  if (cert_pem.empty() || key_pem.empty()) {
    RTC_LOG(LS_ERROR) << "Failed to read certificate or key file";
    return nullptr;
  }

  // Log the PEM strings for debugging
  RTC_LOG(LS_VERBOSE) << "Certificate PEM:\n" << cert_pem;
  RTC_LOG(LS_VERBOSE) << "Private Key PEM:\n" << key_pem;

  // Create an SSL identity from the PEM strings
  auto identity = rtc::SSLIdentity::CreateFromPEMStrings(key_pem, cert_pem);
  if (!identity) {
    RTC_LOG(LS_ERROR) << "Failed to create SSL identity from PEM strings";
    return nullptr;
  }

  return rtc::RTCCertificate::Create(std::move(identity));
}

// Function to load certificate from environment variables or fall back to
// CreateCertificate
DIRECT_API rtc::scoped_refptr<rtc::RTCCertificate> DirectLoadCertificateFromEnv(Options opts) {
  RTC_LOG(LS_INFO) << "DirectLoadCertificateFromEnv called with cert_path: '" << opts.webrtc_cert_path << "', key_path: '" << opts.webrtc_key_path << "'";
  
  // Get paths from environment variables
  const char* cert_path = opts.webrtc_cert_path.empty()
                              ? std::getenv("WEBRTC_CERT_PATH")
                              : opts.webrtc_cert_path.c_str();
  const char* key_path = opts.webrtc_key_path.empty()
                             ? std::getenv("WEBRTC_KEY_PATH")
                             : opts.webrtc_key_path.c_str();

  RTC_LOG(LS_INFO) << "Using cert_path: " << (cert_path ? cert_path : "NULL") << ", key_path: " << (key_path ? key_path : "NULL");

  if (cert_path && key_path) {
    RTC_LOG(LS_INFO) << "Loading certificate from " << cert_path << " and "
                     << key_path;
    auto certificate = DirectLoadCertificate(cert_path, key_path);
    if (certificate) {
      return certificate;
    }
    RTC_LOG(LS_WARNING) << "Failed to load certificate from files; falling "
                           "back to CreateCertificate";
  } else {
    RTC_LOG(LS_WARNING)
        << "Environment variables WEBRTC_CERT_PATH and WEBRTC_KEY_PATH not "
           "set; falling back to CreateCertificate";
  }

  // Fall back to CreateCertificate
  return DirectCreateCertificate();
}

DIRECT_API uint32_t DirectCreateRandomId() {
  return rtc::CreateRandomId();
}

DIRECT_API std::string DirectCreateRandomUuid() {
  return rtc::CreateRandomUuid();
}

DIRECT_API void DirectThreadSetName(rtc::Thread* thread, const char* name) {
  if(thread && name && *name) {
    thread->SetName(name, nullptr);
  }
}

DIRECT_API webrtc::IceCandidateInterface* DirectCreateIceCandidate(
  const char* sdp_mid,
  int sdp_mline_index,
  const char* sdp,
  webrtc::SdpParseError* error) {
  
  std::string _sdp_mid = std::string(sdp_mid);
  std::string _sdp = std::string(sdp);
  return webrtc::CreateIceCandidate(_sdp_mid, sdp_mline_index, _sdp, error);                                                   
}


DIRECT_API std::unique_ptr<webrtc::SessionDescriptionInterface> DirectCreateSessionDescription(
    webrtc::SdpType type,
    const char* sdp,
    webrtc::SdpParseError* error) {

  std::string remote_sdp = std::string(sdp);
  return webrtc::CreateSessionDescription(type, remote_sdp, error);
}
