// Card + PIN credential store (NVS) and persistent lockout counter.
// PINs are stored as PBKDF2-HMAC-SHA256(pin, per-record salt).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace creds {

enum class Verify : uint8_t { Ok, BadPin, UnknownCard };

bool begin();   // loads records into RAM
uint8_t count();
bool slotUsed(uint8_t slot);
bool slotUid(uint8_t slot, uint8_t* uid, uint8_t& len);

// Always runs exactly one PBKDF2, also for unknown cards, so timing does not
// reveal which cards are enrolled.
Verify verify(const uint8_t* uid, uint8_t uidLen, const char* pin, uint8_t pinLen);

// Adds a record, or replaces the PIN if the UID is already enrolled.
// Returns the slot, or -1 if storage is full or the write failed.
int enroll(const uint8_t* uid, uint8_t uidLen, const char* pin, uint8_t pinLen,
           uint32_t* hashMs = nullptr);
bool remove(uint8_t slot);
void wipeAll();

// Lockout counter, kept in NVS so a reboot does not reset it.
uint32_t failCount();
void setFailCount(uint32_t n);

}  // namespace creds
