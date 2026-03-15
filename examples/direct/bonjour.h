#pragma once
#include <string>

// Advertise a TCP service on the local network with Bonjour (mDNS)
bool AdvertiseBonjourService(const std::string& name, int port);

// Discover a TCP service by name on the local network with Bonjour (mDNS)
// Returns true if found, and sets out_ip and out_port
bool DiscoverBonjourService(const std::string& name, std::string& out_ip, int& out_port, int timeout_seconds = 3); 