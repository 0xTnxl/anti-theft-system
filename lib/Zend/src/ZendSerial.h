// Zend for boards with no network (Arduino Uno, Nano, Mega, Leonardo, Pro Mini…), through a
// bridge: an ESP32 or ESP8266 running examples/SerialBridge, or a Raspberry Pi running
// python/zend_bridge.py, joined to this board by a serial wire (or USB to a Pi).
//
// The bridge holds the device key, talks to Zend over TLS, checks every event's signature and
// only then passes it on, so this board never handles keys or crypto. Keep both boards in the
// same box: anyone who can reach the wire between them can send this board fake lines.
//
//   ZendSerial zend;
//   void onPaid(const char* ref, long kobo, const char* person, bool pass) { startWash(); }
//   void onQr(const char* ref, const char* url) { drawQr(url); }
//   void setup() { Serial.begin(9600); zend.begin(Serial); zend.onPaid(onPaid); zend.onQr(onQr); }
//   void loop() { zend.loop(); if (pressed()) zend.pay(500, "wash-3"); }
//
// Protocol (one line each way, fields split by single spaces, "-" for empty): see
// docs.usezend.app/devices#boards-without-wi-fi.
#pragma once
#include <Arduino.h>

#ifndef ZEND_SERIAL_LINE
#define ZEND_SERIAL_LINE 160  // longest line kept; longer ones are cut
#endif

class ZendSerial {
 public:
  typedef void (*QrFn)(const char* reference, const char* url);
  typedef void (*ErrorFn)(const char* what, const char* message);
  typedef void (*PaidFn)(const char* reference, long amountKobo, const char* person, bool hasPass);
  typedef void (*CheckinFn)(const char* reference, const char* person, bool hasPass, const char* name);
  typedef void (*CommandFn)(const char* commandId, const char* name, const char* argsJson);
  typedef void (*ConfigFn)(const char* key, const char* value);
  typedef void (*PassFn)(const char* person, bool valid);
  typedef void (*TextFn)(const char* text);
  typedef void (*VoidFn)();

  void begin(Stream& link);
  /// Call often: reads the bridge's lines and runs your callbacks.
  void loop();

  // What the board can ask for. Answers arrive through the callbacks.
  /// Payment request for whole Naira (≥ 50). passSecs > 0 sells a pass. Answer: onQr or onError.
  void pay(long amountNgn, const char* reference, long passSecs = 0, const char* note = nullptr);
  /// Check-in code. Answer: onQr, then onCheckin when someone scans it.
  void checkin(const char* reference, const char* note = nullptr);
  void ack(const char* commandId, bool ok, const char* message = nullptr);
  void alert(const char* message);
  void reading(const char* label, float value, const char* reference = nullptr, const char* unit = nullptr);
  /// Answer: onPass.
  void askPass(const char* person);
  /// Answer: onConfig for each setting, then onConfigDone.
  void requestConfig();
  /// Answer: onReady with the device's name.
  void hello();

  void onQr(QrFn f) { _qr = f; }
  void onError(ErrorFn f) { _err = f; }
  void onPaid(PaidFn f) { _paid = f; }
  void onCheckin(CheckinFn f) { _in = f; }
  void onCommand(CommandFn f) { _cmd = f; }
  void onConfig(ConfigFn f) { _cfg = f; }
  void onConfigDone(VoidFn f) { _cfgEnd = f; }
  void onPass(PassFn f) { _pass = f; }
  void onReady(TextFn f) { _ready = f; }
  void onOffline(VoidFn f) { _offline = f; }
  void onOk(TextFn f) { _ok = f; }
  void onTest(VoidFn f) { _test = f; }

 private:
  Stream* _s = nullptr;
  char _line[ZEND_SERIAL_LINE + 1];
  uint16_t _len = 0;
  bool _overflow = false;
  QrFn _qr = nullptr;
  ErrorFn _err = nullptr;
  PaidFn _paid = nullptr;
  CheckinFn _in = nullptr;
  CommandFn _cmd = nullptr;
  ConfigFn _cfg = nullptr;
  VoidFn _cfgEnd = nullptr;
  PassFn _pass = nullptr;
  TextFn _ready = nullptr;
  VoidFn _offline = nullptr;
  TextFn _ok = nullptr;
  VoidFn _test = nullptr;
  void _word(const char* w);  // one field: spaces become "_", empty becomes "-"
  void _handle(char* line);
};
