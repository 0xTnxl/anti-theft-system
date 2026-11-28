// Zend for Wi-Fi boards: ESP32, ESP8266, Arduino UNO R4 WiFi, MKR WiFi 1010, Nano 33 IoT and
// Nano RP2040 Connect. Boards without Wi-Fi (Uno, Nano, Mega…) use ZendSerial.h and a bridge.
//
//   Zend zend(ZEND_DEVICE_KEY, ZEND_DEVICE_SECRET);   // after WiFi is connected
//   ZendRequest r = zend.request(500, "wash-3");        // ₦500
//   showQr(r.url);
//   ZendEvent e;
//   if (zend.next(e)) {
//     if (e.type == "payment.received" && e.reference == "wash-3") startMachine();
//     if (e.type == "command") { run(e.name); zend.ack(e.commandId, true); }
//     if (e.type == "config.updated") applySettings(e.data["config"]);
//   }
//
// Every event is checked (HMAC-SHA256 over `signed`, and SHA-256 of `data_raw`) before next()
// returns it, and its fields come from the signed data. TLS checks Zend's certificate.
#pragma once
#include "ZendPlatform.h"

#if ZEND_HAS_NETWORK
#include <Arduino.h>
#include <ArduinoJson.h>

struct ZendRequest {
  bool ok = false;
  String id;          // zl_…
  String url;         // show this as a QR
  String amountLabel; // "₦500"
  String error;       // set when ok is false
};

struct ZendEvent {
  long seq = 0;
  String type;        // payment.received, checkin, command, config.updated, test
  String requestId;
  long amountKobo = 0;
  String reference;
  String method;      // payments: "zend" or "bank_transfer"
  String person;      // payments and check-ins: a code for the person (zp_…)
  String name;        // check-ins: the person's name, if they chose to share it
  String passValidUntil; // when they hold a pass (ISO time), else empty
  String commandId;   // commands
  long signedAt = 0;  // [ATDS patch] Unix time Zend signed the event (second-to-last signed field), 0 if absent
  JsonDocument data;  // the whole signed data (commands: data["args"]; config: data["config"])
};
using ZendPayment = ZendEvent;  // older sketches

class Zend {
 public:
  Zend(const char* deviceKey, const char* signingSecret, const char* baseUrl = "https://api.usezend.app");

  /// One-time payment request in the owner's name. amountNgn ≥ 50, up to the device's limit.
  /// passSecs > 0: paying buys a pass of that length (see hasPass).
  ZendRequest request(float amountNgn, const String& reference = "", const String& note = "",
                      long expiresInSecs = 900, long passSecs = 0);
  /// A check-in code to show as a QR. The person then arrives as a `checkin` event.
  ZendRequest checkin(const String& reference = "", const String& note = "");
  /// Waits up to waitSecs (0–30) for the next verified event. False on timeout or error.
  bool next(ZendEvent& out, int waitSecs = 25);
  bool paid(ZendEvent& out, int waitSecs = 25) { return next(out, waitSecs); }
  /// Settings from the owner (the group's, with the device's own on top).
  bool config(JsonDocument& out);
  /// Whether `person` holds a pass for this device (or its group).
  bool hasPass(const String& person);
  bool ack(const String& commandId, bool ok, const String& message = "");
  /// A notification to the owner and the group's staff ("Soap is low"). Up to 10 an hour.
  bool alert(const String& message);
  /// A reading Zend signs into its hash chain (label: a-z, 0-9, _).
  bool reading(const String& label, float value, const String& unit = "", const String& reference = "");
  /// "waiting", "paid", "expired", "cancelled", or "" on error.
  String status(const String& requestId);
  bool cancel(const String& requestId);
  /// Resume point: store it in flash so a restart doesn't miss or repeat events.
  long cursor() const { return _after; }
  void setCursor(long after) { _after = after; }
  /// The last HTTP status or library error (negative), for debugging.
  int lastStatus() const { return _lastStatus; }
  /// PEM roots to trust instead of the built-in ones (ESP32, ESP8266, UNO R4 WiFi).
  void setRootCA(const char* pem) { _rootCA = pem; }

  /// HMAC-SHA256(secret, line) as hex, for checking the published test vector.
  static String hmacHex(const String& secret, const String& line);
  static String sha256Hex(const String& s);
  /// True when `sig` and the data hash in `signedLine` both check out.
  static bool verifyEvent(const String& secret, const String& signedLine, const String& sig, const String& dataRaw);

 private:
  String _key, _secret, _host;
  const char* _rootCA;
  long _after = -1;  // -1: start from new events only
  int _lastStatus = 0;
  int _call(const char* method, const String& path, const String& body, String& out, unsigned long timeoutMs);
  bool _post(const String& path, JsonDocument& body);
};

#endif  // ZEND_HAS_NETWORK
