#include "oled.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "font5x7.h"

bool Oled::begin(uint8_t sda, uint8_t scl, uint8_t addr, uint32_t hz, bool flip) {
    addr_ = addr;
    Wire.begin(sda, scl, hz);
    // Default is 50 ms per transfer. With a bad SCL contact every failed transfer
    // waited that long, freezing the keypad. A good transfer takes about 3 ms.
    Wire.setTimeOut(10);
    // The module may still be powering up right after boot: probe a few times.
    present_ = false;
    for (int i = 0; i < 5 && !present_; i++) {
        if (i) delay(100);
        Wire.beginTransmission(addr_);
        present_ = Wire.endTransmission() == 0;
    }
    lastBusMs_ = millis();
    if (!present_) return false;

    const uint8_t init[] = {
        0xAE,                  // display off
        0xD5, 0x80,            // clock divide
        0xA8, 0x1F,            // multiplex 32
        0xD3, 0x00,            // display offset
        0x40,                  // start line 0
        0x8D, 0x14,            // charge pump on
        0x20, 0x02,            // page addressing (SH1106 only has this mode)
        (uint8_t)(flip ? 0xA0 : 0xA1),   // segment remap
        (uint8_t)(flip ? 0xC0 : 0xC8),   // COM scan direction
        0xDA, 0x02,            // COM pins for 128x32
        0x81, 0x8F,            // contrast
        0xD9, 0xF1,            // pre-charge
        0xDB, 0x40,            // VCOMH
        0xA4,                  // show RAM
        0xA6,                  // normal (not inverted)
        0x2E,                  // no scroll
        0xAF,                  // display on
    };
    cmds(init, sizeof(init));
    on_ = true;
    sentValid_ = false;
    clear();
    commit();
    return true;
}

void Oled::cmds(const uint8_t* c, uint8_t n) {
    Wire.beginTransmission(addr_);
    Wire.write((uint8_t)0x00);
    Wire.write(c, n);
    if (Wire.endTransmission() != 0) writeErrors_++;
    lastBusMs_ = millis();
}

void Oled::clear() { memset(buf_, 0, sizeof(buf_)); }

void Oled::pixel(int x, int y) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    buf_[x + (y / 8) * W] |= (uint8_t)(1 << (y & 7));
}

int Oled::textWidth(const char* s, uint8_t scale) {
    int n = (int)strlen(s);
    return n == 0 ? 0 : (n * 6 - 1) * scale;
}

void Oled::text(int x, int y, const char* s, uint8_t scale) {
    if (scale == 0) scale = 1;
    for (; *s; s++, x += 6 * scale) {
        char c = *s;
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint8_t* g = FONT5X7[c - FONT_FIRST];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (!(g[col] >> row & 1)) continue;
                for (int dx = 0; dx < scale; dx++)
                    for (int dy = 0; dy < scale; dy++)
                        pixel(x + col * scale + dx, y + row * scale + dy);
            }
        }
    }
}

void Oled::textCentered(int y, const char* s, uint8_t scale) {
    text((W - textWidth(s, scale)) / 2, y, s, scale);
}

bool Oled::commit() {
    if (!present_ || !on_) return false;
    if (sentValid_ && memcmp(buf_, sent_, sizeof(buf_)) == 0) return false;
    const uint32_t t0 = millis();
    // Page by page with explicit column address: works on SSD1306 and SH1106
    // (SH1106 has 132 columns, the visible 128 start at column 2).
    // Only pages that changed are sent.
    for (uint8_t page = 0; page < H / 8; page++) {
        if (sentValid_ && memcmp(buf_ + page * W, sent_ + page * W, W) == 0) continue;
        if (idle_) idle_();
        const uint8_t pos[] = {(uint8_t)(0xB0 | page), (uint8_t)(0x00 | (colOffset_ & 0x0F)),
                               (uint8_t)(0x10 | (colOffset_ >> 4))};
        cmds(pos, sizeof(pos));
        for (int x = 0; x < W; x += 32) {
            Wire.beginTransmission(addr_);
            Wire.write((uint8_t)0x40);
            Wire.write(buf_ + page * W + x, 32);
            if (Wire.endTransmission() != 0) writeErrors_++;
            if (idle_) idle_();
        }
    }
    lastBusMs_ = millis();
    lastCommitMs_ = lastBusMs_ - t0;
    memcpy(sent_, buf_, sizeof(buf_));
    sentValid_ = true;
    return true;
}

void Oled::power(bool on) {
    if (!present_ || on == on_) return;
    const uint8_t c = on ? 0xAF : 0xAE;
    cmds(&c, 1);
    on_ = on;
    if (on) sentValid_ = false;   // force a full redraw
}
