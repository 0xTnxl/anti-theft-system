#include "credentials.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <string.h>

#include "config_interior.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "protocol.h"

namespace creds {
namespace {

constexpr uint8_t PIN_VERSION = 2;

struct __attribute__((packed)) PinRecord {
    uint8_t version;   // 0 = no PIN set
    uint32_t iterations;
    uint8_t salt[16];
    uint8_t hash[32];
};

struct __attribute__((packed)) Card {
    uint8_t uidLen;    // 0 = empty slot
    uint8_t uid[proto::UID_MAX];
};

Preferences gPrefs;
PinRecord gPin;
PinRecord gDummy;   // hashed against when no PIN is set, to keep timing constant
Card gCards[MAX_CREDENTIALS];
uint32_t gFails = 0;

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

bool pbkdf2(const char* pin, uint8_t pinLen, const uint8_t salt[16], uint32_t iter, uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char*)pin, pinLen, salt, 16, iter, 32,
                                        out) == 0;
    mbedtls_md_free(&ctx);
    return ok;
}

bool saveCards() { return gPrefs.putBytes("cards", gCards, sizeof(gCards)) == sizeof(gCards); }

// One PBKDF2 against the shared PIN (or the dummy record when none is set).
bool pinMatches(const char* pin, uint8_t pinLen) {
    const PinRecord& r = gPin.version ? gPin : gDummy;
    uint8_t h[32];
    const bool ok = pbkdf2(pin, pinLen, r.salt, r.iterations, h) && ctEqual(h, r.hash, sizeof(h));
    mbedtls_platform_zeroize(h, sizeof(h));
    return ok && gPin.version;
}

}  // namespace

bool begin() {
    // The per-card PIN records of the old design cannot be converted: drop them.
    {
        Preferences old;
        if (old.begin("atds_creds", false)) {
            if (old.isKey("r00") || old.isKey("r01") || old.isKey("fails")) {
                const uint32_t f = old.getUInt("fails", 0);
                old.clear();
                if (f) gFails = f;
                Serial.println("[creds] old per-card PIN records removed: set the shared PIN and enroll cards again");
            }
            old.end();
        }
    }
    if (!gPrefs.begin("atds_v2", false)) return false;
    memset(&gPin, 0, sizeof(gPin));
    if (gPrefs.getBytesLength("pin") == sizeof(gPin)) gPrefs.getBytes("pin", &gPin, sizeof(gPin));
    if (gPin.version != PIN_VERSION) memset(&gPin, 0, sizeof(gPin));
    memset(gCards, 0, sizeof(gCards));
    if (gPrefs.getBytesLength("cards") == sizeof(gCards)) gPrefs.getBytes("cards", gCards, sizeof(gCards));
    for (Card& c : gCards)
        if (c.uidLen > proto::UID_MAX) memset(&c, 0, sizeof(c));
    if (gPrefs.isKey("fails")) gFails = gPrefs.getUInt("fails", 0);
    else if (gFails) gPrefs.putUInt("fails", gFails);

    memset(&gDummy, 0, sizeof(gDummy));
    esp_fill_random(gDummy.salt, sizeof(gDummy.salt));
    esp_fill_random(gDummy.hash, sizeof(gDummy.hash));
    gDummy.iterations = PBKDF2_ITERATIONS;
    return true;
}

uint8_t count() {
    uint8_t n = 0;
    for (const Card& c : gCards) n += c.uidLen != 0;
    return n;
}

bool slotUsed(uint8_t slot) { return slot < MAX_CREDENTIALS && gCards[slot].uidLen != 0; }

bool slotUid(uint8_t slot, uint8_t* uid, uint8_t& len) {
    if (!slotUsed(slot)) return false;
    len = gCards[slot].uidLen;
    memcpy(uid, gCards[slot].uid, len);
    return true;
}

bool pinSet() { return gPin.version != 0; }

int findCard(const uint8_t* uid, uint8_t uidLen) {
    if (uidLen == 0 || uidLen > proto::UID_MAX) return -1;
    uint8_t padded[proto::UID_MAX] = {};
    memcpy(padded, uid, uidLen);
    int match = -1;
    for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {   // no early exit: constant work
        const bool eq = gCards[i].uidLen == uidLen && ctEqual(gCards[i].uid, padded, proto::UID_MAX);
        if (eq && match < 0) match = i;
    }
    return match;
}

Verify verify(const uint8_t* uid, uint8_t uidLen, const char* pin, uint8_t pinLen, int* slot) {
    const int s = findCard(uid, uidLen);
    if (slot) *slot = s;
    const bool pinOk = pinMatches(pin, pinLen);
    if (!gPin.version) return Verify::NoPin;
    if (s < 0) return Verify::UnknownCard;
    return pinOk ? Verify::Ok : Verify::BadPin;
}

bool checkPin(const char* pin, uint8_t pinLen) { return pinMatches(pin, pinLen); }

int addCard(const uint8_t* uid, uint8_t uidLen) {
    if (uidLen == 0 || uidLen > proto::UID_MAX) return -1;
    const int existing = findCard(uid, uidLen);
    if (existing >= 0) return existing;
    for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
        if (gCards[i].uidLen) continue;
        gCards[i].uidLen = uidLen;
        memset(gCards[i].uid, 0, sizeof(gCards[i].uid));
        memcpy(gCards[i].uid, uid, uidLen);
        if (saveCards()) return i;
        memset(&gCards[i], 0, sizeof(Card));
        return -1;
    }
    return -1;
}

bool setPin(const char* pin, uint8_t pinLen, uint32_t* hashMs) {
    PinRecord r = {};
    r.version = PIN_VERSION;
    r.iterations = PBKDF2_ITERATIONS;
    esp_fill_random(r.salt, sizeof(r.salt));
    const uint32_t t0 = millis();
    bool ok = pbkdf2(pin, pinLen, r.salt, r.iterations, r.hash);
    if (hashMs) *hashMs = millis() - t0;
    ok = ok && gPrefs.putBytes("pin", &r, sizeof(r)) == sizeof(r);
    if (ok) gPin = r;
    mbedtls_platform_zeroize(&r, sizeof(r));
    return ok;
}

bool remove(uint8_t slot) {
    if (!slotUsed(slot)) return false;
    memset(&gCards[slot], 0, sizeof(Card));
    return saveCards();
}

void wipeAll() {
    memset(gCards, 0, sizeof(gCards));
    mbedtls_platform_zeroize(&gPin, sizeof(gPin));
    gPrefs.remove("cards");
    gPrefs.remove("pin");
}

uint32_t failCount() { return gFails; }

void setFailCount(uint32_t n) {
    if (n == gFails) return;
    gFails = n;
    gPrefs.putUInt("fails", n);
}

}  // namespace creds
