#ifndef EXAMPLES_DIRECT_PARSER_H_
#define EXAMPLES_DIRECT_PARSER_H_

#include <string>
#include <json/json.h>
#include <memory>

// Utility that splits a signaling message into a command and an optional JSON
// parameter object.
//   COMMAND                  (no parameters)
//   COMMAND:{ ...json... }   (parameters)
// Returns true on success (including missing JSON). False when JSON is
// present but malformed.
inline bool ParseMessageLine(const std::string& message,
                             std::string&       out_command,
                             Json::Value&       out_params) {
    out_command.clear();
    out_params = Json::Value();

    // Identify end of command: first space or colon
    size_t idx = message.find_first_of(" :");
    if (idx == std::string::npos) {
        out_command = message; // whole string is command
        return true;
    }

    out_command = message.substr(0, idx);

    // Skip any combination of spaces and a single optional colon
    size_t pos = idx;
    while (pos < message.size() && message[pos] == ' ') ++pos;
    if (pos < message.size() && message[pos] == ':') ++pos;
    while (pos < message.size() && message[pos] == ' ') ++pos;

    std::string payload = message.substr(pos);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end   = s.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) {
            s.clear();
        } else {
            s = s.substr(start, end - start + 1);
        }
    };
    trim(payload);

    if (!payload.empty() && payload.front() == '{') {
        Json::CharReaderBuilder builder;
        std::string errs;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        if (!reader->parse(payload.data(), payload.data() + payload.size(), &out_params, &errs)) {
            return false; // malformed JSON
        }
    }
    return true;
}

#endif // EXAMPLES_DIRECT_PARSER_H_ 