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

constexpr uint8_t RECORD_VERSION = 1;

struct __attribute__((packed)) Record {
    uint8_t version;   // 0 = empty slot
    uint8_t uidLen;
    uint8_t uid[proto::UID_MAX];
    uint32_t iterations;
    uint8_t salt[16];
    uint8_t hash[32];
};

Preferences gPrefs;
Record gRecs[MAX_CREDENTIALS];
Record gDummy;   // used for unknown cards
uint32_t gFails = 0;

void key(uint8_t slot, char out[8]) { snprintf(out, 8, "r%02u", slot); }

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

bool pbkdf2(const char* pin, uint8_t pinLen, const uint8_t salt[16], uint32_t iter,
            uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char*)pin, pinLen, salt, 16, iter,
                                        32, out) == 0;
    mbedtls_md_free(&ctx);
    return ok;
}

bool save(uint8_t slot) {
    char k[8];
    key(slot, k);
    if (gRecs[slot].version == 0) return gPrefs.remove(k) || !gPrefs.isKey(k);
    return gPrefs.putBytes(k, &gRecs[slot], sizeof(Record)) == sizeof(Record);
}

}  // namespace

bool begin() {
    if (!gPrefs.begin("atds_creds", false)) return false;
    for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
        char k[8];
        key(i, k);
        memset(&gRecs[i], 0, sizeof(Record));
        if (gPrefs.getBytesLength(k) == sizeof(Record)) gPrefs.getBytes(k, &gRecs[i], sizeof(Record));
        if (gRecs[i].version != RECORD_VERSION) memset(&gRecs[i], 0, sizeof(Record));
    }
    gFails = gPrefs.getUInt("fails", 0);
    memset(&gDummy, 0, sizeof(gDummy));
    esp_fill_random(gDummy.salt, sizeof(gDummy.salt));
    esp_fill_random(gDummy.hash, sizeof(gDummy.hash));
    gDummy.iterations = PBKDF2_ITERATIONS;
    return true;
}

uint8_t count() {
    uint8_t n = 0;
    for (const Record& r : gRecs) n += r.version != 0;
    return n;
}

bool slotUsed(uint8_t slot) { return slot < MAX_CREDENTIALS && gRecs[slot].version != 0; }

bool slotUid(uint8_t slot, uint8_t* uid, uint8_t& len) {
    if (!slotUsed(slot)) return false;
    len = gRecs[slot].uidLen;
    memcpy(uid, gRecs[slot].uid, len);
    return true;
}

static int findUid(const uint8_t* uid, uint8_t uidLen) {
    uint8_t padded[proto::UID_MAX] = {};
    memcpy(padded, uid, uidLen);
    int match = -1;
    for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
        const bool eq = gRecs[i].version != 0 && gRecs[i].uidLen == uidLen &&
                        ctEqual(gRecs[i].uid, padded, proto::UID_MAX);
        if (eq && match < 0) match = i;
    }
    return match;
}

Verify verify(const uint8_t* uid, uint8_t uidLen, const char* pin, uint8_t pinLen) {
    if (uidLen == 0 || uidLen > proto::UID_MAX) return Verify::UnknownCard;
    const int slot = findUid(uid, uidLen);
    const Record& r = slot >= 0 ? gRecs[slot] : gDummy;
    uint8_t h[32];
    const bool hashed = pbkdf2(pin, pinLen, r.salt, r.iterations, h);
    const bool pinOk = hashed && ctEqual(h, r.hash, sizeof(h));
    mbedtls_platform_zeroize(h, sizeof(h));
    if (slot < 0) return Verify::UnknownCard;
    return pinOk ? Verify::Ok : Verify::BadPin;
}

int enroll(const uint8_t* uid, uint8_t uidLen, const char* pin, uint8_t pinLen, uint32_t* hashMs) {
    if (uidLen == 0 || uidLen > proto::UID_MAX) return -1;
    int slot = findUid(uid, uidLen);
    if (slot < 0) {
        for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
            if (gRecs[i].version == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return -1;

    Record r = {};
    r.version = RECORD_VERSION;
    r.uidLen = uidLen;
    memcpy(r.uid, uid, uidLen);
    r.iterations = PBKDF2_ITERATIONS;
    esp_fill_random(r.salt, sizeof(r.salt));
    const uint32_t t0 = millis();
    const bool ok = pbkdf2(pin, pinLen, r.salt, r.iterations, r.hash);
    if (hashMs) *hashMs = millis() - t0;
    if (!ok) {
        mbedtls_platform_zeroize(&r, sizeof(r));
        return -1;
    }
    const Record old = gRecs[slot];
    gRecs[slot] = r;
    mbedtls_platform_zeroize(&r, sizeof(r));
    if (!save((uint8_t)slot)) {
        gRecs[slot] = old;
        return -1;
    }
    return slot;
}

bool remove(uint8_t slot) {
    if (!slotUsed(slot)) return false;
    mbedtls_platform_zeroize(&gRecs[slot], sizeof(Record));
    return save(slot);
}

void wipeAll() {
    for (Record& r : gRecs) mbedtls_platform_zeroize(&r, sizeof(r));
    const uint32_t fails = gFails;
    gPrefs.clear();
    if (fails) gPrefs.putUInt("fails", fails);   // wiping cards must not reset the lockout
}

uint32_t failCount() { return gFails; }

void setFailCount(uint32_t n) {
    if (n == gFails) return;
    gFails = n;
    gPrefs.putUInt("fails", n);
}

}  // namespace creds
