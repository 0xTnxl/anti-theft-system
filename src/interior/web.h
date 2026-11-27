// Dashboard served by the interior over its own WPA2 access point.
//
//   https://192.168.4.1   the app (TLS, certificate from tools/gen_device_keys.py)
//   http://192.168.4.1    setup page only: download ca.crt, then go to https
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
