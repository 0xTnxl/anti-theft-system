#include "homenet.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "evlog.h"

namespace homenet {
namespace {

constexpr uint32_t RETRY_MS = 60000;            // reconnect attempt with the known channel
constexpr uint32_t FULL_SCAN_MS = 10UL * 60000; // at most this often a scan of every channel
constexpr uint32_t MIN_VALID_UNIX = 1767225600; // 2026-01-01

Preferences gPrefs;
char gSsid[33] = "", gPass[65] = "";
uint8_t gChannel = 0;   // router channel last seen
bool gWas = false;
uint32_t gLastTry = 0, gLastFull = 0;
bool gTried = false;

void attempt(uint32_t now) {
    const bool full = !gChannel || !gTried || now - gLastFull >= FULL_SCAN_MS;
    if (full) gLastFull = now;
    gTried = true;
    gLastTry = now;
    WiFi.disconnect(false, false);
    WiFi.begin(gSsid, gPass, full ? 0 : gChannel);
    Serial.printf("[home] joining '%s'%s\n", gSsid, full ? " (scanning all channels)" : "");
}

}  // namespace

void begin() {
    gPrefs.begin("atds_home", false);
    gPrefs.getString("ssid", gSsid, sizeof(gSsid));
    gPrefs.getString("pass", gPass, sizeof(gPass));
    gChannel = gPrefs.getUChar("ch", 0);
    WiFi.setAutoReconnect(false);   // reconnects are paced here instead
    sntp_set_time_sync_notification_cb(nullptr);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    if (gSsid[0]) attempt(millis());
    else Serial.println("[home] no home WiFi set: Zend features are off ('homewifi' to set one)");
}

void service(uint32_t now) {
    if (!gSsid[0]) return;
    const bool c = WiFi.status() == WL_CONNECTED;
    if (c && !gWas) {
        const uint8_t ch = WiFi.channel();
        Serial.printf("[home] connected to '%s', channel %u, IP %s\n", gSsid, ch, WiFi.localIP().toString().c_str());
        if (ch != gChannel) {
            gChannel = ch;
            gPrefs.putUChar("ch", ch);
        }
        evlog::append(evlog::EV_HOME_WIFI, evlog::NO_SLOT, 1, ch);
    } else if (!c && gWas) {
        Serial.println("[home] home WiFi lost");
        evlog::append(evlog::EV_HOME_WIFI, evlog::NO_SLOT, 0, gChannel);
        gLastTry = now;   // wait a full retry period: the router may be rebooting
    }
    gWas = c;
    if (!c && now - gLastTry >= RETRY_MS) attempt(now);
}

bool connected() { return gSsid[0] && WiFi.status() == WL_CONNECTED; }
bool timeValid() { return time(nullptr) >= (time_t)MIN_VALID_UNIX; }
uint32_t unixNow() { return timeValid() ? (uint32_t)time(nullptr) : 0; }

bool set(const char* ssid, const char* pass) {
    if (strlen(ssid) > 32 || strlen(pass) > 64 || (ssid[0] && pass[0] && strlen(pass) < 8)) return false;
    strlcpy(gSsid, ssid, sizeof(gSsid));
    strlcpy(gPass, pass, sizeof(gPass));
    gChannel = 0;
    gPrefs.putString("ssid", gSsid);
    gPrefs.putString("pass", gPass);
    gPrefs.putUChar("ch", 0);
    gWas = false;
    gTried = false;
    WiFi.disconnect(false, false);
    if (gSsid[0]) attempt(millis());
    return true;
}

void printStatus() {
    if (!gSsid[0]) {
        Serial.println("home WiFi: off (homewifi set <ssid> | <password>)");
        return;
    }
    Serial.printf("home WiFi '%s': %s, router channel %u, radio channel %u, time %s\n", gSsid,
                  connected() ? "connected" : "not connected", gChannel, WiFi.channel(),
                  timeValid() ? "synced" : "not synced");
    if (connected()) Serial.printf("IP %s, signal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void statusJson(String& out) {
    String s = gSsid;
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    out += String("\"home\":{\"set\":") + (gSsid[0] ? "true" : "false") + ",\"ssid\":\"" + s +
           "\",\"up\":" + (connected() ? "true" : "false") + ",\"ch\":" + WiFi.channel() + "}";
}

}  // namespace homenet
