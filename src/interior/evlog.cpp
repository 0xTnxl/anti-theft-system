#include "evlog.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>

#include "config_interior.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "rtc.h"

namespace evlog {
namespace {

const char* const FILES[2] = {"/log0.bin", "/log1.bin"};

struct __attribute__((packed)) Head {
    uint32_t seq;      // newest record, 0 = empty log
    uint8_t tag[16];
    uint8_t file;      // file holding the newest record
    uint32_t lastRtc;  // newest valid RTC time logged
};
struct __attribute__((packed)) Anchor {
    uint32_t seq;          // first retained record
    uint8_t prevTag[16];   // tag preceding it (zeros for the very first record)
};

Preferences gPrefs;
uint8_t gKey[32];
Head gHead = {};
Anchor gAnchor = {};
uint32_t gBoot = 0;
bool gReady = false;
// Clock-jump baseline: RTC time and uptime when it was last trusted.
uint32_t gBaseRtc = 0, gBaseMs = 0;
bool gBaseValid = false;
uint32_t gLastCheckMs = 0;
bool gInvalidLogged = false;

void hmac(const uint8_t* key, size_t keyLen, const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen,
          uint8_t out[32]) {
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&c, key, keyLen);
    mbedtls_md_hmac_update(&c, a, aLen);
    if (b) mbedtls_md_hmac_update(&c, b, bLen);
    mbedtls_md_hmac_finish(&c, out);
    mbedtls_md_free(&c);
}

void computeTag(const uint8_t prev[16], const Body& b, uint8_t out[16]) {
    uint8_t full[32];
    hmac(gKey, sizeof(gKey), prev, 16, (const uint8_t*)&b, sizeof(b), full);
    memcpy(out, full, 16);
    mbedtls_platform_zeroize(full, sizeof(full));
}

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

uint32_t recordsIn(uint8_t f) {
    File h = LittleFS.open(FILES[f], "r");
    if (!h) return 0;
    const uint32_t n = h.size() / sizeof(Record);
    h.close();
    return n;
}

bool readRecord(uint8_t f, uint32_t idx, Record& r) {
    File h = LittleFS.open(FILES[f], "r");
    if (!h) return false;
    const bool ok = h.seek(idx * sizeof(Record)) && h.read((uint8_t*)&r, sizeof(r)) == sizeof(r);
    h.close();
    return ok;
}

void saveHead() { gPrefs.putBytes("head", &gHead, sizeof(gHead)); }
void saveAnchor() { gPrefs.putBytes("anchor", &gAnchor, sizeof(gAnchor)); }

// Drops a trailing partial record (power cut during a write) by rewriting the file.
bool repairPartial(uint8_t f) {
    File h = LittleFS.open(FILES[f], "r");
    if (!h) return false;
    const size_t size = h.size();
    if (size % sizeof(Record) == 0) {
        h.close();
        return false;
    }
    File t = LittleFS.open("/logtmp.bin", "w");
    Record r;
    for (size_t i = 0; i < size / sizeof(Record); i++) {
        h.read((uint8_t*)&r, sizeof(r));
        t.write((const uint8_t*)&r, sizeof(r));
    }
    h.close();
    t.close();
    LittleFS.remove(FILES[f]);
    LittleFS.rename("/logtmp.bin", FILES[f]);
    return true;
}

uint8_t readTime(uint32_t* t) {
    bool lost = false;
    if (!rtc::present()) {
        *t = 0;
        return RTC_NONE;
    }
    if (!rtc::read(t, &lost)) {
        *t = 0;
        return RTC_ERROR;
    }
    return lost ? RTC_LOST_POWER : RTC_OK;
}

Observer gObserver = nullptr;

bool writeRecord(Event e, uint8_t slot, uint8_t detail, uint32_t arg1, uint32_t arg2, uint32_t t,
                 uint8_t state) {
    Record r = {};
    r.body.seq = gHead.seq + 1;
    r.body.boot = gBoot;
    r.body.uptimeMs = millis();
    r.body.rtc = t;
    r.body.rtcState = state;
    r.body.event = e;
    r.body.slot = slot;
    r.body.detail = detail;
    r.body.arg1 = arg1;
    r.body.arg2 = arg2;
    computeTag(gHead.tag, r.body, r.tag);

    uint8_t f = gHead.file;
    if (recordsIn(f) >= LOG_RECORDS_PER_FILE) {
        // Rotate: the other file is the older one. Its last tag precedes the
        // first record of the current file, which becomes the oldest retained.
        const uint8_t other = f ^ 1;
        const uint32_t n = recordsIn(other);
        Record last, first;
        if (n && readRecord(other, n - 1, last) && readRecord(f, 0, first)) {
            gAnchor.seq = first.body.seq;
            memcpy(gAnchor.prevTag, last.tag, 16);
            saveAnchor();
        }
        LittleFS.remove(FILES[other]);
        f = other;
    }
    File h = LittleFS.open(FILES[f], "a");
    if (!h) return false;
    const bool ok = h.write((const uint8_t*)&r, sizeof(r)) == sizeof(r);
    h.close();
    if (!ok) return false;
    gHead.seq = r.body.seq;
    memcpy(gHead.tag, r.tag, 16);
    gHead.file = f;
    if (state == RTC_OK && t > gHead.lastRtc) gHead.lastRtc = t;
    saveHead();
    if (gObserver) gObserver(e, detail, arg1);
    return true;
}

// Logs a clock jump if the RTC disagrees with uptime since the baseline.
void checkClock(uint32_t t, uint8_t state) {
    if (state != RTC_OK) {
        if (state == RTC_LOST_POWER && !gInvalidLogged) {
            gInvalidLogged = true;
            writeRecord(EV_TIME_INVALID, NO_SLOT, 0, 0, t, t, state);
        }
        return;
    }
    const uint32_t ms = millis();
    if (gBaseValid) {
        const uint32_t expected = gBaseRtc + (ms - gBaseMs) / 1000;
        const int64_t diff = (int64_t)t - (int64_t)expected;
        if (diff > (int64_t)CLOCK_JUMP_TOLERANCE_S || diff < -(int64_t)CLOCK_JUMP_TOLERANCE_S) {
            Serial.printf("[log] CLOCK JUMP: expected %lu, RTC says %lu\n", (unsigned long)expected,
                          (unsigned long)t);
            writeRecord(EV_CLOCK_JUMP, NO_SLOT, 0, expected, t, t, state);
        } else {
            return;   // keep the original baseline so slow drift does not hide
        }
    }
    gBaseRtc = t;
    gBaseMs = ms;
    gBaseValid = true;
}

}  // namespace

const char* eventName(uint8_t e) {
    static const char* n[] = {"?",          "BOOT",         "GRANTED",       "DENIED_PIN",    "DENIED_CARD",
                              "DENIED_NO_PIN", "LOCKOUT",   "LOCK_REQUEST",  "LOCK_DENIED_CARD", "LOCKED",
                              "UNLOCKED",   "DOOR_OPEN",    "DOOR_CLOSED",   "ALARM",         "ALARM_CLEARED",
                              "FAULT",      "CARD_ADDED",   "CARD_REMOVED",  "PIN_CHANGED",   "WIPE",
                              "TIME_SET",   "CLOCK_JUMP",   "CLOCK_BACKWARDS", "TIME_INVALID", "LOG_ROLLBACK",
                              "LOG_REPAIRED", "LOCKOUT_RESET", "LATCH_RELEASED", "WEB_LOGIN", "WEB_LOGIN_FAIL",
                              "PHONE_PIN",  "WEB_ACTION",   "OTA_START",     "OTA_INSTALLED", "OTA_FAIL",
                              "OTA_CONFIRMED", "OTA_ROLLED_BACK", "HOME_WIFI", "ZEND_COMMAND", "ZEND_ANCHOR", "ZEND_ALERT"};
    return e < sizeof(n) / sizeof(n[0]) ? n[e] : "?";
}

bool begin(const uint8_t psk[32]) {
    static const char label[] = "ATDS log key v1";
    hmac(psk, 32, (const uint8_t*)label, sizeof(label) - 1, nullptr, 0, gKey);
    if (!LittleFS.begin(true)) {
        Serial.println("[log] LittleFS mount failed: event log disabled");
        return false;
    }
    gPrefs.begin("atds_log", false);
    gBoot = gPrefs.getUInt("boot", 0) + 1;
    gPrefs.putUInt("boot", gBoot);
    if (gPrefs.getBytesLength("head") == sizeof(gHead)) gPrefs.getBytes("head", &gHead, sizeof(gHead));
    if (gPrefs.getBytesLength("anchor") == sizeof(gAnchor)) gPrefs.getBytes("anchor", &gAnchor, sizeof(gAnchor));
    if (gHead.file > 1) gHead.file = 0;
    if (!gAnchor.seq) gAnchor.seq = 1;
    gReady = true;

    bool repaired = repairPartial(0);
    repaired = repairPartial(1) || repaired;

    // Compare the file's newest record with the NVS head.
    uint32_t fileSeq = 0;
    Record last;
    const uint32_t n = recordsIn(gHead.file);
    if (n && readRecord(gHead.file, n - 1, last)) fileSeq = last.body.seq;
    uint32_t rollback = 0;
    if (fileSeq == gHead.seq + 1) {
        // Power cut between the file write and the NVS update: accept the record
        // if it chains from the head.
        uint8_t t[16];
        computeTag(gHead.tag, last.body, t);
        if (ctEqual(t, last.tag, 16)) {
            gHead.seq = fileSeq;
            memcpy(gHead.tag, last.tag, 16);
            saveHead();
        }
    } else if (fileSeq < gHead.seq) {
        rollback = fileSeq;
        Serial.printf("[log] WARNING: log file ends at seq %lu but NVS head is %lu (truncated or rolled back)\n",
                      (unsigned long)fileSeq, (unsigned long)gHead.seq);
    }

    const uint32_t headBefore = gHead.seq;
    uint32_t t;
    const uint8_t st = readTime(&t);
    append(EV_BOOT, NO_SLOT, 0, (uint32_t)esp_reset_reason(), gBoot);
    if (repaired) append(EV_LOG_REPAIRED);
    if (rollback || (fileSeq == 0 && headBefore > 0)) append(EV_LOG_ROLLBACK, NO_SLOT, 0, rollback, headBefore);
    if (st == RTC_OK && gHead.lastRtc && t + CLOCK_JUMP_TOLERANCE_S < gHead.lastRtc) {
        Serial.println("[log] WARNING: RTC is earlier than the last logged time");
        append(EV_CLOCK_BACKWARDS, NO_SLOT, 0, gHead.lastRtc, t);
    }
    Serial.printf("[log] %lu record(s), head seq %lu, boot %lu, RTC %s\n", (unsigned long)count(),
                  (unsigned long)gHead.seq, (unsigned long)gBoot,
                  st == RTC_OK ? "ok" : st == RTC_NONE ? "not fitted" : st == RTC_LOST_POWER ? "LOST POWER (set the time)" : "read error");
    return true;
}

bool append(Event e, uint8_t slot, uint8_t detail, uint32_t arg1, uint32_t arg2) {
    if (!gReady) return false;
    uint32_t t;
    const uint8_t st = readTime(&t);
    checkClock(t, st);
    return writeRecord(e, slot, detail, arg1, arg2, t, st);
}

void service(uint32_t nowMs) {
    if (!gReady || !rtc::present() || nowMs - gLastCheckMs < 60000) return;
    gLastCheckMs = nowMs;
    uint32_t t;
    const uint8_t st = readTime(&t);
    checkClock(t, st);
}

bool setTime(uint32_t unixS) {
    if (!gReady || !rtc::present()) return false;
    uint32_t old;
    readTime(&old);
    if (!rtc::set(unixS)) return false;
    gBaseRtc = unixS;   // the change itself is logged, so it is not a "jump"
    gBaseMs = millis();
    gBaseValid = true;
    gInvalidLogged = false;
    if (unixS < gHead.lastRtc) gHead.lastRtc = unixS;   // later records are compared to the new time
    return append(EV_TIME_SET, NO_SLOT, 0, old, unixS);
}

uint32_t now(uint8_t* state) {
    uint32_t t;
    const uint8_t st = readTime(&t);
    if (state) *state = st;
    return t;
}

void setObserver(Observer fn) { gObserver = fn; }
void headTag(uint8_t out[16]) { memcpy(out, gHead.tag, 16); }
uint32_t headSeq() { return gHead.seq; }
uint32_t bootCount() { return gBoot; }

uint32_t count() {
    if (!gReady) return 0;
    return recordsIn(0) + recordsIn(1);
}

// Oldest file first: the one not holding the head, if it has records.
static void order(uint8_t files[2], uint8_t* nf) {
    const uint8_t cur = gHead.file, other = cur ^ 1;
    *nf = 0;
    if (recordsIn(other)) files[(*nf)++] = other;
    files[(*nf)++] = cur;
}

bool verify(uint32_t* checked, uint32_t* badSeq, uint32_t* backwards) {
    *checked = 0;
    *badSeq = 0;
    *backwards = 0;
    if (!gReady) return false;
    uint8_t files[2], nf;
    order(files, &nf);
    uint8_t prev[16];
    memcpy(prev, gAnchor.prevTag, 16);
    uint32_t expect = gAnchor.seq, lastRtc = 0;
    for (uint8_t k = 0; k < nf; k++) {
        File h = LittleFS.open(FILES[files[k]], "r");
        if (!h) continue;
        Record r;
        while (h.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
            uint8_t t[16];
            computeTag(prev, r.body, t);
            if (r.body.seq != expect || !ctEqual(t, r.tag, 16)) {
                *badSeq = r.body.seq ? r.body.seq : expect;
                h.close();
                return false;
            }
            if (r.body.rtcState == RTC_OK) {
                if (r.body.rtc + CLOCK_JUMP_TOLERANCE_S < lastRtc && r.body.event != EV_TIME_SET) (*backwards)++;
                lastRtc = r.body.rtc;
            }
            memcpy(prev, r.tag, 16);
            expect++;
            (*checked)++;
        }
        h.close();
    }
    if (expect - 1 != gHead.seq || !ctEqual(prev, gHead.tag, 16)) {
        *badSeq = expect;   // chain ends before the NVS head
        return false;
    }
    return true;
}

static void hex(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) Serial.printf("%02x", d[i]);
}

void dump() {
    uint8_t files[2], nf;
    order(files, &nf);
    Serial.printf("ATDSLOG 1 anchor %lu ", (unsigned long)gAnchor.seq);
    hex(gAnchor.prevTag, 16);
    Serial.printf(" head %lu ", (unsigned long)gHead.seq);
    hex(gHead.tag, 16);
    Serial.println();
    for (uint8_t k = 0; k < nf; k++) {
        File h = LittleFS.open(FILES[files[k]], "r");
        if (!h) continue;
        Record r;
        while (h.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
            Serial.print("R ");
            hex((const uint8_t*)&r, sizeof(r));
            Serial.println();
        }
        h.close();
    }
    Serial.println("ATDSLOG end");
}

void forEach(uint32_t lastN, RecordFn fn, void* ctx) {
    if (!gReady) return;
    uint8_t files[2], nf;
    order(files, &nf);
    const uint32_t total = count();
    const uint32_t skip = total > lastN ? total - lastN : 0;
    uint32_t i = 0;
    for (uint8_t k = 0; k < nf; k++) {
        File h = LittleFS.open(FILES[files[k]], "r");
        if (!h) continue;
        Record r;
        while (h.read((uint8_t*)&r, sizeof(r)) == sizeof(r))
            if (i++ >= skip) fn(r, ctx);
        h.close();
    }
}

void show(uint32_t lastN) {
    uint8_t files[2], nf;
    order(files, &nf);
    const uint32_t total = count();
    const uint32_t skip = total > lastN ? total - lastN : 0;
    uint32_t i = 0;
    for (uint8_t k = 0; k < nf; k++) {
        File h = LittleFS.open(FILES[files[k]], "r");
        if (!h) continue;
        Record r;
        while (h.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
            if (i++ < skip) continue;
            char when[24] = "no clock";
            if (r.body.rtcState == RTC_OK || r.body.rtcState == RTC_LOST_POWER) {
                int y, mo, d, hh, mi, s;
                rtc::toCivil(r.body.rtc, &y, &mo, &d, &hh, &mi, &s);
                snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d%s", y, mo, d, hh, mi, s,
                         r.body.rtcState == RTC_LOST_POWER ? "?" : "");
            }
            Serial.printf("#%-6lu %s  boot %lu +%lus  %-16s", (unsigned long)r.body.seq, when,
                          (unsigned long)r.body.boot, (unsigned long)(r.body.uptimeMs / 1000),
                          eventName(r.body.event));
            if (r.body.slot != NO_SLOT) Serial.printf(" card %u", r.body.slot);
            if (r.body.detail) Serial.printf(" d=%u", r.body.detail);
            if (r.body.arg1 || r.body.arg2)
                Serial.printf(" %lu %lu", (unsigned long)r.body.arg1, (unsigned long)r.body.arg2);
            Serial.println();
        }
        h.close();
    }
}

}  // namespace evlog
