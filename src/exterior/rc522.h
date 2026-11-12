// Minimal MFRC522 driver: read the UID of one ISO 14443A card (4, 7 or 10 bytes).
// The caller owns the SPI bus (SPI.begin / SPI.end), because MOSI (GPIO23) is
// shared with the keypad. RST is not driven by this driver.
#pragma once
#include <stdint.h>

class Rc522 {
public:
    void begin(uint8_t csPin, uint32_t spiHz);
    bool init();            // soft reset + configure + antenna on (SPI must be running)
    bool isConfigured();    // false after a hard reset (e.g. an OLED I2C transfer)
    uint8_t version();
    bool readUid(uint8_t* uid, uint8_t& len);   // uid must hold 10 bytes
    void antennaOff();

private:
    enum Status : uint8_t { OK, TIMEOUT, ERROR, COLLISION, NO_ROOM };

    uint8_t read(uint8_t reg);
    void write(uint8_t reg, uint8_t v);
    void setBits(uint8_t reg, uint8_t mask);
    void clearBits(uint8_t reg, uint8_t mask);
    Status transceive(const uint8_t* tx, uint8_t txLen, uint8_t* rx, uint8_t* rxLen,
                      uint8_t txLastBits, uint8_t* rxLastBits = nullptr);
    void halt();

    uint8_t cs_ = 5;
    uint32_t hz_ = 4000000;
};

// ISO/IEC 14443-3 CRC_A, result low byte first.
void crcA(const uint8_t* data, uint8_t len, uint8_t out[2]);
