// Credential store (NVS): one shared PIN for every card, plus the list of
// enrolled card UIDs. The PIN is stored as PBKDF2-HMAC-SHA256(pin, salt).
// Also keeps the persistent failed-attempt counter for the lockout.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace creds {

enum class Verify : uint8_t { Ok, BadPin, UnknownCard, NoPin };

bool begin();   // loads everything into RAM; drops the old per-card PIN records
uint8_t count();
bool slotUsed(uint8_t slot);
bool slotUid(uint8_t slot, uint8_t* uid, uint8_t& len);
int findCard(const uint8_t* uid, uint8_t uidLen);   // slot or -1
bool pinSet();

// Card + PIN check. Always runs exactly one PBKDF2 (also for unknown cards and
// when no PIN is set), so timing does not reveal which cards are enrolled.
// *slot is set to the card's slot, or -1.
Verify verify(const uint8_t* uid, uint8_t uidLen, const char* pin, uint8_t pinLen, int* slot);
bool checkPin(const char* pin, uint8_t pinLen);   // shared PIN only (one PBKDF2)

int addCard(const uint8_t* uid, uint8_t uidLen);  // slot (existing slot if already enrolled), -1 if full
bool setPin(const char* pin, uint8_t pinLen, uint32_t* hashMs = nullptr);
bool remove(uint8_t slot);
void wipeAll();   // all cards and the PIN; the failure counter is kept

uint32_t failCount();
void setFailCount(uint32_t n);

}  // namespace creds
