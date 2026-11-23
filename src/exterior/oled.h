// Minimal SSD1306 128x32 I2C driver with a 5x7 font.
// commit() only touches the bus when the frame changed, because every I2C
// transfer also resets the RC522 (GPIO22 is OLED SCL and RC522 RST).
#pragma once
#include <stdint.h>

class Oled {
public:
    static constexpr int W = 128;
    static constexpr int H = 32;

    bool begin(uint8_t sda, uint8_t scl, uint8_t addr, uint32_t hz, bool flip);
    void clear();
    void text(int x, int y, const char* s, uint8_t scale = 1);
    void textCentered(int y, const char* s, uint8_t scale = 1);
    static int textWidth(const char* s, uint8_t scale = 1);

    bool commit();            // returns true if an I2C transfer happened
    void power(bool on);
    bool isOn() const { return on_; }
    bool present() const { return present_; }
    uint32_t lastBusMs() const { return lastBusMs_; }
    // Column of the first visible pixel: 0 for SSD1306, 2 for SH1106 (OLED_COL_OFFSET).
    void setColumnOffset(uint8_t c) { colOffset_ = c; sentValid_ = false; }
    void forceRedraw() { sentValid_ = false; }
    uint32_t writeErrors() const { return writeErrors_; }   // I2C writes not acknowledged

private:
    void cmds(const uint8_t* c, uint8_t n);
    void pixel(int x, int y);

    uint8_t addr_ = 0x3C;
    bool present_ = false;
    bool on_ = false;
    bool sentValid_ = false;
    uint32_t lastBusMs_ = 0;
    uint8_t colOffset_ = 0;
    uint32_t writeErrors_ = 0;
    uint8_t buf_[W * H / 8] = {};
    uint8_t sent_[W * H / 8] = {};
};
