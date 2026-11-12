// 4x4 matrix keypad scanner. begin() claims the pins, end() releases them all
// to plain inputs so GPIO23 (C4) can go back to the SPI bus as MOSI.
#pragma once
#include <stdint.h>

class Keypad {
public:
    void begin();
    void end();
    bool active() const { return active_; }
    // Call often. Returns a key once per press (debounced), or 0.
    char poll(uint32_t nowMs);

private:
    char scan();

    bool active_ = false;
    uint32_t lastScanMs_ = 0;
    char candidate_ = 0;
    uint8_t stableCount_ = 0;
    char reported_ = 0;
};
