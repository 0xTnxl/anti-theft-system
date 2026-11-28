#include "ZendSerial.h"

#include <stdlib.h>
#include <string.h>

void ZendSerial::begin(Stream& link) { _s = &link; }

void ZendSerial::_word(const char* w) {
  if (!w || !*w) { _s->print('-'); return; }
  for (const char* p = w; *p; p++) _s->print((*p == ' ' || *p == '\n' || *p == '\r') ? '_' : *p);
}

static void text(Stream* s, const char* t) {
  for (const char* p = t; p && *p; p++) s->print((*p == '\n' || *p == '\r') ? ' ' : *p);
}

void ZendSerial::pay(long amountNgn, const char* reference, long passSecs, const char* note) {
  if (!_s) return;
  _s->print(F("PAY ")); _s->print(amountNgn); _s->print(' '); _word(reference); _s->print(' ');
  _s->print(passSecs > 0 ? passSecs : 0L);
  if (note && *note) { _s->print(' '); text(_s, note); }
  _s->print('\n');
}

void ZendSerial::checkin(const char* reference, const char* note) {
  if (!_s) return;
  _s->print(F("CHECKIN ")); _word(reference);
  if (note && *note) { _s->print(' '); text(_s, note); }
  _s->print('\n');
}

void ZendSerial::ack(const char* commandId, bool ok, const char* message) {
  if (!_s) return;
  _s->print(F("ACK ")); _word(commandId); _s->print(ok ? F(" 1") : F(" 0"));
  if (message && *message) { _s->print(' '); text(_s, message); }
  _s->print('\n');
}

void ZendSerial::alert(const char* message) {
  if (!_s) return;
  _s->print(F("ALERT ")); text(_s, message); _s->print('\n');
}

void ZendSerial::reading(const char* label, float value, const char* reference, const char* unit) {
  if (!_s) return;
  _s->print(F("READ ")); _word(label); _s->print(' '); _s->print(value, 4); _s->print(' '); _word(reference);
  if (unit && *unit) { _s->print(' '); _word(unit); }
  _s->print('\n');
}

void ZendSerial::askPass(const char* person) {
  if (!_s) return;
  _s->print(F("PASS ")); _word(person); _s->print('\n');
}

void ZendSerial::requestConfig() { if (_s) _s->print(F("CONFIG\n")); }
void ZendSerial::hello() { if (_s) _s->print(F("HELLO\n")); }

void ZendSerial::loop() {
  if (!_s) return;
  while (_s->available()) {
    char c = (char)_s->read();
    if (c == '\r') continue;
    if (c == '\n') {
      _line[_len] = 0;
      if (_len && !_overflow) _handle(_line);
      _len = 0;
      _overflow = false;
      continue;
    }
    if (_len < ZEND_SERIAL_LINE) _line[_len++] = c;
    else _overflow = true;  // dropped: a cut line could change its meaning
  }
}

// Splits off the next field. `rest` = everything after it (for free-text fields).
static char* field(char*& p) {
  if (!p || !*p) return (char*)"";
  char* start = p;
  char* sp = strchr(p, ' ');
  if (sp) { *sp = 0; p = sp + 1; } else { p = start + strlen(start); }
  return strcmp(start, "-") == 0 ? (char*)"" : start;
}

void ZendSerial::_handle(char* line) {
  char* p = line;
  char* kind = field(p);
  if (!strcmp(kind, "PAID")) {
    char* ref = field(p); long kobo = atol(field(p)); char* person = field(p); bool pass = !strcmp(field(p), "1");
    if (_paid) _paid(ref, kobo, person, pass);
  } else if (!strcmp(kind, "IN")) {
    char* ref = field(p); char* person = field(p); bool pass = !strcmp(field(p), "1");
    if (_in) _in(ref, person, pass, p);
  } else if (!strcmp(kind, "QR")) {
    char* ref = field(p); char* url = field(p);
    if (_qr) _qr(ref, url);
  } else if (!strcmp(kind, "ERR")) {
    char* what = field(p);
    if (_err) _err(what, p);
  } else if (!strcmp(kind, "CMD")) {
    char* id = field(p); char* name = field(p);
    if (_cmd) _cmd(id, name, *p ? p : "{}");
  } else if (!strcmp(kind, "CFG")) {
    char* key = field(p);
    if (_cfg) _cfg(key, p);
  } else if (!strcmp(kind, "CFGEND")) {
    if (_cfgEnd) _cfgEnd();
  } else if (!strcmp(kind, "PASS")) {
    char* person = field(p); bool valid = !strcmp(field(p), "1");
    if (_pass) _pass(person, valid);
  } else if (!strcmp(kind, "OK")) {
    if (_ok) _ok(field(p));
  } else if (!strcmp(kind, "READY")) {
    if (_ready) _ready(p);
  } else if (!strcmp(kind, "OFFLINE")) {
    if (_offline) _offline();
  } else if (!strcmp(kind, "TEST")) {
    if (_test) _test();
  }
}
