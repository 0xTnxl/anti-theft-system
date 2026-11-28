// Minimal ESP-NOW transport: one fixed peer, frames from any other MAC dropped.
// Received frames are copied from the WiFi task into a queue; call receive()
// from loop().
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace espnow {

struct Packet {
    uint8_t len;
    uint8_t data[250];
};

// Starts WiFi STA on `channel`, sets the STA MAC to myMac, registers peerMac
// as the only peer.
// maxTxQdBm: max TX power in 0.25 dBm units, 0 keeps the SDK default.
// apSsid / apPass (8+ chars): also run a WPA2 access point on the same channel.
bool begin(const uint8_t myMac[6], const uint8_t peerMac[6], uint8_t channel, int8_t maxTxQdBm,
           const char* apSsid = nullptr, const char* apPass = nullptr);
bool send(const uint8_t* data, size_t len);
// Exterior only (not joined to any network): move the radio to another channel.
bool setChannel(uint8_t channel);
uint8_t channel();   // the radio's current channel
bool receive(Packet& out);   // non-blocking

uint32_t foreignFrames();   // frames from other MACs (dropped)
uint32_t queueOverflows();

}  // namespace espnow
