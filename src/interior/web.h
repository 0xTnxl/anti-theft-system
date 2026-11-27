// Dashboard served by the interior over its own WPA2 access point.
//
// Two levels of access:
//   anyone on the WiFi:  the phone-PIN page (/api/public, /api/phonepin)
//   admin (password):    status, event log, cards, lock, alarms, time
// There is deliberately no remote unlock: unlocking always needs the card at
// the door. See docs/DASHBOARD.md.
#pragma once
#include <Arduino.h>

namespace web {

void loadCredentials();     // before WiFi starts: reads or generates the AP and admin passwords
const char* apSsid();
const char* apPass();
void begin();               // after WiFi is up: starts the HTTP server
void service(bool busy);    // call every loop; skipped while the motor is driving
void printCredentials();    // console 'wifi'
bool setApPass(const char* p);    // takes effect after a reboot
bool setAdminPass(const char* p);
// Card nicknames shown on the dashboard ("Grandma"). Letters, digits, space ' - . only, max 20.
void cardName(uint8_t slot, char* out, size_t cap);
void forgetName(uint8_t slot);
void forgetAllNames();

}  // namespace web

// Implemented in src/interior/main.cpp: the door logic the dashboard may use.
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
}  // namespace app
