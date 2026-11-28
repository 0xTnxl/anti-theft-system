// The interior's connection to the home WiFi, used only to reach Zend
// (api.usezend.app) for alerts, remote lock and log anchoring. Optional: with
// no home WiFi set, the door works exactly as before.
//
// The ESP32 has one radio. Joining the router moves the radio, and with it the
// ATDS-Door access point and the ESP-NOW link, to the router's channel. The
// exterior finds the new channel by itself (config_exterior.h, HUNT_*).
// Reconnects use the router's last channel so the radio does not sweep all
// channels (which would interrupt the exterior link) more than every 10 minutes.
#pragma once
#include <Arduino.h>

namespace homenet {

void begin();                 // after espnow::begin(): joins the home WiFi if one is set
void service(uint32_t now);   // every loop
bool connected();
bool timeValid();             // SNTP time received since boot
uint32_t unixNow();           // 0 until timeValid()
bool set(const char* ssid, const char* pass);   // saves and reconnects; empty ssid = off
void printStatus();           // console 'homewifi'
void statusJson(String& out); // fragment: "home":{...}

}  // namespace homenet
