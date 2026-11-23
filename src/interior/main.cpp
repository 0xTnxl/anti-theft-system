// Interior module: verifies card + PIN, drives the bolt motor (L298N + encoder),
// watches the door, limit switches, vibration and motion sensors, runs the alarm and the USB
// serial console (enrollment). See docs/DESIGN.md sections 5 and 6.2.
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#include "config_common.h"
#include "config_interior.h"
#include "credentials.h"
#include "espnow_transport.h"
#include "intrusion.h"
#include "motor.h"
#include "mbedtls/platform_util.h"
#include "protocol.h"
#include "secrets_guard.h"
#include "secure_link.h"

namespace {

// ------------------------------------------------------------- helpers ---

void rng(uint8_t* b, size_t n) { esp_fill_random(b, n); }

bool elapsed(uint32_t now, uint32_t since, uint32_t ms) { return now - since >= ms; }

bool ctEqual(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}

struct Debounced {
    uint8_t pin;
    int activeLevel;
    uint32_t debounceMs;
    bool stable = false;
    bool raw = false;
    uint32_t changedMs = 0;
    uint32_t stableSinceMs = 0;

    void begin(uint32_t now) {
        pinMode(pin, INPUT_PULLUP);
        delay(2);
        raw = stable = digitalRead(pin) == activeLevel;
        changedMs = stableSinceMs = now;
    }
    void update(uint32_t now) {
        const bool r = digitalRead(pin) == activeLevel;
        if (r != raw) {
            raw = r;
            changedMs = now;
        }
        if (raw != stable && elapsed(now, changedMs, debounceMs)) {
            stable = raw;
            stableSinceMs = now;
        }
    }
};

Debounced gSwLocked = {PIN_SW_LOCKED, SWITCH_PRESSED_LEVEL, SWITCH_DEBOUNCE_MS};
Debounced gSwUnlocked = {PIN_SW_UNLOCKED, SWITCH_PRESSED_LEVEL, SWITCH_DEBOUNCE_MS};
Debounced gDoorClosed = {PIN_REED, DOOR_CLOSED_LEVEL, REED_DEBOUNCE_MS};

// ---------------------------------------------------------------- motor ---

bool gMotorOn = false;
uint32_t gBrakeUntil = 0;   // 0 = not braking
uint32_t gMoveStartMs = 0;
int32_t gMoveStartCount = 0;

void motorRun(motor::Dir d, uint32_t now) {
    motor::drive(d, MOTOR_DUTY_PCT);
    gMotorOn = true;
    gBrakeUntil = 0;
    gMoveStartMs = now;
    gMoveStartCount = motor::count();
}

void motorStop(uint32_t now) {
    if (!gMotorOn) return;
    motor::brake();
    gMotorOn = false;
    gBrakeUntil = (now + MOTOR_BRAKE_MS) | 1;
}

void motorService(uint32_t now) {
    if (gBrakeUntil && (int32_t)(now - gBrakeUntil) >= 0) {
        motor::coast();
        gBrakeUntil = 0;
    }
}

void logMove(const char* what, uint32_t now) {
    Serial.printf("[lock] %s in %lu ms, encoder %ld counts\n", what,
                  (unsigned long)(now - gMoveStartMs), (long)(motor::count() - gMoveStartCount));
}

// --------------------------------------------------------------- buzzer ---

struct Beep {
    uint16_t hz;
    uint16_t onMs;
    uint16_t offMs;
    uint8_t left;
    uint32_t stepMs;
    bool on;
} gBeep = {};
uint16_t gToneHz = 0;

void buzz(uint16_t hz) {
    if (hz == gToneHz) return;
    ledcWriteTone(BUZZER_LEDC_CH, hz);
    gToneHz = hz;
}

void beep(uint16_t hz, uint16_t onMs, uint16_t offMs, uint8_t count) {
    gBeep = {hz, onMs, offMs, count, millis(), false};
}

// ----------------------------------------------------------------- state ---

slink::SecureLink gLink;
uint32_t gLastRxMs = 0;
bool gStatusDirty = true;
uint32_t gLastStatusMs = 0;

proto::LockState gLock = proto::LOCK_UNKNOWN;
uint32_t gLockSince = 0;
bool gRetried = false;
uint32_t gBackoffUntil = 0;      // LOCKING retry: back off towards unlock first
bool gDoorOpenedSinceUnlock = false;

proto::AlarmCause gAlarm = proto::ALARM_NONE;
uint32_t gAlarmSince = 0;
bool gDisarmed = false;
bool gArmedPrev = false;
uint32_t gArmedSince = 0;

uint32_t gRfFails[RF_TAMPER_FAILS] = {};
uint8_t gRfFailIdx = 0;

uint32_t gLockoutUntil = 0;
uint32_t gLastAuthMs = 0;
bool gAuthSeen = false;

bool gEnrollMode = false;
uint32_t gEnrollSince = 0;
bool gEnrollPending = false;
uint8_t gEnrollUid[proto::UID_MAX];
uint8_t gEnrollUidLen = 0;
char gEnrollPin[proto::PIN_MAX];
uint8_t gEnrollPinLen = 0;

const char* lockName(uint8_t s) {
    static const char* names[] = {"UNKNOWN", "LOCKED", "UNLOCKING", "UNLOCKED", "LOCKING", "FAULT"};
    return s < 6 ? names[s] : "?";
}

const char* alarmName(uint8_t a) {
    static const char* names[] = {"NONE",      "FORCED_DOOR", "VIBRATION", "TAMPER_LINK",
                                  "TAMPER_RF", "BOLT_TAMPER", "MOTION"};
    return a < 7 ? names[a] : "?";
}

void setLock(proto::LockState s, uint32_t now) {
    if (s != gLock) Serial.printf("[lock] %s -> %s\n", lockName(gLock), lockName(s));
    gLock = s;
    gLockSince = now;
    gStatusDirty = true;
}

void startUnlock(uint32_t now) {
    gBackoffUntil = 0;
    motorRun(motor::Dir::Unlock, now);
    setLock(proto::LOCK_UNLOCKING, now);
}

void startLock(uint32_t now) {
    if (!gDoorClosed.stable) return;   // never throw the bolt with the door open
    gRetried = false;
    gBackoffUntil = 0;
    motorRun(motor::Dir::Lock, now);
    setLock(proto::LOCK_LOCKING, now);
}

void fault(uint32_t now, const char* why) {
    Serial.printf("[lock] FAULT: %s (encoder %ld counts this move)\n", why,
                  (long)(motor::count() - gMoveStartCount));
    motorStop(now);
    setLock(proto::LOCK_FAULT, now);
    beep(400, 150, 100, 3);
}

void raiseAlarm(proto::AlarmCause cause, uint32_t now) {
    if (gDisarmed || gAlarm != proto::ALARM_NONE) return;
    gAlarm = cause;
    gAlarmSince = now;
    gStatusDirty = true;
    Serial.printf("[alarm] RAISED: %s\n", alarmName(cause));
}

void clearAlarm(const char* by) {
    if (gAlarm == proto::ALARM_NONE) return;
    Serial.printf("[alarm] cleared by %s\n", by);
    gAlarm = proto::ALARM_NONE;
    gStatusDirty = true;
}

bool armedBase() { return !gDisarmed && gLock == proto::LOCK_LOCKED && gDoorClosed.stable; }

// --------------------------------------------------------- lock machine ---

void updateLock(uint32_t now) {
    switch (gLock) {
        case proto::LOCK_UNLOCKING:
            // Stop on the first contact (raw), confirm with the debounced state.
            if (gMotorOn && gSwUnlocked.raw) {
                motorStop(now);
                logMove("unlock switch reached", now);
            }
            if (!gMotorOn && gSwUnlocked.stable) {
                gDoorOpenedSinceUnlock = !gDoorClosed.stable;
                setLock(proto::LOCK_UNLOCKED, now);
            } else if (gMotorOn && motor::stalled(now)) {
                fault(now, "motor stalled while unlocking (no encoder movement)");
            } else if (elapsed(now, gLockSince, MOTOR_MOVE_TIMEOUT_MS)) {
                fault(now, "unlock limit switch not reached");
            }
            break;

        case proto::LOCK_UNLOCKED:
            if (!gDoorClosed.stable) {
                gDoorOpenedSinceUnlock = true;
            } else if (gDoorOpenedSinceUnlock) {
                if (elapsed(now, gDoorClosed.stableSinceMs, RELOCK_AFTER_CLOSE_MS)) startLock(now);
            } else if (elapsed(now, gLockSince, RELOCK_IF_NOT_OPENED_MS)) {
                startLock(now);
            }
            break;

        case proto::LOCK_LOCKING:
            if (!gDoorClosed.stable) {   // door pushed open mid-move: pull the bolt back
                Serial.println("[lock] door opened while locking, retracting");
                startUnlock(now);
                break;
            }
            if (gBackoffUntil) {
                if (gMotorOn && gSwUnlocked.raw) motorStop(now);   // do not push into the end stop
                if ((int32_t)(now - gBackoffUntil) >= 0) {
                    gBackoffUntil = 0;
                    motorRun(motor::Dir::Lock, now);
                    gLockSince = now;
                }
                break;
            }
            if (gMotorOn && gSwLocked.raw) {
                motorStop(now);
                logMove("lock switch reached", now);
            }
            if (!gMotorOn && gSwLocked.stable) {
                setLock(proto::LOCK_LOCKED, now);
                break;
            }
            {
                const bool stall = gMotorOn && motor::stalled(now);
                if (stall || elapsed(now, gLockSince, MOTOR_MOVE_TIMEOUT_MS)) {
                    if (!gRetried) {
                        Serial.printf("[lock] %s, backing off and retrying once\n",
                                      stall ? "motor stalled while locking" : "lock switch not reached");
                        gRetried = true;
                        motorRun(motor::Dir::Unlock, now);
                        gBackoffUntil = now + MOTOR_RETRY_BACKOFF_MS;
                    } else {
                        fault(now, stall ? "motor stalled while locking (no encoder movement)"
                                         : "lock limit switch not reached");
                    }
                }
            }
            break;

        case proto::LOCK_LOCKED:
        case proto::LOCK_FAULT:
        case proto::LOCK_UNKNOWN:
            break;
    }
}

void bootLockState(uint32_t now) {
    const bool l = gSwLocked.stable, u = gSwUnlocked.stable;
    if (l && u) {
        fault(now, "both limit switches pressed at boot");
    } else if (l) {
        setLock(proto::LOCK_LOCKED, now);
    } else if (u) {
        setLock(proto::LOCK_UNLOCKED, now);
        gDoorOpenedSinceUnlock = !gDoorClosed.stable;
    } else if (gDoorClosed.stable) {
        Serial.println("[lock] bolt position unknown at boot, locking");
        startLock(now);
    } else {
        // Door open, position unknown: lock once the door has been closed.
        setLock(proto::LOCK_UNLOCKED, now);
        gDoorOpenedSinceUnlock = true;
    }
}

// --------------------------------------------------------- alarm inputs ---

void updateAlarmInputs(uint32_t now) {
    const bool base = armedBase();
    if (base && !gArmedPrev) {
        gArmedSince = now;
        gStatusDirty = true;
    }
    if (!base && gArmedPrev) gStatusDirty = true;
    gArmedPrev = base;

    // Door forced: reed opens while the bolt is locked. armedBase() needs the
    // door closed, so check the lock state directly here.
    if (!gDisarmed && gLock == proto::LOCK_LOCKED && !gDoorClosed.stable)
        raiseAlarm(proto::ALARM_FORCED_DOOR, now);

    if (BOLT_TAMPER_IS_ALARM && !gDisarmed && gLock == proto::LOCK_LOCKED && !gSwLocked.stable &&
        elapsed(now, gSwLocked.stableSinceMs, BOLT_TAMPER_MS))
        raiseAlarm(proto::ALARM_BOLT_TAMPER, now);

    const bool intrusionArmed =
        base && elapsed(now, gArmedSince, ARM_DELAY_MS) && now >= PIR_WARMUP_MS;
    const intrusion::Trip trip = intrusion::update(now, intrusionArmed);
    if (trip == intrusion::VIBRATION) raiseAlarm(proto::ALARM_INTRUSION, now);
    if (trip == intrusion::MOTION) raiseAlarm(proto::ALARM_MOTION, now);

    if (LINK_LOSS_IS_ALARM && base && elapsed(now, gLastRxMs, INT_LINK_TIMEOUT_MS))
        raiseAlarm(proto::ALARM_TAMPER_LINK, now);

    for (uint32_t n = gLink.takeAuthFailures(); n > 0; n--) {
        gRfFails[gRfFailIdx] = now ? now : 1;
        gRfFailIdx = (gRfFailIdx + 1) % RF_TAMPER_FAILS;
        Serial.println("[link] frame failed authentication");
    }
    uint8_t recent = 0;
    for (uint32_t t : gRfFails) recent += t && !elapsed(now, t, RF_TAMPER_WINDOW_MS);
    if (recent >= RF_TAMPER_FAILS && base) raiseAlarm(proto::ALARM_TAMPER_RF, now);
}

uint32_t gIdentifyUntil = 0;

void updateOutputs(uint32_t now) {
    // Buzzer: siren overrides feedback beeps.
    if (gAlarm != proto::ALARM_NONE && !elapsed(now, gAlarmSince, ALARM_SIREN_MS)) {
        buzz(((now / 300) & 1) ? 1800 : 1000);
    } else if (gBeep.left) {
        if (elapsed(now, gBeep.stepMs, gBeep.on ? gBeep.onMs : gBeep.offMs) || !gBeep.stepMs) {
            gBeep.on = !gBeep.on;
            gBeep.stepMs = now;
            if (!gBeep.on) gBeep.left--;
        }
        buzz(gBeep.on ? gBeep.hz : 0);
    } else {
        buzz(0);
    }

    bool led;
    if (gAlarm != proto::ALARM_NONE) led = (now / 125) & 1;
    else if (gEnrollMode) led = true;
    else if (gLock == proto::LOCK_FAULT) led = (now / 500) & 1;
    else if (gArmedPrev) led = now % 3000 < 60;   // short flash: armed
    else led = false;

    // "identify": fast blink on the red LED and the DevKit's onboard LED (GPIO2).
    const bool ident = (int32_t)(gIdentifyUntil - now) > 0;
    if (ident) led = (now / 100) & 1;
    digitalWrite(PIN_ONBOARD_LED, ident && led);
    digitalWrite(PIN_LED_RED, led);
}

// ------------------------------------------------------------------ auth ---

uint32_t lockoutSeconds(uint32_t fails) {
    if (fails < LOCKOUT_THRESHOLD) return 0;
    const uint32_t shift = min<uint32_t>(fails - LOCKOUT_THRESHOLD, 10);
    return min<uint32_t>(LOCKOUT_BASE_S << shift, LOCKOUT_MAX_S);
}

uint16_t lockoutLeftS(uint32_t now) {
    if ((int32_t)(gLockoutUntil - now) <= 0) return 0;
    return (uint16_t)((gLockoutUntil - now + 999) / 1000);
}

uint8_t attemptsLeft() {
    const uint32_t f = creds::failCount();
    return f >= LOCKOUT_THRESHOLD ? 0 : (uint8_t)(LOCKOUT_THRESHOLD - f);
}

void recordFailure(uint32_t now) {
    const uint32_t f = creds::failCount() + 1;
    creds::setFailCount(f);
    const uint32_t s = lockoutSeconds(f);
    if (s) {
        gLockoutUntil = now + s * 1000;
        Serial.printf("[auth] lockout %lu s after %lu failures\n", (unsigned long)s,
                      (unsigned long)f);
    }
}

void endEnroll() {
    gEnrollMode = false;
    gEnrollPending = false;
    mbedtls_platform_zeroize(gEnrollUid, sizeof(gEnrollUid));
    mbedtls_platform_zeroize(gEnrollPin, sizeof(gEnrollPin));
    gEnrollUidLen = gEnrollPinLen = 0;
    gStatusDirty = true;
}

uint8_t handleEnroll(const proto::AuthRequest& q) {
    if (!gEnrollPending) {
        memcpy(gEnrollUid, q.uid, sizeof(gEnrollUid));
        gEnrollUidLen = q.uidLen;
        memcpy(gEnrollPin, q.pin, sizeof(gEnrollPin));
        gEnrollPinLen = q.pinLen;
        gEnrollPending = true;
        gEnrollSince = millis();
        Serial.println("[enroll] step 1 ok: tap the same card and enter the same PIN again");
        return proto::AUTH_ENROLL_AGAIN;
    }
    const bool same = q.uidLen == gEnrollUidLen && q.pinLen == gEnrollPinLen &&
                      ctEqual(q.uid, gEnrollUid, sizeof(gEnrollUid)) &&
                      ctEqual(q.pin, gEnrollPin, sizeof(gEnrollPin));
    uint8_t code = proto::AUTH_ENROLL_FAIL;
    if (!same) {
        Serial.println("[enroll] card or PIN did not match step 1, cancelled");
    } else {
        uint32_t ms = 0;
        const int slot = creds::enroll(q.uid, q.uidLen, q.pin, q.pinLen, &ms);
        if (slot >= 0) {
            Serial.printf("[enroll] stored in slot %d (PBKDF2 took %lu ms)\n", slot,
                          (unsigned long)ms);
            code = proto::AUTH_ENROLL_OK;
        } else {
            Serial.println("[enroll] storage full or write failed");
        }
    }
    endEnroll();
    return code;
}

bool requestValid(const proto::AuthRequest& q) {
    if (!(q.uidLen == 4 || q.uidLen == 7 || q.uidLen == 10)) return false;
    if (q.pinLen < proto::PIN_MIN || q.pinLen > proto::PIN_MAX) return false;
    for (uint8_t i = 0; i < proto::PIN_MAX; i++) {
        const char c = q.pin[i];
        if (i < q.pinLen ? (c < '0' || c > '9') : c != 0) return false;
    }
    for (uint8_t i = q.uidLen; i < proto::UID_MAX; i++)
        if (q.uid[i]) return false;
    return true;
}

proto::AuthResult handleAuth(const proto::AuthRequest& q, uint32_t now) {
    proto::AuthResult r = {};
    r.reqId = q.reqId;
    if (!requestValid(q)) {
        r.code = proto::AUTH_DENIED;
        r.attemptsLeft = attemptsLeft();
        return r;
    }
    if (lockoutLeftS(now)) {
        r.code = proto::AUTH_LOCKED_OUT;
        r.lockoutS = lockoutLeftS(now);
        return r;
    }
    if (gAuthSeen && !elapsed(now, gLastAuthMs, AUTH_MIN_INTERVAL_MS)) {
        r.code = proto::AUTH_BUSY;
        return r;
    }
    gAuthSeen = true;
    gLastAuthMs = now;

    if (gEnrollMode) {
        r.code = handleEnroll(q);
        return r;
    }

    const creds::Verify v = creds::verify(q.uid, q.uidLen, q.pin, q.pinLen);
    now = millis();   // PBKDF2 takes a while
    if (v == creds::Verify::Ok) {
        creds::setFailCount(0);
        gLockoutUntil = 0;
        clearAlarm("valid card + PIN");
        Serial.println("[auth] GRANTED");
        if (gLock == proto::LOCK_UNLOCKED) {
            gLockSince = now;   // restart the auto-relock timer
        } else if (gLock != proto::LOCK_UNLOCKING) {
            startUnlock(now);
        }
        beep(2000, 80, 0, 1);
        r.code = proto::AUTH_GRANTED;
        r.attemptsLeft = attemptsLeft();
        return r;
    }

    Serial.printf("[auth] DENIED (%s)\n", v == creds::Verify::BadPin ? "wrong PIN" : "unknown card");
    recordFailure(now);
    beep(300, 300, 0, 1);
    if (lockoutLeftS(now)) {
        r.code = proto::AUTH_LOCKED_OUT;
        r.lockoutS = lockoutLeftS(now);
    } else {
        r.code = proto::AUTH_DENIED;
        r.attemptsLeft = attemptsLeft();
    }
    return r;
}

// ------------------------------------------------------------------ link ---

void sendMsg(uint8_t type, const void* payload, size_t len, uint32_t now) {
    uint8_t frame[slink::MAX_FRAME];
    const size_t n = gLink.seal(type, payload, len, frame, sizeof(frame), now);
    if (n) espnow::send(frame, n);
}

void sendStatus(uint32_t now) {
    proto::Status s = {};
    s.lock = gLock;
    s.alarm = gAlarm;
    if (gArmedPrev) s.flags |= proto::STATUS_ARMED;
    if (gDisarmed) s.flags |= proto::STATUS_DISARMED;
    if (gEnrollMode) s.flags |= proto::STATUS_ENROLL;
    if (!gDoorClosed.stable) s.flags |= proto::STATUS_DOOR_OPEN;
    sendMsg(proto::STATUS, &s, sizeof(s), now);
    gLastStatusMs = now;
    gStatusDirty = false;
}

void serviceLink(uint32_t now) {
    espnow::Packet p;
    while (espnow::receive(p)) {
        slink::RxResult r;
        gLink.onFrame(p.data, p.len, now, r);
        if (r.replyLen) espnow::send(r.reply, r.replyLen);
        if (r.kind == slink::RxResult::SessionUp) {
            Serial.println("[link] session established");
            gLastRxMs = now;
        } else if (r.kind == slink::RxResult::Message &&
                   r.payloadLen == proto::payloadSize(r.type)) {
            gLastRxMs = now;
            if (r.type == proto::HEARTBEAT) {
                sendStatus(now);
            } else if (r.type == proto::AUTH_REQUEST) {
                proto::AuthRequest q;
                memcpy(&q, r.payload, sizeof(q));
                const proto::AuthResult res = handleAuth(q, now);
                mbedtls_platform_zeroize(&q, sizeof(q));
                const uint32_t t = millis();
                sendMsg(proto::AUTH_RESULT, &res, sizeof(res), t);
                sendStatus(t);
            }
        }
        mbedtls_platform_zeroize(r.payload, sizeof(r.payload));
    }
    if (gStatusDirty && elapsed(now, gLastStatusMs, 200) && gLink.established(now)) sendStatus(now);
}

// --------------------------------------------------------------- console ---

char gLine[64];
uint8_t gLineLen = 0;

void printStatus(uint32_t now) {
    Serial.printf("lock=%s door=%s swLocked=%d swUnlocked=%d armed=%d disarmed=%d\n",
                  lockName(gLock), gDoorClosed.stable ? "closed" : "OPEN", gSwLocked.stable,
                  gSwUnlocked.stable, gArmedPrev, gDisarmed);
    Serial.printf("motor=%s encoder=%ld vibration=%d motion=%d\n",
                  gMotorOn ? "RUNNING" : "off", (long)motor::count(), intrusion::vibrationActive(),
                  intrusion::motionActive());
    Serial.printf("alarm=%s link=%s lastRx=%lus ago\n", alarmName(gAlarm),
                  gLink.established(now) ? "up" : "down",
                  (unsigned long)((now - gLastRxMs) / 1000));
    Serial.printf("cards=%u/%u fails=%lu lockout=%us enroll=%d\n", creds::count(), MAX_CREDENTIALS,
                  (unsigned long)creds::failCount(), lockoutLeftS(now), gEnrollMode);
}

void runCommand(char* cmd, uint32_t now) {
    char* arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;

    if (!strcmp(cmd, "help")) {
        Serial.println("status | enroll | cancel | list | delete <slot> | wipe yes | identify [s]");
        if (CONSOLE_TEST_COMMANDS)
            Serial.println("test: lock | unlock | arm | disarm | clear | resetlockout");
    } else if (!strcmp(cmd, "status")) {
        printStatus(now);
    } else if (!strcmp(cmd, "identify")) {
        const int s = arg ? constrain(atoi(arg), 1, 300) : 30;
        gIdentifyUntil = now + (uint32_t)s * 1000;
        Serial.printf("[int] INTERIOR board: blinking the onboard blue LED for %d s\n", s);
    } else if (!strcmp(cmd, "enroll")) {
        endEnroll();
        gEnrollMode = true;
        gEnrollSince = now;
        gStatusDirty = true;
        Serial.println("[enroll] tap the new card at the door and enter its PIN (60 s)");
    } else if (!strcmp(cmd, "cancel")) {
        endEnroll();
        Serial.println("[enroll] cancelled");
    } else if (!strcmp(cmd, "list")) {
        for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
            uint8_t uid[proto::UID_MAX], len;
            if (!creds::slotUid(i, uid, len)) continue;
            Serial.printf("  slot %2u  uid ", i);
            for (uint8_t j = 0; j < len; j++) Serial.printf("%02X", uid[j]);
            Serial.println();
        }
        Serial.printf("%u card(s)\n", creds::count());
    } else if (!strcmp(cmd, "delete") && arg) {
        const int slot = atoi(arg);
        Serial.println(slot >= 0 && creds::remove((uint8_t)slot) ? "deleted" : "no such slot");
    } else if (!strcmp(cmd, "wipe")) {
        if (arg && !strcmp(arg, "yes")) {
            creds::wipeAll();
            Serial.println("all cards deleted");
        } else {
            Serial.println("type 'wipe yes' to delete every card");
        }
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "lock")) {
        if (gDoorClosed.stable) startLock(now);
        else Serial.println("door is open");
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "unlock")) {
        startUnlock(now);
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "disarm")) {
        gDisarmed = true;
        clearAlarm("console");
        gStatusDirty = true;
        Serial.println("alarms suppressed");
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "arm")) {
        gDisarmed = false;
        gStatusDirty = true;
        Serial.println("alarms enabled");
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "clear")) {
        clearAlarm("console");
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "resetlockout")) {
        creds::setFailCount(0);
        gLockoutUntil = 0;
        Serial.println("lockout reset");
    } else if (cmd[0]) {
        Serial.println("unknown command, try 'help'");
    }
}

void serviceConsole(uint32_t now) {
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            gLine[gLineLen] = 0;
            runCommand(gLine, now);
            gLineLen = 0;
        } else if (gLineLen < sizeof(gLine) - 1) {
            gLine[gLineLen++] = c;
        }
    }
}

}  // namespace

void setup() {
    // Quiet the buzzer first: GPIO14 toggles during boot (Rev B, accepted).
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    // Motor driver off before anything else: IN1, IN2, ENA all LOW.
    for (uint8_t p : {PIN_MOTOR_IN1, PIN_MOTOR_IN2, PIN_MOTOR_ENA}) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_ONBOARD_LED, OUTPUT);
    digitalWrite(PIN_ONBOARD_LED, LOW);

    Serial.begin(115200);
    delay(50);
    Serial.println("\n[int] anti-theft interior module, L298N + encoder wiring. Type 'help'.");

    motor::begin();
    ledcSetup(BUZZER_LEDC_CH, 2000, 10);
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
    ledcWriteTone(BUZZER_LEDC_CH, 0);

    const uint32_t now = millis();
    gSwLocked.begin(now);
    gSwUnlocked.begin(now);
    gDoorClosed.begin(now);
    intrusion::begin();

    if (!creds::begin()) Serial.println("[creds] NVS open failed");
    Serial.printf("[creds] %u card(s) enrolled\n", creds::count());
    const uint32_t lockS = lockoutSeconds(creds::failCount());
    if (lockS) {
        gLockoutUntil = now + lockS * 1000;   // survives reboot
        Serial.printf("[auth] lockout active after reboot: %lu s\n", (unsigned long)lockS);
    }
    if (creds::count() == 0) Serial.println("[creds] no cards yet: run 'enroll'");

    if (!espnow::begin(ATDS_MAC_INTERIOR, ATDS_MAC_EXTERIOR, ESPNOW_CHANNEL, 0)) {
        Serial.println("[int] ESP-NOW init failed, restarting in 5 s");
        delay(5000);
        ESP.restart();
    }
    gLink.begin(slink::Role::Interior, ATDS_PSK, ATDS_MAC_EXTERIOR, ATDS_MAC_INTERIOR, rng);
    gLastRxMs = millis();

    bootLockState(millis());
    Serial.printf("[int] PIR warm-up: intrusion input ignored for %lu s\n",
                  (unsigned long)(PIR_WARMUP_MS / 1000));
}

void loop() {
    const uint32_t now = millis();
    gSwLocked.update(now);
    gSwUnlocked.update(now);
    gDoorClosed.update(now);

    serviceLink(now);
    motorService(now);
    updateLock(now);
    updateAlarmInputs(now);
    if (gEnrollMode && elapsed(now, gEnrollSince, ENROLL_TIMEOUT_MS)) {
        Serial.println("[enroll] timed out");
        endEnroll();
    }
    updateOutputs(now);
    serviceConsole(now);
    delay(1);
}
