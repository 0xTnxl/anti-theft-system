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

// Sets this board's base MAC to myMac (must run before anything starts WiFi),
// starts WiFi STA on `channel`, registers peerMac as the only peer.
// maxTxQdBm: max TX power in 0.25 dBm units, 0 keeps the SDK default.
bool begin(const uint8_t myMac[6], const uint8_t peerMac[6], uint8_t channel, int8_t maxTxQdBm);
bool send(const uint8_t* data, size_t len);
bool receive(Packet& out);   // non-blocking

uint32_t foreignFrames();   // frames from other MACs (dropped)
uint32_t queueOverflows();

}  // namespace espnow
