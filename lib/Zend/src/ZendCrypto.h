// SHA-256 and HMAC-SHA256 in plain C++, so every board checks Zend's signatures the same way
// (ESP32, ESP8266, UNO R4 WiFi, MKR / Nano 33 IoT, and AVR boards if they ever need it).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace zend {

struct Sha256 {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t len;
  size_t used;
  Sha256();
  void update(const uint8_t* data, size_t n);
  void update(const char* s);
  void finish(uint8_t out[32]);
};

/// HMAC-SHA256(key, msg) as 64 lowercase hex characters plus a NUL.
void hmacSha256Hex(const char* key, size_t keyLen, const char* msg, size_t msgLen, char out[65]);
/// SHA-256(msg) as 64 lowercase hex characters plus a NUL.
void sha256Hex(const char* msg, size_t len, char out[65]);
/// Compares two strings in time that doesn't depend on where they differ.
bool sameInConstantTime(const char* a, size_t aLen, const char* b, size_t bLen);

}  // namespace zend
