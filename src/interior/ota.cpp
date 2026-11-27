#include "ota.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "tls_guard.h"
#include "version.h"

namespace ota {

struct Session {
    esp_ota_handle_t handle;
    const esp_partition_t* part;
    mbedtls_md_context_t md;
    uint8_t want[32];
    uint32_t len, got, version;
};

namespace {

Session gSession;
bool gBusy = false;
constexpr char TARGET[] = "interior";

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool sigOk(const uint8_t* signedPart, const uint8_t* sig, size_t sigLen) {
    uint8_t hash[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), signedPart, 64, hash) != 0) return false;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)OTA_PUBKEY_PEM, sizeof(OTA_PUBKEY_PEM)) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

}  // namespace

Session* begin(const uint8_t h[HEADER_LEN], size_t bodyLen, const char** err, uint32_t* version) {
    *err = nullptr;
    if (gBusy) { *err = "another update is running"; return nullptr; }
    if (memcmp(h, "ATDSOTA1", 8) != 0) { *err = "not a door update file"; return nullptr; }
    const uint32_t v = le32(h + 8), len = le32(h + 12);
    *version = v;
    char target[17] = {};
    memcpy(target, h + 48, 16);
    if (strcmp(target, TARGET) != 0) { *err = "this update is for a different board"; return nullptr; }
    const size_t sigLen = h[64];
    if (sigLen < 8 || sigLen > 72 || !sigOk(h, h + 68, sigLen)) { *err = "signature check failed: not an official update"; return nullptr; }
    if (v < ATDS_FW_VERSION) { *err = "older than the installed version"; return nullptr; }
    if (len != bodyLen) { *err = "file length does not match"; return nullptr; }
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part || len > part->size) { *err = "image too large"; return nullptr; }

    Session* s = &gSession;
    memset(s, 0, sizeof(*s));
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s->handle) != ESP_OK) { *err = "could not open the update slot"; return nullptr; }
    mbedtls_md_init(&s->md);
    mbedtls_md_setup(&s->md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&s->md);
    memcpy(s->want, h + 16, 32);
    s->part = part;
    s->len = len;
    s->version = v;
    gBusy = true;
    return s;
}

bool write(Session* s, const uint8_t* data, size_t len, const char** err) {
    if (s->got + len > s->len) { *err = "too much data"; return false; }
    mbedtls_md_update(&s->md, data, len);
    if (esp_ota_write(s->handle, data, len) != ESP_OK) { *err = "flash write failed"; return false; }
    s->got += len;
    return true;
}

void abort(Session* s) {
    if (!s) return;
    esp_ota_abort(s->handle);
    mbedtls_md_free(&s->md);
    gBusy = false;
}

bool finish(Session* s, const char** err) {
    uint8_t hash[32];
    mbedtls_md_finish(&s->md, hash);
    if (s->got != s->len) { *err = "upload incomplete"; abort(s); return false; }
    if (memcmp(hash, s->want, 32) != 0) { *err = "image does not match its signature"; abort(s); return false; }
    mbedtls_md_free(&s->md);
    if (esp_ota_end(s->handle) != ESP_OK) { *err = "image is not valid firmware"; gBusy = false; return false; }
    if (esp_ota_set_boot_partition(s->part) != ESP_OK) { *err = "could not select the new firmware"; gBusy = false; return false; }
    Preferences p;
    p.begin("atds_ota", false);
    p.putUInt("expect", s->version);   // checked after the reboot: did it come up?
    p.end();
    gBusy = false;
    return true;
}

bool pendingVerify() {
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void confirm() {
    if (pendingVerify()) esp_ota_mark_app_valid_cancel_rollback();
    Preferences p;
    p.begin("atds_ota", false);
    p.remove("expect");
    p.end();
}

bool rolledBack(uint32_t* failed) {
    Preferences p;
    p.begin("atds_ota", false);
    const uint32_t expect = p.getUInt("expect", 0);
    const bool back = expect && expect != ATDS_FW_VERSION;
    if (back) p.remove("expect");   // same version: kept until confirm()
    p.end();
    *failed = expect;
    return back;
}

}  // namespace ota

// The new image confirms itself from loop() after a health check
// (see main.cpp), instead of the Arduino core doing it at once.
extern "C" bool verifyRollbackLater() { return true; }
