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

#ifndef EXAMPLES_DIRECT_STATUS_H_
#define EXAMPLES_DIRECT_STATUS_H_

#include <regex>
#include <string>

namespace StatusCodes {

// -------------------------
// SIP-style status responses
// -------------------------
inline constexpr const char kOk[]                    = "200 OK";                       // Success / ACK
inline constexpr const char kBadRequest[]            = "400 Bad Request";              // Generic parse error
inline constexpr const char kTemporarilyUnavailable[]= "480 Temporarily Unavailable";  // Callee offline
inline constexpr const char kBusyHere[]              = "486 Busy Here";               // Callee busy

    inline bool IsStatusCode(const std::string& message) {
        std::regex pattern("^\\d+\\s");
        return std::regex_match(message, pattern);
    }

} // namespace StatusCodes

// -----------------------------------------------------------------------------
// Signaling command / prefix constants (non-status control messages)
// -----------------------------------------------------------------------------
namespace Msg {

// Greeting / discovery
inline constexpr const char kHello[]       = "HELLO";    // broadcast HELLO
inline constexpr const char kHelloPrefix[] = "HELLO:";   // targeted HELLO:<user>

// Session initiation (was "INIT")
inline constexpr const char kInvite[]       = "INVITE";    // plain INVITE without payload
inline constexpr const char kInvitePrefix[] = "INVITE:";   // INVITE:{json-payload}

// Waiting acknowledgment while caller prepares offer
inline constexpr const char kWaiting[]      = "WAITING";

// SDP / ICE negotiation prefixes
inline constexpr const char kOfferPrefix[]  = "OFFER:";
inline constexpr const char kAnswerPrefix[] = "ANSWER:";
inline constexpr const char kIcePrefix[]    = "ICE:";

// Call control / termination
inline constexpr const char kCancel[] = "CANCEL";
inline constexpr const char kBye[]    = "BYE";

inline constexpr const char kAddress[]       = "ADDRESS";    // broadcast ADDRESS
inline constexpr const char kAddressPrefix[] = "ADDRESS:";   // targeted ADDRESS:<user>:<ip>:<port>

inline constexpr const char kUsers[]       = "USERS";    // broadcast USERS
inline constexpr const char kUsersPrefix[] = "USERS:";   // targeted USERS:<user1>,<user2>

} // namespace Msg

#endif // EXAMPLES_DIRECT_STATUS_H_ 