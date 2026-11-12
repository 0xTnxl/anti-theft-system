#include "oled.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "font5x7.h"

bool Oled::begin(uint8_t sda, uint8_t scl, uint8_t addr, uint32_t hz, bool flip) {
    addr_ = addr;
    Wire.begin(sda, scl, hz);
    Wire.beginTransmission(addr_);
    present_ = Wire.endTransmission() == 0;
    lastBusMs_ = millis();
    if (!present_) return false;

    const uint8_t init[] = {
        0xAE,                  // display off
        0xD5, 0x80,            // clock divide
        0xA8, 0x1F,            // multiplex 32
        0xD3, 0x00,            // display offset
        0x40,                  // start line 0
        0x8D, 0x14,            // charge pump on
        0x20, 0x00,            // horizontal addressing
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
    Wire.endTransmission();
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
    const uint8_t window[] = {0x21, 0, W - 1, 0x22, 0, H / 8 - 1};
    cmds(window, sizeof(window));
    for (size_t i = 0; i < sizeof(buf_); i += 32) {
        Wire.beginTransmission(addr_);
        Wire.write((uint8_t)0x40);
        Wire.write(buf_ + i, 32);
        Wire.endTransmission();
    }
    lastBusMs_ = millis();
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
