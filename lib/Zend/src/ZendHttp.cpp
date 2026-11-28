#include "ZendHttp.h"
#if ZEND_HAS_NETWORK

namespace zend {

// Reads one line (without "\r\n") into `line`. False on timeout or a dropped connection.
static bool readLine(Client& c, String& line, unsigned long deadline, size_t max = 512) {
  line = "";
  while (millis() < deadline) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') {
        if (line.length() && line[line.length() - 1] == '\r') line.remove(line.length() - 1);
        return true;
      }
      if (line.length() < max) line += ch;
    }
    if (!c.connected()) return false;
    delay(1);
  }
  return false;
}

// Reads exactly n bytes into `out`, or fewer if the connection ends. False on timeout.
static bool readBytes(Client& c, String& out, size_t n, unsigned long deadline) {
  while (n && millis() < deadline) {
    int avail = c.available();
    if (avail <= 0) {
      if (!c.connected()) return false;
      delay(1);
      continue;
    }
    while (avail-- > 0 && n) {
      out += (char)c.read();
      n--;
    }
  }
  return n == 0;
}

int httpRequest(Client& c, const char* host, const char* method, const String& path, const String& bearer,
                const String& body, String& out, unsigned long timeoutMs, size_t maxBody) {
  out = "";
  unsigned long deadline = millis() + timeoutMs;
  if (!c.connect(host, 443)) return -1;
  String req;
  req.reserve(240 + path.length());
  req += method; req += ' '; req += path; req += " HTTP/1.1\r\nHost: "; req += host;
  req += "\r\nAuthorization: Bearer "; req += bearer;
  req += "\r\nUser-Agent: zend-arduino/0.2 (" ZEND_PLATFORM ")\r\nAccept: application/json\r\nConnection: close\r\n";
  if (body.length() || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
    req += "Content-Type: application/json\r\nContent-Length: "; req += String(body.length()); req += "\r\n";
  }
  req += "\r\n";
  c.print(req);
  if (body.length()) c.print(body);

  String line;
  if (!readLine(c, line, deadline)) { c.stop(); return -2; }
  // "HTTP/1.1 200 OK"
  int sp = line.indexOf(' ');
  int status = sp > 0 ? line.substring(sp + 1, sp + 4).toInt() : 0;
  if (status < 100) { c.stop(); return -4; }
  long contentLength = -1;
  bool chunked = false;
  for (;;) {
    if (!readLine(c, line, deadline)) { c.stop(); return -2; }
    if (!line.length()) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = lower.substring(15).toInt();
    else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") > 0) chunked = true;
  }
  if (chunked) {
    for (;;) {
      if (!readLine(c, line, deadline)) { c.stop(); return -2; }
      long n = strtol(line.c_str(), nullptr, 16);
      if (n <= 0) break;
      if (out.length() + (size_t)n > maxBody) { c.stop(); return -3; }
      if (!readBytes(c, out, (size_t)n, deadline)) { c.stop(); return -2; }
      readLine(c, line, deadline);  // the "\r\n" after each chunk
    }
  } else if (contentLength >= 0) {
    if ((size_t)contentLength > maxBody) { c.stop(); return -3; }
    out.reserve(contentLength);
    if (!readBytes(c, out, (size_t)contentLength, deadline)) { c.stop(); return -2; }
  } else {
    // No length: read until the server closes.
    while (millis() < deadline && (c.connected() || c.available())) {
      while (c.available()) {
        if (out.length() >= maxBody) { c.stop(); return -3; }
        out += (char)c.read();
      }
      delay(1);
    }
  }
  c.stop();
  return status;
}

}  // namespace zend
#endif
