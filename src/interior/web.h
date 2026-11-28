// Dashboard served by the interior over its own WPA2 access point.
//
//   https://door.usezend.app  the app, with a Let's Encrypt certificate for the
//                             door's own key (devcert.h, tools/renew_cert.py).
//                             The door's DNS answers that name on its WiFi.
//   https://192.168.4.1   before a certificate is installed: local-CA certificate
//   http://192.168.4.1    redirect to the app (or the local-CA setup page)
//
// Two levels of access:
//   anyone on the WiFi:  the phone-PIN page (/api/public, /api/phonepin)
//   admin (password):    status, event log, cards, lock, alarms, time, updates
// There is deliberately no remote unlock: unlocking always needs the card at
// the door. See docs/DASHBOARD.md.
//
// The HTTPS server runs in its own task. Everything it does with the door
// logic happens under lock(), which loop() also holds while it runs.
#pragma once
#include <Arduino.h>

namespace web {

void loadCredentials();     // before WiFi starts: reads or generates the AP and admin passwords
const char* apSsid();
const char* apPass();
void begin();               // after WiFi is up: starts the HTTPS and setup servers
void service();             // call every loop (setup page on port 80)
void lock();
void unlock();
bool rebootRequested();     // a verified update is installed
void certInstalled();       // a new certificate was installed: restart HTTPS shortly
void printCredentials();    // console 'wifi'
bool setApPass(const char* p);    // takes effect after a reboot
bool setAdminPass(const char* p);
// Card nicknames shown on the dashboard ("Grandma"). Letters, digits, space ' - . only, max 20.
void cardName(uint8_t slot, char* out, size_t cap);
void forgetName(uint8_t slot);
void forgetAllNames();

}  // namespace web

// Implemented in src/interior/main.cpp: the door logic the dashboard may use.
// Called with web::lock() held.
namespace app {
void statusJson(String& out);
void publicJson(String& out);
void cardsJson(String& out);
bool lock(String& msg);
void summary(String& out);   // one plain sentence, for the owner's phone
bool clearAlarm(String& msg);
bool resetLockout(String& msg);
bool startEnroll(String& msg);
bool startSetPin(String& msg);
bool cancelAdmin(String& msg);
bool deleteCard(int slot, String& msg);
bool setPhonePin(const char* pin, String& msg);
bool setTime(uint32_t unixS, String& msg);
bool updateAllowed(String& msg);   // bolt idle: safe to install an update and restart
}  // namespace app
