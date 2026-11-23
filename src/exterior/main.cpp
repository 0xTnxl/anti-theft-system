// Exterior module: card + PIN entry terminal. Holds no credentials and has no
// way to command an unlock; it forwards what it reads to the interior.
//
// Shared pins on Rev B (docs/DESIGN.md 3.1):
//   GPIO23 = keypad C4 and RC522 MOSI -> CARD phase (SPI on) and PIN phase
//            (SPI off, keypad on) never overlap.
//   GPIO22 = OLED SCL and RC522 RST   -> every OLED transfer resets the RC522,
//            so it is re-initialised before the next card poll.
#include <Arduino.h>
#include <SPI.h>
#include <esp_random.h>
#include <string.h>

#include "config_common.h"
#include "config_exterior.h"
#include "espnow_transport.h"
#include "keypad.h"
#include "mbedtls/platform_util.h"
#include "oled.h"
#include "protocol.h"
#include "rc522.h"
#include "secrets_guard.h"
#include "secure_link.h"

namespace {

enum class Ui : uint8_t { Linking, CardWait, PinEntry, Verifying, Result, Lockout };
enum class Phase : uint8_t { None, Card, Pin };
enum class Tone : uint8_t { Ok, Fail, Info };

slink::SecureLink gLink;
Keypad gKeypad;
Rc522 gRfid;
Oled gOled;

Ui gUi = Ui::Linking;
Phase gPhase = Phase::None;
bool gUiDirty = true;

// Link
uint32_t gLastRxMs = 0;
uint32_t gLastHsMs = 0;
bool gHsSent = false;
uint32_t gLastHbMs = 0;
uint32_t gSessionUpMs = 0;
bool gSessionConfirmed = false;   // interior has answered on a session (UI shows link up)
bool gAwaitConfirm = false;       // newest session not yet answered (HS3 may be lost)
proto::Status gStatus = {};

// Card / PIN (wiped as soon as they are sent)
uint8_t gUid[proto::UID_MAX];
uint8_t gUidLen = 0;
char gPin[proto::PIN_MAX];
uint8_t gPinLen = 0;
bool gPinTooShort = false;
uint32_t gLastKeyMs = 0;

// RFID
bool gRfidNeedsInit = true;
bool gRfidWarned = false;
uint32_t gLastRfidPollMs = 0;
uint32_t gCardCooldownUntil = 0;

// Verify / result / lockout
uint32_t gReqId = 0;
uint32_t gVerifyStartMs = 0;
uint32_t gResultUntil = 0;
char gResultLine1[22];
char gResultLine2[11];
Tone gResultTone = Tone::Info;
uint32_t gLockoutUntil = 0;

uint32_t gLastActivityMs = 0;

// Console + "identify" (slow blink on the red LED and the DevKit's onboard LED).
constexpr uint8_t PIN_ONBOARD_LED = 2;   // DevKit V1 blue LED, not used on Rev B
char gTestLine[48];
uint8_t gTestLen = 0;
uint32_t gIdentifyUntil = 0;

void rng(uint8_t* b, size_t n) { esp_fill_random(b, n); }

void wipeCredentials() {
    mbedtls_platform_zeroize(gUid, sizeof(gUid));
    mbedtls_platform_zeroize(gPin, sizeof(gPin));
    gUidLen = 0;
    gPinLen = 0;
}

bool linkUp(uint32_t now) { return gLink.established(now) && gSessionConfirmed; }

// ---------------------------------------------------------------- phases ---

void setPhase(Phase p) {
    if (p == gPhase) return;
    if (gPhase == Phase::Card) {
        gRfid.antennaOff();
        SPI.end();
    } else if (gPhase == Phase::Pin) {
        gKeypad.end();
    }
    // All shared pins are released at this point.
    if (p == Phase::Card) {
        SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
        gRfidNeedsInit = true;
    } else if (p == Phase::Pin) {
        gKeypad.begin();
    }
    gPhase = p;
}

// Serial event log for tools/flow_test.py. Never prints PIN digits or card UIDs.
const char* uiName(Ui ui) {
    static const char* n[] = {"Linking", "CardWait", "PinEntry", "Verifying", "Result", "Lockout"};
    return n[(uint8_t)ui];
}

void setUi(Ui ui, uint32_t now) {
    if (ui != gUi) Serial.printf("[ui] %s -> %s\n", uiName(gUi), uiName(ui));
    gUi = ui;
    gUiDirty = true;
    gLastActivityMs = now;
    switch (ui) {
        case Ui::CardWait:
            setPhase(Phase::Card);
            gCardCooldownUntil = now + CARD_COOLDOWN_MS;
            break;
        case Ui::PinEntry:
            setPhase(Phase::Pin);
            break;
        default:
            setPhase(Phase::None);
            break;
    }
}

// --------------------------------------------------------------- display ---

void render(uint32_t now) {
    if (!gUiDirty && gUi != Ui::Lockout) return;
    gUiDirty = false;
    char line[24];
    gOled.clear();
    switch (gUi) {
        case Ui::Linking:
            gOled.textCentered(0, "DOOR LOCK");
            gOled.textCentered(16, "CONNECTING", 1);
            break;
        case Ui::CardWait: {
            const char* top = "LOCKED";
            if (gStatus.alarm != proto::ALARM_NONE) top = "!! ALARM !!";
            else if (gStatus.flags & proto::STATUS_ENROLL) top = "ENROLL MODE";
            else if (gStatus.lock == proto::LOCK_FAULT) top = "LOCK FAULT";
            else if (gStatus.lock == proto::LOCK_UNLOCKED) top = "UNLOCKED";
            gOled.textCentered(0, top);
            gOled.textCentered(14, "TAP CARD", 2);
            break;
        }
        case Ui::PinEntry: {
            gOled.textCentered(0, gPinTooShort ? "MIN 4 DIGITS" : "ENTER PIN  # = OK");
            uint8_t i = 0;
            for (; i < gPinLen && i < sizeof(line) - 1; i++) line[i] = '*';
            line[i] = 0;
            gOled.textCentered(14, gPinLen ? line : "_", 2);
            break;
        }
        case Ui::Verifying:
            gOled.textCentered(10, "CHECKING", 2);
            break;
        case Ui::Result:
            gOled.textCentered(0, gResultLine1);
            gOled.textCentered(14, gResultLine2, 2);
            break;
        case Ui::Lockout: {
            const uint32_t left = gLockoutUntil > now ? (gLockoutUntil - now + 999) / 1000 : 0;
            snprintf(line, sizeof(line), "%02lu:%02lu", (unsigned long)(left / 60),
                     (unsigned long)(left % 60));
            gOled.textCentered(0, "LOCKED OUT");
            gOled.textCentered(14, line, 2);
            break;
        }
    }
    if (!gOled.isOn()) gOled.power(true);
    gOled.commit();
}

void updateLeds(uint32_t now) {
    const bool blinkFast = (now / 125) & 1;
    const bool blinkSlow = (now / 500) & 1;
    bool g = false, y = false, r = false;
    switch (gUi) {
        case Ui::Linking: y = true; break;
        case Ui::PinEntry:
        case Ui::Verifying: y = true; break;
        case Ui::Result:
            g = gResultTone == Tone::Ok;
            r = gResultTone == Tone::Fail;
            y = gResultTone == Tone::Info;
            break;
        case Ui::Lockout: y = blinkSlow; break;
        case Ui::CardWait: break;
    }
    if (gStatus.alarm != proto::ALARM_NONE && gUi != Ui::Result) r = blinkFast;
    const bool ident = (int32_t)(gIdentifyUntil - now) > 0;
    if (ident) {
        g = y = false;
        r = blinkSlow;
    }
    digitalWrite(PIN_ONBOARD_LED, ident && blinkSlow);
    digitalWrite(PIN_LED_GREEN, g);
    digitalWrite(PIN_LED_YELLOW, y);
    digitalWrite(PIN_LED_RED, r);
}

void showResult(const char* l1, const char* l2, Tone tone, uint32_t ms, uint32_t now) {
    strncpy(gResultLine1, l1, sizeof(gResultLine1) - 1);
    gResultLine1[sizeof(gResultLine1) - 1] = 0;
    strncpy(gResultLine2, l2, sizeof(gResultLine2) - 1);
    gResultLine2[sizeof(gResultLine2) - 1] = 0;
    gResultTone = tone;
    gResultUntil = now + ms;
    setUi(Ui::Result, now);
}

// ------------------------------------------------------------------ link ---

void sendMsg(uint8_t type, const void* payload, size_t len, uint32_t now) {
    uint8_t frame[slink::MAX_FRAME];
    const size_t n = gLink.seal(type, payload, len, frame, sizeof(frame), now);
    if (n) espnow::send(frame, n);
}

void sendHeartbeat(uint32_t now) {
    proto::Heartbeat hb = {};
    hb.uptimeS = now / 1000;
    hb.extState = (uint8_t)gUi;
    sendMsg(proto::HEARTBEAT, &hb, sizeof(hb), now);
    gLastHbMs = now;
}

const char* codeName(uint8_t c) {
    static const char* names[] = {"?",    "GRANTED",      "DENIED",    "LOCKED_OUT", "FAULT",
                                  "BUSY", "ENROLL_AGAIN", "ENROLL_OK", "ENROLL_FAIL"};
    return c < 9 ? names[c] : "?";
}

void onAuthResult(const proto::AuthResult& r, uint32_t now) {
    if (gUi != Ui::Verifying || r.reqId != gReqId) return;
    Serial.printf("[auth] result %s attempts_left=%u lockout=%us\n", codeName(r.code),
                  r.attemptsLeft, r.lockoutS);
    char buf[22];
    switch (r.code) {
        case proto::AUTH_GRANTED:
            showResult("ACCESS GRANTED", "WELCOME", Tone::Ok, RESULT_OK_MS, now);
            break;
        case proto::AUTH_DENIED:
            snprintf(buf, sizeof(buf), "%u TRIES LEFT", r.attemptsLeft);
            showResult(buf, "DENIED", Tone::Fail, RESULT_FAIL_MS, now);
            break;
        case proto::AUTH_LOCKED_OUT:
            gLockoutUntil = now + (uint32_t)r.lockoutS * 1000;
            setUi(Ui::Lockout, now);
            break;
        case proto::AUTH_BUSY:
            showResult("TRY AGAIN", "BUSY", Tone::Info, RESULT_FAIL_MS, now);
            break;
        case proto::AUTH_ENROLL_AGAIN:
            showResult("SAME CARD + PIN", "REPEAT", Tone::Info, RESULT_OK_MS, now);
            break;
        case proto::AUTH_ENROLL_OK:
            showResult("CARD ENROLLED", "SAVED", Tone::Ok, RESULT_OK_MS, now);
            break;
        case proto::AUTH_ENROLL_FAIL:
            showResult("ENROLL FAILED", "ERROR", Tone::Fail, RESULT_FAIL_MS, now);
            break;
        default:
            showResult("LOCK FAULT", "FAULT", Tone::Fail, RESULT_FAIL_MS, now);
            break;
    }
}

void onMessage(const slink::RxResult& r, uint32_t now) {
    if (r.payloadLen != proto::payloadSize(r.type)) return;
    gLastRxMs = now;
    switch (r.type) {
        case proto::STATUS: {
            proto::Status s;
            memcpy(&s, r.payload, sizeof(s));
            if (gAwaitConfirm) Serial.println("[link] session confirmed");
            gSessionConfirmed = true;
            gAwaitConfirm = false;
            if (memcmp(&s, &gStatus, sizeof(s)) != 0) {
                Serial.printf("[status] lock=%u alarm=%u flags=0x%02X\n", s.lock, s.alarm, s.flags);
                gStatus = s;
                if (gUi == Ui::CardWait) gUiDirty = true;
            }
            break;
        }
        case proto::AUTH_RESULT: {
            proto::AuthResult a;
            memcpy(&a, r.payload, sizeof(a));
            onAuthResult(a, now);
            break;
        }
        default:
            break;
    }
}

void serviceLink(uint32_t now) {
    espnow::Packet p;
    while (espnow::receive(p)) {
        slink::RxResult r;
        gLink.onFrame(p.data, p.len, now, r);
        if (r.replyLen) espnow::send(r.reply, r.replyLen);
        if (r.kind == slink::RxResult::SessionUp) {
            Serial.println("[link] session keys installed");
            gSessionUpMs = now;
            gAwaitConfirm = true;   // a rekey keeps gSessionConfirmed, so the UI does not flicker
            gLastRxMs = now;
            sendHeartbeat(now);   // interior answers with STATUS -> confirms HS3 arrived
        } else if (r.kind == slink::RxResult::Message) {
            onMessage(r, now);
        }
        mbedtls_platform_zeroize(r.payload, sizeof(r.payload));
    }

    if (gLink.established(now) && now - gLastRxMs > EXT_LINK_TIMEOUT_MS) {
        Serial.println("[link] interior silent, dropping session");
        gLink.reset();
        gSessionConfirmed = false;
        gAwaitConfirm = false;
    }
    if (gLink.established(now) && gAwaitConfirm && now - gSessionUpMs > SESSION_CONFIRM_MS) {
        Serial.println("[link] session not confirmed, re-handshaking");
        gLink.reset();
        gSessionConfirmed = false;
        gAwaitConfirm = false;
    }

    if (gLink.rekeyDue(now) && (!gHsSent || now - gLastHsMs >= HANDSHAKE_RETRY_MS)) {
        uint8_t hs[slink::MAX_FRAME];
        const size_t n = gLink.startHandshake(hs, sizeof(hs), now);
        if (n) espnow::send(hs, n);
        gLastHsMs = now;
        gHsSent = true;
    }

    if (gLink.established(now) && now - gLastHbMs >= HEARTBEAT_INTERVAL_MS) sendHeartbeat(now);

    const uint32_t fails = gLink.takeAuthFailures();
    if (fails) Serial.printf("[link] %lu frame(s) failed authentication\n", (unsigned long)fails);
}

// ------------------------------------------------------------- card / PIN ---

void submitPin(uint32_t now) {
    proto::AuthRequest req = {};
    esp_fill_random(&gReqId, sizeof(gReqId));
    req.reqId = gReqId;
    req.uidLen = gUidLen;
    memcpy(req.uid, gUid, gUidLen);
    req.pinLen = gPinLen;
    memcpy(req.pin, gPin, gPinLen);
    wipeCredentials();
    if (linkUp(now)) {
        sendMsg(proto::AUTH_REQUEST, &req, sizeof(req), now);
        gVerifyStartMs = now;
        setUi(Ui::Verifying, now);
    } else {
        showResult("NO LINK TO LOCK", "OFFLINE", Tone::Fail, RESULT_FAIL_MS, now);
    }
    mbedtls_platform_zeroize(&req, sizeof(req));
}

void pollRfid(uint32_t now) {
    if (now - gLastRfidPollMs < RFID_POLL_MS) return;
    if (now - gOled.lastBusMs() < RC522_RESET_SETTLE_MS) return;   // RC522 just reset by SCL
    gLastRfidPollMs = now;

    if (gRfidNeedsInit || !gRfid.isConfigured()) {
        if (!gRfid.init()) {
            if (!gRfidWarned) Serial.println("[rfid] RC522 not responding");
            gRfidWarned = true;
            return;
        }
        if (gRfidWarned || gRfidNeedsInit) {
            Serial.printf("[rfid] RC522 ready, version 0x%02X\n", gRfid.version());
        }
        gRfidWarned = false;
        gRfidNeedsInit = false;
    }

    uint8_t uid[proto::UID_MAX];
    uint8_t len = 0;
    if (!gRfid.readUid(uid, len)) return;
    if ((int32_t)(now - gCardCooldownUntil) < 0) return;
    memcpy(gUid, uid, len);
    gUidLen = len;
    mbedtls_platform_zeroize(uid, sizeof(uid));
    Serial.printf("[rfid] card read, %u-byte UID\n", len);
    gPinLen = 0;
    gPinTooShort = false;
    gLastKeyMs = now;
    setUi(Ui::PinEntry, now);
}

void handleKeys(uint32_t now) {
    const char k = gKeypad.poll(now);
    if (k) {
        gLastKeyMs = now;
        gLastActivityMs = now;
        gUiDirty = true;
        gPinTooShort = false;
        if (k >= '0' && k <= '9') Serial.printf("[key] digit (%u entered)\n", gPinLen < proto::PIN_MAX ? gPinLen + 1 : gPinLen);
        else Serial.printf("[key] %c\n", k);
        if (k >= '0' && k <= '9') {
            if (gPinLen < proto::PIN_MAX) gPin[gPinLen++] = k;
        } else if (k == KEY_CLEAR) {
            if (gPinLen) {
                mbedtls_platform_zeroize(gPin, sizeof(gPin));
                gPinLen = 0;
            } else {
                wipeCredentials();
                setUi(Ui::CardWait, now);
                return;
            }
        } else if (k == KEY_SUBMIT) {
            if (gPinLen >= proto::PIN_MIN) {
                submitPin(now);
                return;
            }
            gPinTooShort = true;
            Serial.printf("[ui] PIN too short (%u digits), not sent\n", gPinLen);
        }
    }
    if (now - gLastKeyMs > PIN_ENTRY_TIMEOUT_MS) {
        Serial.println("[ui] PIN entry timed out");
        wipeCredentials();
        setUi(Ui::CardWait, now);
    }
}

#ifdef ATDS_TEST_HOOKS
// TEST BUILD ONLY (env:exterior_test). "auth <uid hex> <pin>" over USB serial
// stands in for the RC522 and keypad so tools/link_test.py can drive the whole
// link without peripherals. Never ship a board with this build.

bool parseUid(const char* s, uint8_t* out, uint8_t& len) {
    const size_t n = strlen(s);
    if (n % 2 || n / 2 > proto::UID_MAX || n == 0) return false;
    for (size_t i = 0; i < n / 2; i++) {
        char b[3] = {s[2 * i], s[2 * i + 1], 0};
        char* end;
        out[i] = (uint8_t)strtoul(b, &end, 16);
        if (*end) return false;
    }
    len = (uint8_t)(n / 2);
    return true;
}

void testCommand(char* line, uint32_t now) {
    char* cmd = strtok(line, " ");
    char* uid = strtok(nullptr, " ");
    char* pin = strtok(nullptr, " ");
    if (!cmd || strcmp(cmd, "auth") != 0 || !uid || !pin) {
        Serial.println("[test] usage: auth <uid hex> <pin digits>");
        return;
    }
    const size_t pinLen = strlen(pin);
    bool digits = pinLen >= 1 && pinLen <= proto::PIN_MAX;   // short PINs allowed: interior must reject
    for (size_t i = 0; digits && i < pinLen; i++) digits = pin[i] >= '0' && pin[i] <= '9';
    uint8_t u[proto::UID_MAX];
    uint8_t uLen = 0;
    if (!digits || !parseUid(uid, u, uLen)) {
        Serial.println("[test] bad uid or pin");
        return;
    }
    // Lockout is allowed on purpose: the interior must refuse even if the exterior UI is bypassed.
    if (!linkUp(now) || !(gUi == Ui::CardWait || gUi == Ui::Lockout)) {
        Serial.println("[test] not ready");
        return;
    }
    memcpy(gUid, u, uLen);
    gUidLen = uLen;
    memcpy(gPin, pin, pinLen);
    gPinLen = (uint8_t)pinLen;
    mbedtls_platform_zeroize(u, sizeof(u));
    Serial.println("[test] auth sent");
    submitPin(now);
}

#endif

// USB serial console. Production: only "identify". Test build: also "auth".
void consoleCommand(char* line, uint32_t now) {
    if (!strncmp(line, "identify", 8) && (line[8] == 0 || line[8] == ' ')) {
        const int s = line[8] ? constrain(atoi(line + 9), 1, 300) : 30;
        gIdentifyUntil = now + (uint32_t)s * 1000;
        Serial.printf("[ext] EXTERIOR board: slow blink, red LED + onboard blue LED, for %d s\n", s);
        return;
    }
#ifdef ATDS_TEST_HOOKS
    testCommand(line, now);
#else
    Serial.println("[ext] commands: identify [seconds]");
#endif
}

void serviceConsole(uint32_t now) {
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (!gTestLen) continue;
            gTestLine[gTestLen] = 0;
            gTestLen = 0;
            consoleCommand(gTestLine, now);
        } else if (gTestLen < sizeof(gTestLine) - 1) {
            gTestLine[gTestLen++] = c;
        }
    }
}

}  // namespace

void setup() {
    setCpuFrequencyMhz(CPU_MHZ);
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[ext] anti-theft exterior module, Rev B wiring");
#ifdef ATDS_TEST_HOOKS
    Serial.printf("[ext] TEST BUILD: serial auth hook on, rekey every %lu s. Do not ship.\n",
                  (unsigned long)(slink::REKEY_INTERVAL_MS / 1000));
#endif

    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_ONBOARD_LED, OUTPUT);
    digitalWrite(PIN_ONBOARD_LED, LOW);
    digitalWrite(PIN_LED_YELLOW, HIGH);

    gKeypad.end();   // all keypad pins high-impedance until the PIN phase
    gRfid.begin(PIN_RC522_CS, RC522_SPI_HZ);
    gOled.setColumnOffset(OLED_COL_OFFSET);
    if (!gOled.begin(PIN_I2C_SDA, PIN_I2C_SCL, OLED_ADDR, OLED_I2C_HZ, OLED_FLIP))
        Serial.println("[oled] SSD1306 not found at 0x3C");

    if (!espnow::begin(ATDS_MAC_EXTERIOR, ATDS_MAC_INTERIOR, ESPNOW_CHANNEL, WIFI_MAX_TX_QDBM)) {
        Serial.println("[ext] ESP-NOW init failed, restarting in 5 s");
        delay(5000);
        ESP.restart();
    }
    gLink.begin(slink::Role::Exterior, ATDS_PSK, ATDS_MAC_EXTERIOR, ATDS_MAC_INTERIOR, rng);
    setUi(Ui::Linking, millis());
}

void loop() {
    const uint32_t now = millis();
    serviceLink(now);
    serviceConsole(now);
    const bool up = linkUp(now);

    switch (gUi) {
        case Ui::Linking:
            if (up) setUi(Ui::CardWait, now);
            break;
        case Ui::CardWait:
            if (!up) {
                setUi(Ui::Linking, now);
                break;
            }
            pollRfid(now);
            if (gOled.isOn() && now - gLastActivityMs > OLED_SLEEP_MS) gOled.power(false);
            break;
        case Ui::PinEntry:
            handleKeys(now);
            break;
        case Ui::Verifying:
            if (now - gVerifyStartMs > VERIFY_TIMEOUT_MS) {
                Serial.println("[auth] result TIMEOUT attempts_left=0 lockout=0s");
                showResult("NO RESPONSE", "TIMEOUT", Tone::Fail, RESULT_FAIL_MS, now);
            }
            break;
        case Ui::Result:
            if ((int32_t)(now - gResultUntil) >= 0) setUi(up ? Ui::CardWait : Ui::Linking, now);
            break;
        case Ui::Lockout:
            if ((int32_t)(now - gLockoutUntil) >= 0) setUi(up ? Ui::CardWait : Ui::Linking, now);
            break;
    }

    // While asleep in CardWait, only wake the display for a state change.
    if (gUi != Ui::CardWait || gOled.isOn() || gUiDirty) render(now);
    updateLeds(now);
    delay(1);
}
