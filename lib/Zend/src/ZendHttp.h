// A small HTTP/1.1 client over any Arduino `Client` (a TLS client in practice). Used on every
// networked board, so request and response handling is the same everywhere.
#pragma once
#include "ZendPlatform.h"
#if ZEND_HAS_NETWORK
#include <Arduino.h>
#include <Client.h>

namespace zend {

/// Returns the HTTP status, or a negative number: -1 can't connect, -2 timed out, -3 the answer
/// was bigger than maxBody, -4 the answer couldn't be read.
int httpRequest(Client& c, const char* host, const char* method, const String& path, const String& bearer,
                const String& body, String& out, unsigned long timeoutMs, size_t maxBody);

}  // namespace zend
#endif
