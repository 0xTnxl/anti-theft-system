#include "Zend.h"
#if ZEND_HAS_NETWORK

#include "ZendCrypto.h"
#include "ZendHttp.h"
#include "ZendRoots.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFiClientSecure.h>
#elif defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>
#elif defined(ARDUINO_UNOR4_WIFI)
#include <WiFiS3.h>
#else
#include <WiFiNINA.h>
#endif

Zend::Zend(const char* deviceKey, const char* signingSecret, const char* baseUrl)
    : _key(deviceKey), _secret(signingSecret), _rootCA(ZEND_ROOT_CA) {
  String b(baseUrl);
  if (b.startsWith("https://")) b = b.substring(8);
  int slash = b.indexOf('/');
  _host = slash >= 0 ? b.substring(0, slash) : b;
}

String Zend::hmacHex(const String& secret, const String& line) {
  char out[65];
  zend::hmacSha256Hex(secret.c_str(), secret.length(), line.c_str(), line.length(), out);
  return String(out);
}

String Zend::sha256Hex(const String& s) {
  char out[65];
  zend::sha256Hex(s.c_str(), s.length(), out);
  return String(out);
}

bool Zend::verifyEvent(const String& secret, const String& signedLine, const String& sig, const String& dataRaw) {
  int bar = signedLine.lastIndexOf('|');
  if (bar < 0 || !dataRaw.length()) return false;
  String mac = hmacHex(secret, signedLine);
  if (!zend::sameInConstantTime(mac.c_str(), mac.length(), sig.c_str(), sig.length())) return false;
  String want = sha256Hex(dataRaw);
  const char* got = signedLine.c_str() + bar + 1;
  return zend::sameInConstantTime(want.c_str(), want.length(), got, strlen(got));
}

#if defined(ARDUINO_ARCH_ESP8266)
// BearSSL checks certificate dates, so the board needs the time once.
static void ensureClock() {
  static bool set = false;
  if (set) return;
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  unsigned long until = millis() + 15000;
  while (time(nullptr) < 1700000000 && millis() < until) delay(200);
  set = time(nullptr) >= 1700000000;
}
#endif

int Zend::_call(const char* method, const String& path, const String& body, String& out, unsigned long timeoutMs) {
#if defined(ARDUINO_ARCH_ESP32)
  WiFiClientSecure tls;
  tls.setCACert(_rootCA);
#elif defined(ARDUINO_ARCH_ESP8266)
  ensureClock();
  static BearSSL::X509List* anchors = nullptr;
  static const char* anchorsFor = nullptr;
  if (anchorsFor != _rootCA) {  // parse the roots once
    delete anchors;
    anchors = new BearSSL::X509List(_rootCA);
    anchorsFor = _rootCA;
  }
  BearSSL::WiFiClientSecure tls;
  tls.setTrustAnchors(anchors);
  tls.setBufferSizes(16384, 512);  // full-size records in, small requests out
#elif defined(ARDUINO_UNOR4_WIFI)
  WiFiSSLClient tls;
  if (_rootCA != ZEND_ROOT_CA) tls.setCACert(_rootCA);  // otherwise the WiFi chip's own roots
#else
  WiFiSSLClient tls;  // roots in the NINA firmware; keep it up to date
#endif
  _lastStatus = zend::httpRequest(tls, _host.c_str(), method, path, _key, body, out, timeoutMs, ZEND_MAX_BODY);
  return _lastStatus;
}

bool Zend::_post(const String& path, JsonDocument& body) {
  String b, out;
  serializeJson(body, b);
  return _call("POST", path, b, out, 20000) == 200;
}

static ZendRequest toRequest(int code, const String& out) {
  ZendRequest r;
  JsonDocument a;
  if (deserializeJson(a, out)) {
    r.error = code > 0 ? "Bad answer from Zend" : "No connection";
    return r;
  }
  if (code != 200) {
    r.error = a["message"] | "That didn't go through. Try again.";
    return r;
  }
  r.ok = true;
  r.id = a["id"].as<String>();
  r.url = a["url"].as<String>();
  r.amountLabel = a["amount_label"] | "";
  return r;
}

ZendRequest Zend::request(float amountNgn, const String& reference, const String& note, long expiresInSecs, long passSecs) {
  JsonDocument q;
  q["amount_ngn"] = amountNgn;
  if (reference.length()) q["reference"] = reference;
  if (note.length()) q["note"] = note;
  q["expires_in_secs"] = expiresInSecs;
  if (passSecs > 0) q["pass_secs"] = passSecs;
  String body, out;
  serializeJson(q, body);
  int code = _call("POST", "/api/v1/device/requests", body, out, 20000);
  return toRequest(code, out);
}

ZendRequest Zend::checkin(const String& reference, const String& note) {
  JsonDocument q;
  if (reference.length()) q["reference"] = reference;
  if (note.length()) q["note"] = note;
  String body, out;
  serializeJson(q, body);
  int code = _call("POST", "/api/v1/device/checkins", body, out, 20000);
  return toRequest(code, out);
}

bool Zend::next(ZendEvent& e, int waitSecs) {
  if (waitSecs < 0) waitSecs = 0;
  if (waitSecs > 30) waitSecs = 30;
  String path = "/api/v1/device/events?limit=" + String(ZEND_EVENT_BATCH) + "&wait=" + String(waitSecs);
  if (_after >= 0) path += "&after=" + String(_after);
  String out;
  if (_call("GET", path, "", out, (unsigned long)(waitSecs + 15) * 1000) != 200) return false;
  JsonDocument a;
  if (deserializeJson(a, out)) return false;
  long next = a["next"] | _after;
  for (JsonObject ev : a["events"].as<JsonArray>()) {
    long seq = ev["seq"] | 0L;
    _after = seq;  // one event per call; the rest come on the next call at once
    String raw = ev["data_raw"] | "";
    if (!verifyEvent(_secret, ev["signed"] | "", ev["sig"] | "", raw)) continue;  // never act on it
    e.data.clear();
    if (deserializeJson(e.data, raw)) continue;
    e.seq = seq;
    e.type = ev["type"].as<String>();
    e.requestId = e.data["request_id"] | "";
    e.amountKobo = e.data["amount_kobo"] | 0L;
    e.reference = e.data["reference"] | "";
    e.method = e.data["method"] | "";
    e.person = e.data["person"] | "";
    e.name = e.data["name"] | "";
    e.passValidUntil = e.data["pass_valid_until"] | "";
    e.commandId = e.data["command_id"] | "";
    {  // [ATDS patch] the signed time, so callers can refuse stale commands
      const String sl = ev["signed"] | "";
      const int last = sl.lastIndexOf('|');
      const int prev = last > 0 ? sl.lastIndexOf('|', last - 1) : -1;
      e.signedAt = prev >= 0 ? sl.substring(prev + 1, last).toInt() : 0;
    }
    return true;
  }
  _after = next;
  return false;
}

bool Zend::config(JsonDocument& outDoc) {
  String out;
  if (_call("GET", "/api/v1/device/config", "", out, 15000) != 200) return false;
  JsonDocument a;
  if (deserializeJson(a, out)) return false;
  outDoc.set(a["config"]);
  return true;
}

bool Zend::hasPass(const String& person) {
  String out;
  if (_call("GET", "/api/v1/device/passes/" + person, "", out, 15000) != 200) return false;
  JsonDocument a;
  return !deserializeJson(a, out) && (a["valid"] | false);
}

bool Zend::ack(const String& commandId, bool ok, const String& message) {
  JsonDocument q;
  q["ok"] = ok;
  if (message.length()) q["message"] = message;
  return _post("/api/v1/device/commands/" + commandId + "/ack", q);
}

bool Zend::alert(const String& message) {
  JsonDocument q;
  q["message"] = message;
  return _post("/api/v1/device/alerts", q);
}

bool Zend::reading(const String& label, float value, const String& unit, const String& reference) {
  JsonDocument q;
  q["label"] = label;
  q["value"] = value;
  if (unit.length()) q["unit"] = unit;
  if (reference.length()) q["reference"] = reference;
  return _post("/api/v1/device/readings", q);
}

String Zend::status(const String& requestId) {
  String out;
  if (_call("GET", "/api/v1/device/requests/" + requestId, "", out, 15000) != 200) return "";
  JsonDocument a;
  if (deserializeJson(a, out)) return "";
  return a["status"].as<String>();
}

bool Zend::cancel(const String& requestId) {
  String out;
  return _call("POST", "/api/v1/device/requests/" + requestId + "/cancel", "{}", out, 15000) == 200;
}

#endif  // ZEND_HAS_NETWORK
