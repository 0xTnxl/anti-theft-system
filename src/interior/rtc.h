// Minimal DS3231 real-time clock driver (I2C address 0x68), UTC Unix seconds.
#pragma once
#include <stdint.h>

namespace rtc {

bool begin(uint8_t sda, uint8_t scl);   // true if a DS3231 answers
bool present();
// Reads the time. *lostPower is set when the oscillator-stop flag is on
// (battery removed or never set): the time is then not trustworthy.
bool read(uint32_t* unixS, bool* lostPower);
bool set(uint32_t unixS);              // also clears the oscillator-stop flag

// Helpers (UTC).
uint32_t fromCivil(int y, int mo, int d, int h, int mi, int s);
void toCivil(uint32_t unixS, int* y, int* mo, int* d, int* h, int* mi, int* s);
bool parseIso(const char* text, uint32_t* unixS);   // "2026-11-21T15:04:05" or with a space

}  // namespace rtc
