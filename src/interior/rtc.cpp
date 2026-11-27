#include "rtc.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>

namespace rtc {
namespace {

constexpr uint8_t ADDR = 0x68;
bool gPresent = false;

uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool readRegs(uint8_t reg, uint8_t* out, uint8_t n) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ADDR, (int)n) != n) return false;
    for (uint8_t i = 0; i < n; i++) out[i] = Wire.read();
    return true;
}

}  // namespace

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
uint32_t fromCivil(int y, int mo, int d, int h, int mi, int s) {
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (uint32_t)(days * 86400 + h * 3600 + mi * 60 + s);
}

void toCivil(uint32_t t, int* y, int* mo, int* d, int* h, int* mi, int* s) {
    int64_t z = t / 86400;
    const uint32_t rem = t % 86400;
    *h = rem / 3600;
    *mi = (rem / 60) % 60;
    *s = rem % 60;
    z += 719468;
    const int64_t era = z / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*mo <= 2));
}

bool parseIso(const char* text, uint32_t* unixS) {
    int y, mo, d, h, mi, s;
    char sep;
    if (sscanf(text, "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &s) != 7) return false;
    if ((sep != 'T' && sep != 't' && sep != ' ') || y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 ||
        d > 31 || h > 23 || mi > 59 || s > 59 || h < 0 || mi < 0 || s < 0)
        return false;
    *unixS = fromCivil(y, mo, d, h, mi, s);
    return true;
}

bool begin(uint8_t sda, uint8_t scl) {
    Wire.begin(sda, scl, 100000);
    Wire.setTimeOut(20);
    Wire.beginTransmission(ADDR);
    gPresent = Wire.endTransmission() == 0;
    return gPresent;
}

bool present() { return gPresent; }

bool read(uint32_t* unixS, bool* lostPower) {
    if (!gPresent) return false;
    uint8_t r[7], st;
    if (!readRegs(0x00, r, 7) || !readRegs(0x0F, &st, 1)) return false;
    const int s = bcd2bin(r[0] & 0x7F), mi = bcd2bin(r[1] & 0x7F);
    const int h = (r[2] & 0x40) ? bcd2bin(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0) : bcd2bin(r[2] & 0x3F);
    const int d = bcd2bin(r[4] & 0x3F), mo = bcd2bin(r[5] & 0x1F);
    const int y = 2000 + bcd2bin(r[6]) + ((r[5] & 0x80) ? 100 : 0);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return false;
    *unixS = fromCivil(y, mo, d, h, mi, s);
    *lostPower = (st & 0x80) != 0;
    return true;
}

bool set(uint32_t unixS) {
    if (!gPresent) return false;
    int y, mo, d, h, mi, s;
    toCivil(unixS, &y, &mo, &d, &h, &mi, &s);
    const int dow = (int)((unixS / 86400 + 4) % 7) + 1;   // 1970-01-01 was a Thursday
    Wire.beginTransmission(ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write(bin2bcd(s));
    Wire.write(bin2bcd(mi));
    Wire.write(bin2bcd(h));   // 24-hour mode
    Wire.write((uint8_t)dow);
    Wire.write(bin2bcd(d));
    Wire.write((uint8_t)(bin2bcd(mo) | (y >= 2100 ? 0x80 : 0)));
    Wire.write(bin2bcd((uint8_t)(y % 100)));
    if (Wire.endTransmission() != 0) return false;
    uint8_t st;
    if (!readRegs(0x0F, &st, 1)) return false;
    Wire.beginTransmission(ADDR);
    Wire.write((uint8_t)0x0F);
    Wire.write((uint8_t)(st & 0x7F));   // clear OSF
    return Wire.endTransmission() == 0;
}

}  // namespace rtc
