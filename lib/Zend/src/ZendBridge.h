// The bridge side of ZendSerial: runs on an ESP32 or ESP8266 wired to a board with no network.
// It talks to Zend, checks every event, and passes verified ones down the wire as short lines.
//
//   Zend zend(KEY, SECRET);
//   ZendBridge bridge(zend);
//   void setup() { Serial2.begin(9600, SERIAL_8N1, 16, 17); bridge.begin(Serial2); }
//   void loop() { bridge.loop(); }
#pragma once
#include "Zend.h"
#if ZEND_HAS_NETWORK

class ZendBridge {
 public:
  explicit ZendBridge(Zend& zend) : _zend(zend) {}
  void begin(Stream& link);
  /// Handles the board's lines, then waits up to pollSecs for one Zend event and passes it on.
  void loop(int pollSecs = 3);

 private:
  Zend& _zend;
  Stream* _s = nullptr;
  String _line;
  void _handle(String line);
  void _send(const String& s);
  void _sendConfig(JsonVariantConst config);
};

#endif
