#include "ZendBridge.h"
#if ZEND_HAS_NETWORK

// A field for the wire: no spaces or line breaks, "-" when empty.
static String word(const String& v) {
  if (!v.length()) return "-";
  String w = v;
  w.replace(' ', '_');
  w.replace('\n', '_');
  w.replace('\r', '_');
  return w;
}
static String flat(const String& v) {
  String t = v;
  t.replace('\n', ' ');
  t.replace('\r', ' ');
  return t;
}
// Splits off the next space-separated field; `rest` keeps the remainder.
static String take(String& rest) {
  rest.trim();
  int sp = rest.indexOf(' ');
  String f = sp < 0 ? rest : rest.substring(0, sp);
  rest = sp < 0 ? String() : rest.substring(sp + 1);
  return f == "-" ? String() : f;
}

void ZendBridge::begin(Stream& link) {
  _s = &link;
  _send("READY " ZEND_PLATFORM);
}

void ZendBridge::_send(const String& s) {
  if (!_s) return;
  _s->print(s);
  _s->print('\n');
}

void ZendBridge::_sendConfig(JsonVariantConst config) {
  for (JsonPairConst kv : config.as<JsonObjectConst>()) {
    String v;
    if (kv.value().is<const char*>()) v = kv.value().as<const char*>();
    else serializeJson(kv.value(), v);
    _send("CFG " + word(kv.key().c_str()) + " " + flat(v));
  }
  _send("CFGEND");
}

void ZendBridge::_handle(String rest) {
  String kind = take(rest);
  if (kind == "PAY") {
    long amount = take(rest).toInt();
    String ref = take(rest);
    long pass = take(rest).toInt();
    ZendRequest r = _zend.request((float)amount, ref, rest, 900, pass);
    _send(r.ok ? "QR " + word(ref) + " " + r.url : "ERR " + word(ref) + " " + flat(r.error));
  } else if (kind == "CHECKIN") {
    String ref = take(rest);
    ZendRequest r = _zend.checkin(ref, rest);
    _send(r.ok ? "QR " + word(ref) + " " + r.url : "ERR " + word(ref) + " " + flat(r.error));
  } else if (kind == "ACK") {
    String id = take(rest);
    bool ok = take(rest) == "1";
    _send(_zend.ack(id, ok, rest) ? "OK ACK" : "ERR ACK That didn't go through. Try again.");
  } else if (kind == "ALERT") {
    _send(_zend.alert(rest) ? "OK ALERT" : "ERR ALERT That didn't go through. Try again.");
  } else if (kind == "READ") {
    String label = take(rest);
    float value = take(rest).toFloat();
    String ref = take(rest);
    String unit = take(rest);
    _send(_zend.reading(label, value, unit, ref) ? "OK READ" : "ERR READ That didn't go through. Try again.");
  } else if (kind == "PASS") {
    String person = take(rest);
    _send("PASS " + word(person) + (_zend.hasPass(person) ? " 1" : " 0"));
  } else if (kind == "CONFIG") {
    JsonDocument c;
    if (_zend.config(c)) _sendConfig(c.as<JsonVariantConst>());
    else _send("ERR CONFIG That didn't go through. Try again.");
  } else if (kind == "HELLO") {
    _send("READY " ZEND_PLATFORM);
  } else if (kind.length()) {
    _send("ERR " + word(kind) + " Unknown request");
  }
}

void ZendBridge::loop(int pollSecs) {
  if (!_s) return;
  while (_s->available()) {
    char c = (char)_s->read();
    if (c == '\r') continue;
    if (c == '\n') {
      String l = _line;
      _line = "";
      if (l.length()) _handle(l);
      continue;
    }
    if (_line.length() < 240) _line += c;
  }
  if (_s->available()) return;  // more to answer first
  ZendEvent e;
  if (!_zend.next(e, pollSecs)) {
    if (_zend.lastStatus() < 0 && _zend.lastStatus() != -2) _send("OFFLINE");
    return;
  }
  bool pass = e.passValidUntil.length() > 0;
  if (e.type == "payment.received") {
    _send("PAID " + word(e.reference) + " " + String(e.amountKobo) + " " + word(e.person) + (pass ? " 1" : " 0"));
  } else if (e.type == "checkin") {
    _send("IN " + word(e.reference) + " " + word(e.person) + (pass ? " 1 " : " 0 ") + flat(e.name));
  } else if (e.type == "command") {
    String args;
    serializeJson(e.data["args"], args);
    _send("CMD " + word(e.commandId) + " " + word(e.data["name"] | "") + " " + args);
  } else if (e.type == "config.updated") {
    _sendConfig(e.data["config"]);
  } else if (e.type == "test") {
    _send("TEST");
  }
}

#endif
