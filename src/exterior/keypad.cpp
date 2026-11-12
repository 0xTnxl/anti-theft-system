#include "keypad.h"

#include <Arduino.h>

#include "config_exterior.h"

void Keypad::begin() {
    // Rows idle as high-impedance inputs; only the row being scanned is driven LOW.
    for (uint8_t r : PIN_KP_ROWS) pinMode(r, INPUT);
    for (uint8_t c : PIN_KP_COLS) pinMode(c, INPUT_PULLUP);
    candidate_ = 0;
    stableCount_ = 0;
    // Treat a key already held when the PIN phase starts as reported, so it
    // only counts after it is released and pressed again.
    reported_ = scan();
    active_ = true;
}

void Keypad::end() {
    for (uint8_t r : PIN_KP_ROWS) pinMode(r, INPUT);
    for (uint8_t c : PIN_KP_COLS) pinMode(c, INPUT);
    active_ = false;
}

char Keypad::scan() {
    char found = 0;
    for (uint8_t r = 0; r < 4; r++) {
        pinMode(PIN_KP_ROWS[r], OUTPUT);
        digitalWrite(PIN_KP_ROWS[r], LOW);
        delayMicroseconds(10);
        for (uint8_t c = 0; c < 4; c++) {
            if (digitalRead(PIN_KP_COLS[c]) == LOW && !found) found = KEYMAP[r][c];
        }
        pinMode(PIN_KP_ROWS[r], INPUT);
    }
    return found;
}

char Keypad::poll(uint32_t nowMs) {
    if (!active_ || nowMs - lastScanMs_ < KEYPAD_SCAN_MS) return 0;
    lastScanMs_ = nowMs;
    const char k = scan();
    if (k != candidate_) {
        candidate_ = k;
        stableCount_ = 1;
        return 0;
    }
    if (stableCount_ < KEYPAD_DEBOUNCE_SCANS) {
        stableCount_++;
        return 0;
    }
    if (k == reported_) return 0;
    reported_ = k;
    return k;   // 0 when the key was released
}
