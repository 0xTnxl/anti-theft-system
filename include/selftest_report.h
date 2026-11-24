// Shared helpers for the component kick tests (env:exterior_selftest, env:interior_selftest).
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <stdarg.h>
#include <string.h>

namespace st {

// PASS / FAIL: decided by the firmware.
// WARN: works, but something looks wrong.
// CHECK: a person has to look, listen or act to confirm.
// SKIP: could not run (a dependency failed).
enum Res : uint8_t { PASS, FAIL, WARN, CHECK, SKIP };

inline const char* resName(Res r) {
    static const char* n[] = {"PASS", "FAIL", "WARN", "CHECK", "SKIP"};
    return n[r];
}

struct Item {
    char test[40];
    Res res;
    char note[160];
};
inline Item items[24];
inline uint8_t itemCount = 0;

// Re-running a test replaces its earlier result.
inline void record(const char* test, Res res, const char* fmt, ...) {
    char note[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(note, sizeof(note), fmt, ap);
    va_end(ap);
    Item* it = nullptr;
    for (uint8_t i = 0; i < itemCount; i++)
        if (!strcmp(items[i].test, test)) it = &items[i];
    if (!it && itemCount < sizeof(items) / sizeof(items[0])) it = &items[itemCount++];
    if (it) {
        snprintf(it->test, sizeof(it->test), "%s", test);
        snprintf(it->note, sizeof(it->note), "%s", note);
        it->res = res;
    }
    Serial.printf("[%s] %s: %s\n", resName(res), test, note);
}

inline void summary() {
    uint8_t n[5] = {};
    Serial.println("\n================ KICK TEST SUMMARY ================");
    for (uint8_t i = 0; i < itemCount; i++) {
        Serial.printf("%-5s  %-30s %s\n", resName(items[i].res), items[i].test, items[i].note);
        n[items[i].res]++;
    }
    Serial.printf("---- %u pass, %u fail, %u warn, %u to check by hand, %u skipped\n", n[PASS],
                  n[FAIL], n[WARN], n[CHECK], n[SKIP]);
    Serial.println("CHECK = a person must confirm (look / listen) or run the interactive test.");
    Serial.println("===================================================\n");
}

// Any input on the serial port stops an interactive test.
inline bool abortRequested() {
    if (!Serial.available()) return false;
    while (Serial.available()) {
        Serial.read();
        delay(2);
    }
    Serial.println("  (stopped)");
    return true;
}

// Reads one command line. Keeps printable ASCII only (drops stray bytes from
// the USB adapter and terminal control codes), trims spaces, lower-cases.
inline bool readLine(char* buf, size_t cap) {
    static size_t len = 0;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            while (len && buf[len - 1] == ' ') len--;
            if (!len) continue;
            buf[len] = 0;
            len = 0;
            return true;
        }
        if (c == 0x08 || c == 0x7F) {   // backspace
            if (len) len--;
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (c == ' ' && len == 0) continue;
        if (len < cap - 1) buf[len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    return false;
}

// For unrecognised commands: shows exactly what arrived.
inline void unknownCommand(const char* line) {
    Serial.printf("unknown command '%s' (%u chars, hex:", line, (unsigned)strlen(line));
    for (const char* p = line; *p; p++) Serial.printf(" %02X", (uint8_t)*p);
    Serial.println(")");
}

// Radio wake-up: start WiFi, scan once, turn it off again.
inline void testRadio() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    const int n = WiFi.scanNetworks(false, true);
    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    if (n < 0) {
        record("radio (WiFi / ESP-NOW)", FAIL, "WiFi scan failed (code %d)", n);
    } else {
        record("radio (WiFi / ESP-NOW)", PASS,
               "radio started, factory MAC %02x:%02x:%02x:%02x:%02x:%02x, %d networks heard",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], n);
    }
}

}  // namespace st
