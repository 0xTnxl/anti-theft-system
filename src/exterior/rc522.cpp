#include "rc522.h"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

namespace {
// Registers
constexpr uint8_t CommandReg = 0x01;
constexpr uint8_t ComIrqReg = 0x04;
constexpr uint8_t ErrorReg = 0x06;
constexpr uint8_t FIFODataReg = 0x09;
constexpr uint8_t FIFOLevelReg = 0x0A;
constexpr uint8_t ControlReg = 0x0C;
constexpr uint8_t BitFramingReg = 0x0D;
constexpr uint8_t CollReg = 0x0E;
constexpr uint8_t ModeReg = 0x11;
constexpr uint8_t TxModeReg = 0x12;
constexpr uint8_t RxModeReg = 0x13;
constexpr uint8_t TxControlReg = 0x14;
constexpr uint8_t TxASKReg = 0x15;
constexpr uint8_t ModWidthReg = 0x24;
constexpr uint8_t TModeReg = 0x2A;
constexpr uint8_t TPrescalerReg = 0x2B;
constexpr uint8_t TReloadRegH = 0x2C;
constexpr uint8_t TReloadRegL = 0x2D;
constexpr uint8_t VersionReg = 0x37;
// Commands
constexpr uint8_t CMD_IDLE = 0x00;
constexpr uint8_t CMD_TRANSCEIVE = 0x0C;
constexpr uint8_t CMD_SOFT_RESET = 0x0F;
// PICC commands
constexpr uint8_t PICC_REQA = 0x26;
constexpr uint8_t PICC_HLTA = 0x50;
constexpr uint8_t PICC_CASCADE_TAG = 0x88;
constexpr uint8_t SEL[3] = {0x93, 0x95, 0x97};

constexpr uint8_t TMODE_CONFIGURED = 0x80;   // TAuto; reset value is 0x00
}  // namespace

void crcA(const uint8_t* data, uint8_t len, uint8_t out[2]) {
    uint16_t crc = 0x6363;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t ch = data[i] ^ (uint8_t)(crc & 0xFF);
        ch ^= (uint8_t)(ch << 4);
        crc = (crc >> 8) ^ ((uint16_t)ch << 8) ^ ((uint16_t)ch << 3) ^ ((uint16_t)ch >> 4);
    }
    out[0] = (uint8_t)(crc & 0xFF);
    out[1] = (uint8_t)(crc >> 8);
}

void Rc522::begin(uint8_t csPin, uint32_t spiHz) {
    cs_ = csPin;
    hz_ = spiHz;
    pinMode(cs_, OUTPUT);
    digitalWrite(cs_, HIGH);
}

uint8_t Rc522::read(uint8_t reg) {
    SPI.beginTransaction(SPISettings(hz_, MSBFIRST, SPI_MODE0));
    digitalWrite(cs_, LOW);
    SPI.transfer((uint8_t)(0x80 | ((reg << 1) & 0x7E)));
    uint8_t v = SPI.transfer(0);
    digitalWrite(cs_, HIGH);
    SPI.endTransaction();
    return v;
}

void Rc522::write(uint8_t reg, uint8_t v) {
    SPI.beginTransaction(SPISettings(hz_, MSBFIRST, SPI_MODE0));
    digitalWrite(cs_, LOW);
    SPI.transfer((uint8_t)((reg << 1) & 0x7E));
    SPI.transfer(v);
    digitalWrite(cs_, HIGH);
    SPI.endTransaction();
}

void Rc522::setBits(uint8_t reg, uint8_t mask) { write(reg, read(reg) | mask); }
void Rc522::clearBits(uint8_t reg, uint8_t mask) { write(reg, read(reg) & ~mask); }

uint8_t Rc522::version() { return read(VersionReg); }

bool Rc522::isConfigured() { return read(TModeReg) == TMODE_CONFIGURED; }

bool Rc522::init() {
    write(CommandReg, CMD_SOFT_RESET);
    uint32_t t0 = millis();
    delay(1);
    while (read(CommandReg) & 0x10) {   // PowerDown bit clears when ready
        if (millis() - t0 > 50) return false;
        delay(1);
    }
    write(TxModeReg, 0x00);
    write(RxModeReg, 0x00);
    write(ModWidthReg, 0x26);
    // Timer: TAuto, about 25 ms receive timeout
    write(TModeReg, TMODE_CONFIGURED);
    write(TPrescalerReg, 0xA9);
    write(TReloadRegH, 0x03);
    write(TReloadRegL, 0xE8);
    write(TxASKReg, 0x40);   // 100 % ASK
    write(ModeReg, 0x3D);    // CRC preset 0x6363
    uint8_t tx = read(TxControlReg);
    if ((tx & 0x03) != 0x03) write(TxControlReg, tx | 0x03);   // antenna on
    const uint8_t v = version();
    return isConfigured() && v != 0x00 && v != 0xFF;
}

void Rc522::antennaOff() { clearBits(TxControlReg, 0x03); }

Rc522::Status Rc522::transceive(const uint8_t* tx, uint8_t txLen, uint8_t* rx, uint8_t* rxLen,
                                uint8_t txLastBits, uint8_t* rxLastBits) {
    write(CommandReg, CMD_IDLE);
    write(ComIrqReg, 0x7F);      // clear interrupt flags
    write(FIFOLevelReg, 0x80);   // flush FIFO
    for (uint8_t i = 0; i < txLen; i++) write(FIFODataReg, tx[i]);
    write(BitFramingReg, txLastBits & 0x07);
    write(CommandReg, CMD_TRANSCEIVE);
    setBits(BitFramingReg, 0x80);   // StartSend

    const uint32_t t0 = millis();
    for (;;) {
        const uint8_t irq = read(ComIrqReg);
        if (irq & 0x30) break;            // RxIRq or IdleIRq
        if (irq & 0x01) return TIMEOUT;   // TimerIRq
        if (millis() - t0 > 40) return TIMEOUT;
    }
    const uint8_t err = read(ErrorReg);
    if (err & 0x13) return ERROR;   // BufferOvfl, ParityErr, ProtocolErr

    if (rx && rxLen) {
        const uint8_t n = read(FIFOLevelReg);
        if (n > *rxLen) return NO_ROOM;
        *rxLen = n;
        for (uint8_t i = 0; i < n; i++) rx[i] = read(FIFODataReg);
        if (rxLastBits) *rxLastBits = read(ControlReg) & 0x07;
    }
    if (err & 0x08) return COLLISION;
    return OK;
}

void Rc522::halt() {
    uint8_t f[4] = {PICC_HLTA, 0x00, 0, 0};
    crcA(f, 2, f + 2);
    uint8_t dummy[4];
    uint8_t n = sizeof(dummy);
    transceive(f, 4, dummy, &n, 0);   // a card that halted does not answer
}

bool Rc522::readUid(uint8_t* uid, uint8_t& len) {
    len = 0;
    clearBits(CollReg, 0x80);
    uint8_t req = PICC_REQA;
    uint8_t atqa[2];
    uint8_t n = sizeof(atqa);
    uint8_t lastBits = 0;
    if (transceive(&req, 1, atqa, &n, 7, &lastBits) != OK || n != 2 || lastBits != 0) return false;

    uint8_t out[10];
    uint8_t outLen = 0;
    for (uint8_t level = 0; level < 3; level++) {
        // Anticollision: no collision resolution, a single card is expected.
        uint8_t ac[2] = {SEL[level], 0x20};
        uint8_t part[5];
        n = sizeof(part);
        if (transceive(ac, 2, part, &n, 0) != OK || n != 5) return false;
        if ((part[0] ^ part[1] ^ part[2] ^ part[3]) != part[4]) return false;

        uint8_t sel[9] = {SEL[level], 0x70, part[0], part[1], part[2], part[3], part[4], 0, 0};
        crcA(sel, 7, sel + 7);
        uint8_t sak[3];
        n = sizeof(sak);
        if (transceive(sel, 9, sak, &n, 0) != OK || n != 3) return false;
        uint8_t c[2];
        crcA(sak, 1, c);
        if (c[0] != sak[1] || c[1] != sak[2]) return false;

        const bool more = sak[0] & 0x04;
        if (more) {
            if (part[0] != PICC_CASCADE_TAG || level == 2) return false;
            memcpy(out + outLen, part + 1, 3);
            outLen += 3;
        } else {
            memcpy(out + outLen, part, 4);
            outLen += 4;
            break;
        }
    }
    if (outLen != 4 && outLen != 7 && outLen != 10) return false;
    halt();
    memcpy(uid, out, outLen);
    len = outLen;
    return true;
}
