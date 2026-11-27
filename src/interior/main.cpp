// Interior module: checks card + shared PIN, drives the bolt motor (L298N +
// encoder) in turns, watches the door, limit switches, vibration and motion
// sensors, runs the alarm, keeps the tamper-evident event log, and runs the
// USB serial console. See docs/DESIGN.md sections 5 and 6.2.
//
// Bolt sequence (positions in motor turns from "locked", see config_interior.h):
//   card + PIN  -> UNLOCKING 0 -> 3 turns -> HOLDING (latch held open)
//   door opens  -> RELEASING 3 -> 2 turns -> UNLOCKED (no automatic relock)
//   card + 'A'  -> LOCKING   2 -> 0 turns -> LOCKED
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#include "config_common.h"
#include "config_interior.h"
#include "credentials.h"
#include "espnow_transport.h"
#include "evlog.h"
#include "intrusion.h"
#include "mbedtls/platform_util.h"
#include "motor.h"
#include "protocol.h"
#include "rtc.h"
#include "secrets_guard.h"
#include "secure_link.h"
#include "web.h"

// ATDS_SIM_BOLT (env:interior_sim): no motor at all, a virtual encoder.
// ATDS_BENCH (env:interior_bench): real motor, door from the console, no end
// switches required. Neither build goes on a door.
#if defined(ATDS_SIM_BOLT) || defined(ATDS_BENCH)
#define ATDS_SIM_DOOR
#endif

namespace {

#ifdef ATDS_SIM_BOLT
constexpr bool SIM_BOLT = true;
#else
constexpr bool SIM_BOLT = false;
#endif
#ifdef ATDS_SIM_DOOR
constexpr bool SIM_DOOR = true;
#else
constexpr bool SIM_DOOR = false;
#endif
constexpr bool END_SWITCHES = REQUIRE_END_SWITCHES && !SIM_DOOR;

constexpr int32_t P_LOCKED = 0;
constexpr int32_t P_UNLOCKED = UNLOCK_TURNS * COUNTS_PER_TURN;
constexpr int32_t P_LATCH = (UNLOCK_TURNS + LATCH_TURNS) * COUNTS_PER_TURN;

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
Debounced gSwLatch = {PIN_SW_UNLOCKED, SWITCH_PRESSED_LEVEL, SWITCH_DEBOUNCE_MS};   // SW2: latch held
Debounced gDoorClosed = {PIN_REED, DOOR_CLOSED_LEVEL, REED_DEBOUNCE_MS};

#ifdef ATDS_SIM_DOOR
bool gSimDoorOpen = false;
bool gSensorsOn = false;
#else
constexpr bool gSensorsOn = true;
#endif

// Simulated builds: the console sets the door instead of the reed switch.
void simDoor(uint32_t now) {
#ifdef ATDS_SIM_DOOR
    const bool closed = !gSimDoorOpen;
    if (gDoorClosed.stable != closed) {
        gDoorClosed.stable = gDoorClosed.raw = closed;
        gDoorClosed.stableSinceMs = now;
    }
#else
    (void)now;
#endif
}

// ---------------------------------------------------------------- motor ---

bool gMotorOn = false;    // driving a move
bool gHoldOn = false;     // holding the latch open
uint32_t gBrakeUntil = 0;
uint32_t gMoveStartMs = 0;
int32_t gMoveStartCount = 0;

#ifdef ATDS_SIM_BOLT
int32_t gSimCount = 0;
uint32_t gSimLastMs = 0;
int8_t gSimDir = 0;
#endif

int32_t posCount() {
#ifdef ATDS_SIM_BOLT
    return gSimCount;
#else
    return motor::count();
#endif
}

void motorRun(motor::Dir d, uint32_t now) {
    gMotorOn = true;
    gHoldOn = false;
    gBrakeUntil = 0;
    gMoveStartMs = now;
    gMoveStartCount = posCount();
#ifdef ATDS_SIM_BOLT
    gSimDir = d == motor::Dir::Unlock ? ENC_UNLOCK_SIGN : -ENC_UNLOCK_SIGN;
    gSimLastMs = now;
#else
    motor::drive(d, MOTOR_DUTY_PCT);
#endif
}

int8_t gHoldDrive = 0;   // hold: -1 pushing to lock, 0 braking, +1 pushing to unlock

void motorHold() {
    gMotorOn = false;
    gHoldOn = true;
    gBrakeUntil = 0;
    gHoldDrive = 2;   // force the first update to apply
}

// Holds the shaft at `target` (counts from locked): brakes inside the band,
// pushes back at HOLD_DUTY_PCT when it drifts out (spring, or nothing to push on).
void holdService(int32_t target, int32_t r) {
    if (!gHoldOn) return;
    const int32_t err = target - r;
    const int8_t want = err > HOLD_BAND_COUNTS ? 1 : err < -HOLD_BAND_COUNTS ? -1 : (abs(err) < HOLD_BAND_COUNTS / 3 ? 0 : gHoldDrive);
    if (want == gHoldDrive) return;
    gHoldDrive = want;
#ifndef ATDS_SIM_BOLT
    if (want == 0) motor::brake();
    else motor::drive(want > 0 ? motor::Dir::Unlock : motor::Dir::Lock, HOLD_DUTY_PCT);
#endif
}

void motorStop(uint32_t now) {
    if (!gMotorOn && !gHoldOn) return;
    gMotorOn = gHoldOn = false;
#ifdef ATDS_SIM_BOLT
    gSimDir = 0;
    (void)now;
#else
    motor::brake();
    gBrakeUntil = (now + MOTOR_BRAKE_MS) | 1;
#endif
}

void motorService(uint32_t now) {
#ifdef ATDS_SIM_BOLT
    if (gSimDir) {
        const uint32_t dt = now - gSimLastMs;
        gSimCount += gSimDir * (int32_t)((int64_t)dt * COUNTS_PER_TURN / TURN_TIME_MS);
        gSimLastMs = now;
    }
#else
    if (gBrakeUntil && (int32_t)(now - gBrakeUntil) >= 0) {
        motor::coast();
        gBrakeUntil = 0;
    }
#endif
}

bool moveStalled(uint32_t now) {
#ifdef ATDS_SIM_BOLT
    (void)now;
    return false;
#else
    return gMotorOn && motor::stalled(now);
#endif
}

int32_t gHome = 0;   // encoder count at "locked"
int32_t gLead = MOVE_STOP_LEAD_COUNTS;   // braking point before the target, learned from each stop
int32_t rel() { return (posCount() - gHome) * ENC_UNLOCK_SIGN; }

void logMove(const char* what, uint32_t now) {
    Serial.printf("[lock] %s in %lu ms, encoder %+ld counts, position %.2f turns\n", what,
                  (unsigned long)(now - gMoveStartMs), (long)(posCount() - gMoveStartCount),
                  (double)rel() / COUNTS_PER_TURN);
}

// One move to an absolute position, ended by the turn count or an end switch.
struct Move {
    bool active;
    int32_t finalTarget;  // where the whole move ends
    uint32_t pauseUntil;  // between single turns
    int32_t target;       // end of the current single turn
    bool up;              // towards unlock
    uint32_t timeoutMs;
    Debounced* sw;        // switch at the target, or nullptr
    uint32_t settleUntil; // after stopping: wait, then check the switch
    bool slow;            // in the slow zone near the target
    bool swStop;          // ended on the end switch (no lead learning)
    const char* fail;
} gMove = {};

// Next single-turn stop from `from` towards `final`: the next whole-turn
// position (so an off-position start finishes the partial turn first).
int32_t nextStep(int32_t from, int32_t final) {
    const int32_t T = COUNTS_PER_TURN;
    int32_t next;
    if (final > from) {
        next = (from + HOLD_BAND_COUNTS) / T * T + T;   // floor + 1 turn
        if (from < 0) next = (from + HOLD_BAND_COUNTS - T + 1) / T * T + T;
        return min(next, final);
    }
    next = (from - HOLD_BAND_COUNTS) >= 0 ? (from - HOLD_BAND_COUNTS + T - 1) / T * T - T   // ceil - 1 turn
                                          : (from - HOLD_BAND_COUNTS) / T * T - T;
    return max(next, final);
}

void beginStep(uint32_t now) {
    const int32_t from = rel();
    gMove.target = nextStep(from, gMove.finalTarget);
    gMove.up = gMove.target > from;
    const uint32_t dist = (uint32_t)abs(gMove.target - from);
    gMove.timeoutMs = (uint32_t)((uint64_t)dist * TURN_TIME_MS * 2 / COUNTS_PER_TURN) + 1500;
    gMove.slow = false;
    gMove.swStop = false;
    gMove.settleUntil = 0;
    if (dist <= (uint32_t)HOLD_BAND_COUNTS) {
        gMove.target = gMove.finalTarget;
        gMove.settleUntil = now | 1;
        return;
    }
    Serial.printf("[lock] turn %.2f -> %.2f\n", (double)from / COUNTS_PER_TURN, (double)gMove.target / COUNTS_PER_TURN);
    motorRun(gMove.up ? motor::Dir::Unlock : motor::Dir::Lock, now);
}

void startMove(int32_t target, Debounced* sw, uint32_t now) {
    gMove = {};
    gMove.active = true;
    gMove.finalTarget = target;
    gMove.sw = SIM_BOLT ? nullptr : sw;
    Serial.printf("[lock] move %.2f -> %.2f turns, one turn at a time\n", (double)rel() / COUNTS_PER_TURN,
                  (double)target / COUNTS_PER_TURN);
    beginStep(now);
}

// 0 = running, 1 = done, -1 = failed (gMove.fail says why).
int updateMove(uint32_t now) {
    if (!gMove.active) return 1;
    if (gMove.pauseUntil) {
        if ((int32_t)(now - gMove.pauseUntil) < 0) return 0;
        gMove.pauseUntil = 0;
        beginStep(now);
        return 0;
    }
    if (gMove.settleUntil) {
        if ((int32_t)(now - gMove.settleUntil) < 0) return 0;
        gMove.active = false;
        {
            // Learn the coast: positive = stopped past the target.
            const int32_t over = gMove.up ? rel() - gMove.target : gMove.target - rel();
            if (!gMove.swStop) gLead = constrain(gLead + over / 2, 0, COUNTS_PER_TURN / 2);
            Serial.printf("[lock] stopped at %.2f turns (target %.2f, next braking lead %ld counts)\n",
                          (double)rel() / COUNTS_PER_TURN, (double)gMove.target / COUNTS_PER_TURN, (long)gLead);
        }
        if (!gMove.swStop && gMove.target != gMove.finalTarget) {
            gMove.active = true;   // more turns to go: short stop, then the next one
            gMove.settleUntil = 0;
            gMove.pauseUntil = (now + TURN_PAUSE_MS) | 1;
            return 0;
        }
        if (END_SWITCHES && gMove.sw && !gMove.sw->stable) {
            gMove.fail = gMove.sw == &gSwLocked ? "turns done but SW1 (locked) not pressed"
                                                : "turns done but SW2 (latch held) not pressed";
            return -1;
        }
        return 1;
    }
    const int32_t r = rel();
    const bool swHit = gMove.sw && gMove.sw->raw;
    const bool reached = gMove.up ? r >= gMove.target - gLead : r <= gMove.target + gLead;
    if (swHit || reached) {
        motorStop(now);
        logMove(swHit ? "end switch reached" : "turns done", now);
        if (swHit && gMove.sw == &gSwLocked) gHome = posCount();   // re-zero at the locked switch
        gMove.swStop = swHit;
        gMove.settleUntil = (now + MOTOR_BRAKE_MS + 60) | 1;
        return 0;
    }
    if (!gMove.slow && abs(gMove.target - r) <= SLOW_ZONE_COUNTS) {
        gMove.slow = true;
#ifndef ATDS_SIM_BOLT
        motor::drive(gMove.up ? motor::Dir::Unlock : motor::Dir::Lock, SLOW_DUTY_PCT);
#endif
    }
    if (moveStalled(now)) {
        gMove.fail = "motor stalled (no encoder movement)";
        gMove.active = false;
        return -1;
    }
    if (elapsed(now, gMoveStartMs, gMove.timeoutMs)) {
        gMove.fail = "move timed out";
        gMove.active = false;
        return -1;
    }
    return 0;
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
uint32_t gHoldSince = 0;
bool gThenUnlock = false;   // finish locking/seeking, then unlock
bool gThenLock = false;     // finish unlocking/releasing, then lock

proto::AlarmCause gAlarm = proto::ALARM_NONE;
uint32_t gAlarmSince = 0;
bool gDisarmed = false;
bool gArmedPrev = false;
uint32_t gArmedSince = 0;
bool gDoorWasClosed = true;

uint32_t gRfFails[RF_TAMPER_FAILS] = {};
uint8_t gRfFailIdx = 0;

uint32_t gLockoutUntil = 0;
uint32_t gLastAuthMs = 0;
bool gAuthSeen = false;

enum class Admin : uint8_t { None, Enroll, SetPin };
Admin gAdmin = Admin::None;
uint32_t gAdminSince = 0;
bool gAdminPending = false;
uint8_t gAdminUid[proto::UID_MAX];
uint8_t gAdminUidLen = 0;
char gAdminPin[proto::PIN_MAX];
uint8_t gAdminPinLen = 0;

// PIN typed on the dashboard: used once by the next card + '#' with no digits.
constexpr uint32_t PHONE_PIN_TTL_MS = 60000;
char gPhonePin[proto::PIN_MAX];
uint8_t gPhonePinLen = 0;
uint32_t gPhonePinUntil = 0;

bool gStatusDirtyFwd = false;
bool phonePinActive(uint32_t now) { return gPhonePinLen && (int32_t)(gPhonePinUntil - now) > 0; }
void clearPhonePin() {
    mbedtls_platform_zeroize(gPhonePin, sizeof(gPhonePin));
    if (gPhonePinLen) gStatusDirtyFwd = true;
    gPhonePinLen = 0;
}

const char* lockName(uint8_t s) {
    static const char* names[] = {"UNKNOWN", "LOCKED", "UNLOCKING", "UNLOCKED",
                                  "LOCKING", "FAULT",  "HOLDING",   "RELEASING"};
    return s < 8 ? names[s] : "?";
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

void fault(uint32_t now, const char* why) {
    Serial.printf("[lock] FAULT: %s (encoder %+ld counts this move, position %.2f turns)\n", why,
                  (long)(posCount() - gMoveStartCount), (double)rel() / COUNTS_PER_TURN);
    motorStop(now);
    gMove.active = false;
    gThenUnlock = gThenLock = false;
    setLock(proto::LOCK_FAULT, now);
    evlog::append(evlog::EV_FAULT, evlog::NO_SLOT, (uint8_t)gLock, (uint32_t)(posCount() - gMoveStartCount),
                  (uint32_t)rel());
    beep(400, 150, 100, 3);
}

void raiseAlarm(proto::AlarmCause cause, uint32_t now) {
    if (gDisarmed || gAlarm != proto::ALARM_NONE) return;
    gAlarm = cause;
    gAlarmSince = now;
    gStatusDirty = true;
    Serial.printf("[alarm] RAISED: %s\n", alarmName(cause));
    evlog::append(evlog::EV_ALARM, evlog::NO_SLOT, cause);
}

void clearAlarm(const char* by) {
    if (gAlarm == proto::ALARM_NONE) return;
    Serial.printf("[alarm] cleared by %s\n", by);
    evlog::append(evlog::EV_ALARM_CLEARED, evlog::NO_SLOT, gAlarm);
    gAlarm = proto::ALARM_NONE;
    gStatusDirty = true;
}

bool armedBase() { return !gDisarmed && gLock == proto::LOCK_LOCKED && gDoorClosed.stable; }

// --------------------------------------------------------- lock machine ---

// Position unknown (boot without SW1): drive towards locked until SW1 closes.
void startSeek(uint32_t now) {
    Serial.println("[lock] position unknown: driving towards locked until SW1 closes");
    gHome = posCount();
    startMove(-(int32_t)(UNLOCK_TURNS + LATCH_TURNS + SEEK_EXTRA_TURNS) * COUNTS_PER_TURN, &gSwLocked, now);
    setLock(proto::LOCK_LOCKING, now);
}

void startUnlock(uint32_t now) {
    startMove(P_LATCH, &gSwLatch, now);
    setLock(proto::LOCK_UNLOCKING, now);
}

void startLock(uint32_t now) {
    if (gLock == proto::LOCK_UNKNOWN) {
        startSeek(now);
        return;
    }
    startMove(P_LOCKED, &gSwLocked, now);
    setLock(proto::LOCK_LOCKING, now);
}

void startRelease(bool doorOpened, uint32_t now) {
    Serial.printf("[lock] releasing the latch (%s)\n", doorOpened ? "door opened" : "hold timed out");
    evlog::append(evlog::EV_LATCH_RELEASED, evlog::NO_SLOT, doorOpened ? 1 : 0);
    motorStop(now);
    if (RELEASE_BY_SPRING) {
        setLock(proto::LOCK_UNLOCKED, now);
        return;
    }
    startMove(P_UNLOCKED, nullptr, now);
    setLock(proto::LOCK_RELEASING, now);
}

// Card + PIN accepted (or console "unlock").
void requestUnlock(uint32_t now) {
    switch (gLock) {
        case proto::LOCK_LOCKED:
        case proto::LOCK_UNLOCKED:
        case proto::LOCK_FAULT:
            startUnlock(now);
            break;
        case proto::LOCK_UNKNOWN:
            gThenUnlock = true;
            startSeek(now);
            break;
        case proto::LOCK_HOLDING:
            gHoldSince = now;   // keep holding
            break;
        case proto::LOCK_LOCKING:
            gThenUnlock = true;
            break;
        default:
            break;   // already moving towards open
    }
}

void updateLock(uint32_t now) {
    int r;
    switch (gLock) {
        case proto::LOCK_UNLOCKING:
            r = updateMove(now);
            if (r < 0) {
                fault(now, gMove.fail);
            } else if (r > 0 && gThenLock) {
                gThenLock = false;
                evlog::append(evlog::EV_UNLOCKED);
                startLock(now);
            } else if (r > 0) {
                motorHold();
                gHoldSince = now;
                setLock(proto::LOCK_HOLDING, now);
                evlog::append(evlog::EV_UNLOCKED);
                Serial.println("[lock] latch held open: push the door");
            }
            break;

        case proto::LOCK_HOLDING:
            holdService(P_LATCH, rel());
            if (!gDoorClosed.stable && elapsed(now, gDoorClosed.stableSinceMs, DOOR_OPEN_CONFIRM_MS))
                startRelease(true, now);
            else if (elapsed(now, gHoldSince, HOLD_MAX_MS))
                startRelease(false, now);
            break;

        case proto::LOCK_RELEASING:
            r = updateMove(now);
            if (r < 0) {
                fault(now, gMove.fail);
            } else if (r > 0) {
                setLock(proto::LOCK_UNLOCKED, now);
                if (gThenLock) {
                    gThenLock = false;
                    startLock(now);
                }
            }
            break;

        case proto::LOCK_LOCKING:
            if (!gDoorClosed.stable) {   // door pushed open mid-move: pull the bolt back
                Serial.println("[lock] door opened while locking, retracting");
                motorStop(now);
                gThenUnlock = false;
                startMove(P_UNLOCKED, nullptr, now);
                setLock(proto::LOCK_RELEASING, now);
                break;
            }
            r = updateMove(now);
            if (r < 0) {
                fault(now, gMove.fail);
            } else if (r > 0) {
                setLock(proto::LOCK_LOCKED, now);
                evlog::append(evlog::EV_LOCKED);
                if (gThenUnlock) {
                    gThenUnlock = false;
                    startUnlock(now);
                }
            }
            break;

        case proto::LOCK_LOCKED:
        case proto::LOCK_UNLOCKED:
        case proto::LOCK_FAULT:
        case proto::LOCK_UNKNOWN:
            break;
    }
}

void bootLockState(uint32_t now) {
    const bool l = gSwLocked.stable, u = gSwLatch.stable;
    if (SIM_DOOR) {
        gHome = posCount();   // test builds start locked
        setLock(proto::LOCK_LOCKED, now);
    } else if (l && u) {
        fault(now, "both limit switches pressed at boot");
    } else if (l) {
        gHome = posCount();
        setLock(proto::LOCK_LOCKED, now);
    } else {
        // No automatic movement: the first card + PIN or card + A finds SW1 first.
        Serial.println("[lock] bolt position unknown (SW1 not pressed): waiting for card + PIN or card + A");
        setLock(proto::LOCK_UNKNOWN, now);
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

    if (gDoorClosed.stable != gDoorWasClosed) {
        gDoorWasClosed = gDoorClosed.stable;
        gStatusDirty = true;
        evlog::append(gDoorWasClosed ? evlog::EV_DOOR_CLOSED : evlog::EV_DOOR_OPEN, evlog::NO_SLOT, gLock);
    }

    // Door forced: reed opens while the bolt is locked.
    if (!gDisarmed && gLock == proto::LOCK_LOCKED && !gDoorClosed.stable)
        raiseAlarm(proto::ALARM_FORCED_DOOR, now);

    if (BOLT_TAMPER_IS_ALARM && !SIM_DOOR && !gDisarmed && gLock == proto::LOCK_LOCKED && !gSwLocked.stable &&
        elapsed(now, gSwLocked.stableSinceMs, BOLT_TAMPER_MS))
        raiseAlarm(proto::ALARM_BOLT_TAMPER, now);

    const bool intrusionArmed =
        gSensorsOn && base && elapsed(now, gArmedSince, ARM_DELAY_MS) && now >= PIR_WARMUP_MS;
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
    else if (gAdmin != Admin::None) led = true;
    else if (gLock == proto::LOCK_FAULT) led = (now / 500) & 1;
    else if (gArmedPrev) led = now % 3000 < 60;   // short flash: armed
    else led = false;

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
        Serial.printf("[auth] lockout %lu s after %lu failures\n", (unsigned long)s, (unsigned long)f);
        evlog::append(evlog::EV_LOCKOUT, evlog::NO_SLOT, 0, s, f);
    }
}

void endAdmin() {
    gAdmin = Admin::None;
    gAdminPending = false;
    mbedtls_platform_zeroize(gAdminUid, sizeof(gAdminUid));
    mbedtls_platform_zeroize(gAdminPin, sizeof(gAdminPin));
    gAdminUidLen = gAdminPinLen = 0;
    gStatusDirty = true;
}

// Console-started enrollment or PIN change, answered at the door.
//   enroll, PIN already set: one step, card + the shared PIN adds the card.
//   enroll, no PIN yet:      two steps (card + new PIN, twice) set the PIN and add the card.
//   setpin:                  two steps with an enrolled card set a new shared PIN.
uint8_t handleAdmin(const proto::AuthRequest& q) {
    if (gAdmin == Admin::Enroll && creds::pinSet()) {
        const uint32_t t0 = millis();
        const bool ok = creds::checkPin(q.pin, q.pinLen);
        const uint32_t ms = millis() - t0;
        uint8_t code = proto::AUTH_ENROLL_FAIL;
        if (!ok) {
            Serial.println("[enroll] wrong shared PIN: card not added");
        } else {
            const int slot = creds::addCard(q.uid, q.uidLen);
            if (slot >= 0) {
                Serial.printf("[enroll] stored in slot %d (PBKDF2 took %lu ms)\n", slot, (unsigned long)ms);
                evlog::append(evlog::EV_CARD_ADDED, (uint8_t)slot);
                code = proto::AUTH_ENROLL_OK;
            } else {
                Serial.println("[enroll] card storage full or write failed");
            }
        }
        endAdmin();
        return code;
    }
    if (gAdmin == Admin::SetPin && !gAdminPending && creds::findCard(q.uid, q.uidLen) < 0) {
        Serial.println("[setpin] that card is not enrolled: PIN not changed");
        endAdmin();
        return proto::AUTH_ENROLL_FAIL;
    }
    const char* who = gAdmin == Admin::SetPin ? "[setpin]" : "[enroll]";
    if (!gAdminPending) {
        memcpy(gAdminUid, q.uid, sizeof(gAdminUid));
        gAdminUidLen = q.uidLen;
        memcpy(gAdminPin, q.pin, sizeof(gAdminPin));
        gAdminPinLen = q.pinLen;
        gAdminPending = true;
        gAdminSince = millis();
        Serial.printf("%s step 1 ok: tap the same card and enter the same PIN again\n", who);
        return proto::AUTH_ENROLL_AGAIN;
    }
    const bool same = q.uidLen == gAdminUidLen && q.pinLen == gAdminPinLen &&
                      ctEqual(q.uid, gAdminUid, sizeof(gAdminUid)) && ctEqual(q.pin, gAdminPin, sizeof(gAdminPin));
    uint8_t code = proto::AUTH_ENROLL_FAIL;
    if (!same) {
        Serial.printf("%s card or PIN did not match step 1, cancelled\n", who);
    } else {
        uint32_t ms = 0;
        if (!creds::setPin(q.pin, q.pinLen, &ms)) {
            Serial.printf("%s PIN write failed\n", who);
        } else {
            evlog::append(evlog::EV_PIN_CHANGED);
            if (gAdmin == Admin::SetPin) {
                Serial.printf("[setpin] shared PIN changed (PBKDF2 took %lu ms)\n", (unsigned long)ms);
                code = proto::AUTH_PIN_SET;
            } else {
                const int slot = creds::addCard(q.uid, q.uidLen);
                if (slot >= 0) {
                    Serial.printf("[enroll] shared PIN set; stored in slot %d (PBKDF2 took %lu ms)\n", slot,
                                  (unsigned long)ms);
                    evlog::append(evlog::EV_CARD_ADDED, (uint8_t)slot);
                    code = proto::AUTH_ENROLL_OK;
                } else {
                    Serial.println("[enroll] PIN set, but card storage full");
                }
            }
        }
    }
    endAdmin();
    return code;
}

bool uidValid(uint8_t uidLen, const uint8_t* uid) {
    if (!(uidLen == 4 || uidLen == 7 || uidLen == 10)) return false;
    for (uint8_t i = uidLen; i < proto::UID_MAX; i++)
        if (uid[i]) return false;
    return true;
}

bool requestValid(const proto::AuthRequest& q) {
    if (!uidValid(q.uidLen, q.uid)) return false;
    // pinLen 0 = "use the PIN entered on the dashboard" (card + '#' with no digits).
    if (q.pinLen != 0 && (q.pinLen < proto::PIN_MIN || q.pinLen > proto::PIN_MAX)) return false;
    for (uint8_t i = 0; i < proto::PIN_MAX; i++) {
        const char c = q.pin[i];
        if (i < q.pinLen ? (c < '0' || c > '9') : c != 0) return false;
    }
    return true;
}

// Shared gate for unlock and lock requests: lockout and rate limit.
bool gate(proto::AuthResult& r, uint32_t now) {
    if (lockoutLeftS(now)) {
        r.code = proto::AUTH_LOCKED_OUT;
        r.lockoutS = lockoutLeftS(now);
        return false;
    }
    if (gAuthSeen && !elapsed(now, gLastAuthMs, AUTH_MIN_INTERVAL_MS)) {
        r.code = proto::AUTH_BUSY;
        return false;
    }
    gAuthSeen = true;
    gLastAuthMs = now;
    return true;
}

void denyResult(proto::AuthResult& r, uint32_t now) {
    if (lockoutLeftS(now)) {
        r.code = proto::AUTH_LOCKED_OUT;
        r.lockoutS = lockoutLeftS(now);
    } else {
        r.code = proto::AUTH_DENIED;
        r.attemptsLeft = attemptsLeft();
    }
}

proto::AuthResult handleAuth(const proto::AuthRequest& q, uint32_t now) {
    proto::AuthResult r = {};
    r.reqId = q.reqId;
    if (!requestValid(q)) {
        Serial.printf("[auth] DENIED (malformed request: uid_len=%u pin_len=%u)\n", q.uidLen, q.pinLen);
        r.code = proto::AUTH_DENIED;
        r.attemptsLeft = attemptsLeft();
        return r;
    }
    if (!gate(r, now)) return r;
    const bool phone = q.pinLen == 0;
    if (gAdmin != Admin::None) {
        if (phone) {
            Serial.println("[enroll] type the PIN on the keypad: the phone PIN is not used for enrollment");
            endAdmin();
            r.code = proto::AUTH_ENROLL_FAIL;
            return r;
        }
        r.code = handleAdmin(q);
        return r;
    }
    char pin[proto::PIN_MAX];
    uint8_t pinLen = q.pinLen;
    memcpy(pin, q.pin, sizeof(pin));
    if (phone) {
        if (!phonePinActive(now)) {
            Serial.println("[auth] DENIED (no phone PIN waiting)");
            evlog::append(evlog::EV_DENIED_NO_PIN, evlog::NO_SLOT, 1);
            r.code = proto::AUTH_DENIED;
            r.attemptsLeft = attemptsLeft();
            return r;
        }
        memcpy(pin, gPhonePin, sizeof(pin));
        pinLen = gPhonePinLen;
        clearPhonePin();   // one use only, right or wrong
    }

    int slot = -1;
    const creds::Verify v = creds::verify(q.uid, q.uidLen, pin, pinLen, &slot);
    mbedtls_platform_zeroize(pin, sizeof(pin));
    now = millis();   // PBKDF2 takes a while
    if (v == creds::Verify::Ok) {
        creds::setFailCount(0);
        gLockoutUntil = 0;
        clearAlarm("valid card + PIN");
        Serial.printf("[auth] GRANTED (card %d%s)\n", slot, phone ? ", phone PIN" : "");
        evlog::append(evlog::EV_GRANTED, (uint8_t)slot, phone ? 1 : 0);
        requestUnlock(now);
        beep(2000, 80, 0, 1);
        r.code = proto::AUTH_GRANTED;
        r.attemptsLeft = attemptsLeft();
        return r;
    }
    if (v == creds::Verify::NoPin) {
        Serial.println("[auth] DENIED (no shared PIN set yet: run 'enroll')");
        evlog::append(evlog::EV_DENIED_NO_PIN);
        r.code = proto::AUTH_DENIED;
        r.attemptsLeft = attemptsLeft();
        return r;
    }
    const bool badPin = v == creds::Verify::BadPin;
    Serial.printf("[auth] DENIED (%s%s)\n", badPin ? "wrong PIN" : "unknown card", phone ? ", phone PIN" : "");
    evlog::append(badPin ? evlog::EV_DENIED_PIN : evlog::EV_DENIED_CARD, badPin ? (uint8_t)slot : evlog::NO_SLOT,
                  phone ? 1 : 0);
    recordFailure(now);
    beep(300, 300, 0, 1);
    denyResult(r, now);
    return r;
}

// Lock from any state (door must already be checked closed).
void doLock(uint32_t now) {
    if (gLock == proto::LOCK_UNLOCKING || gLock == proto::LOCK_RELEASING) {
        gThenLock = true;   // finish the current move, then lock
    } else if (gLock == proto::LOCK_LOCKING) {
        gThenUnlock = false;
    } else if (gLock != proto::LOCK_LOCKED) {
        motorStop(now);     // also lets go of a held latch
        startLock(now);
    }
}

// Card + 'A' at the door. No PIN: only enrolled cards may lock.
proto::AuthResult handleLock(const proto::LockRequest& q, uint32_t now) {
    proto::AuthResult r = {};
    r.reqId = q.reqId;
    bool zero = true;
    for (uint8_t b : q.reserved) zero &= b == 0;
    if (!uidValid(q.uidLen, q.uid) || !zero) {
        Serial.println("[lock] request DENIED (malformed)");
        r.code = proto::AUTH_DENIED;
        r.attemptsLeft = attemptsLeft();
        return r;
    }
    if (!gate(r, now)) return r;
    const int slot = creds::findCard(q.uid, q.uidLen);
    if (slot < 0) {
        // Counted like a wrong PIN, so lock requests cannot be used to probe card IDs.
        Serial.println("[lock] request DENIED (unknown card)");
        evlog::append(evlog::EV_LOCK_DENIED_CARD);
        recordFailure(now);
        beep(300, 300, 0, 1);
        denyResult(r, now);
        return r;
    }
    // Locking is allowed in every state once the door is closed. A move in
    // progress finishes first, a held latch is let go and the bolt thrown.
    uint8_t refuse = 0;
    if (!gDoorClosed.stable) refuse = proto::REFUSE_DOOR_OPEN;
    evlog::append(evlog::EV_LOCK_REQUEST, (uint8_t)slot, refuse ? 0 : 1, refuse);
    if (refuse) {
        Serial.printf("[lock] request from card %d refused (%s)\n", slot,
                      refuse == proto::REFUSE_DOOR_OPEN ? "door open" : "bolt moving");
        r.code = proto::AUTH_LOCK_REFUSED;
        r.attemptsLeft = refuse;
        return r;
    }
    Serial.printf("[lock] request from card %d accepted\n", slot);
    doLock(now);
    beep(2000, 80, 0, 1);
    r.code = proto::AUTH_LOCKING;
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
    if (gAdmin != Admin::None) s.flags |= proto::STATUS_ENROLL;
    if (!gDoorClosed.stable) s.flags |= proto::STATUS_DOOR_OPEN;
    if (phonePinActive(now)) s.flags |= proto::STATUS_PHONE_PIN;
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
        } else if (r.kind == slink::RxResult::Message && r.payloadLen == proto::payloadSize(r.type)) {
            gLastRxMs = now;
            if (r.type == proto::HEARTBEAT) {
                // Someone is typing at the door (exterior PinEntry=2 / Verifying=3):
                // keep enroll / setpin mode from timing out.
                proto::Heartbeat hb;
                memcpy(&hb, r.payload, sizeof(hb));
                if (gAdmin != Admin::None && (hb.extState == 2 || hb.extState == 3)) gAdminSince = now;
                sendStatus(now);
            } else if (r.type == proto::AUTH_REQUEST || r.type == proto::LOCK_REQUEST) {
                proto::AuthResult res;
                if (r.type == proto::AUTH_REQUEST) {
                    proto::AuthRequest q;
                    memcpy(&q, r.payload, sizeof(q));
                    res = handleAuth(q, now);
                    mbedtls_platform_zeroize(&q, sizeof(q));
                } else {
                    proto::LockRequest q;
                    memcpy(&q, r.payload, sizeof(q));
                    res = handleLock(q, now);
                }
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

void printTime(uint32_t t, uint8_t st) {
    if (st == evlog::RTC_NONE) {
        Serial.print("no RTC fitted");
        return;
    }
    if (st == evlog::RTC_ERROR) {
        Serial.print("RTC read error");
        return;
    }
    int y, mo, d, h, mi, s;
    rtc::toCivil(t, &y, &mo, &d, &h, &mi, &s);
    Serial.printf("%04d-%02d-%02d %02d:%02d:%02d UTC%s", y, mo, d, h, mi, s,
                  st == evlog::RTC_LOST_POWER ? " (RTC LOST POWER: set the time)" : "");
}

void printStatus(uint32_t now) {
    Serial.printf("lock=%s position=%.2f turns door=%s SW1(locked)=%d SW2(latch)=%d armed=%d disarmed=%d\n",
                  lockName(gLock), (double)rel() / COUNTS_PER_TURN, gDoorClosed.stable ? "closed" : "OPEN",
                  gSwLocked.stable, gSwLatch.stable, gArmedPrev, gDisarmed);
    Serial.printf("motor=%s encoder=%ld vibration=%d motion=%d\n",
                  gMotorOn ? "RUNNING" : gHoldOn ? "HOLDING" : "off", (long)posCount(),
                  intrusion::vibrationActive(), intrusion::motionActive());
    Serial.printf("alarm=%s link=%s lastRx=%lus ago\n", alarmName(gAlarm), gLink.established(now) ? "up" : "down",
                  (unsigned long)((now - gLastRxMs) / 1000));
    Serial.printf("cards=%u/%u sharedPIN=%s fails=%lu lockout=%us admin=%s\n", creds::count(), MAX_CREDENTIALS,
                  creds::pinSet() ? "set" : "NOT SET", (unsigned long)creds::failCount(), lockoutLeftS(now),
                  gAdmin == Admin::Enroll ? "enroll" : gAdmin == Admin::SetPin ? "setpin" : "none");
    uint8_t st;
    const uint32_t t = evlog::now(&st);
    Serial.printf("log=%lu records (head seq %lu) time=", (unsigned long)evlog::count(),
                  (unsigned long)evlog::headSeq());
    printTime(t, st);
    Serial.println();
}

void startAdmin(Admin a, uint32_t now) {
    endAdmin();
    gAdmin = a;
    gAdminSince = now;
    gStatusDirty = true;
}

void runCommand(char* cmd, uint32_t now) {
    char* arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;

    if (!strcmp(cmd, "help")) {
        Serial.println("status | enroll | setpin | cancel | list | delete <slot> | wipe yes | identify [s]");
        Serial.println("wifi | wifi pass <new> | webpass <new>");
        Serial.println("time | time set YYYY-MM-DDTHH:MM:SS (UTC) | log show [n] | log verify | log dump");
        if (CONSOLE_TEST_COMMANDS) Serial.println("test: lock | unlock | arm | disarm | clear | resetlockout");
    } else if (!strcmp(cmd, "status")) {
        printStatus(now);
    } else if (!strcmp(cmd, "wifi")) {
        if (arg && !strncmp(arg, "pass ", 5)) Serial.println(web::setApPass(arg + 5) ? "WiFi password saved: reboot to apply" : "8-32 characters");
        else web::printCredentials();
    } else if (!strcmp(cmd, "webpass") && arg) {
        Serial.println(web::setAdminPass(arg) ? "dashboard password changed, all devices logged out" : "8-64 characters");
    } else if (!strcmp(cmd, "identify")) {
        const int s = arg ? constrain(atoi(arg), 1, 300) : 30;
        gIdentifyUntil = now + (uint32_t)s * 1000;
        Serial.printf("[int] INTERIOR board: blinking the onboard blue LED for %d s\n", s);
    } else if (!strcmp(cmd, "enroll")) {
        startAdmin(Admin::Enroll, now);
        if (creds::pinSet())
            Serial.println("[enroll] tap the new card at the door and enter the shared PIN (120 s)");
        else
            Serial.println("[enroll] no shared PIN yet: tap a card and choose the PIN, then repeat both (120 s)");
    } else if (!strcmp(cmd, "setpin")) {
        if (!creds::count()) {
            Serial.println("[setpin] no cards enrolled: use 'enroll' (the first enroll sets the PIN)");
        } else {
            startAdmin(Admin::SetPin, now);
            Serial.println("[setpin] tap an enrolled card and type the NEW PIN, then repeat both (120 s)");
        }
    } else if (!strcmp(cmd, "cancel")) {
        endAdmin();
        Serial.println("[enroll] cancelled");
    } else if (!strcmp(cmd, "list")) {
        for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
            uint8_t uid[proto::UID_MAX], len;
            if (!creds::slotUid(i, uid, len)) continue;
            Serial.printf("  slot %2u  uid ", i);
            for (uint8_t j = 0; j < len; j++) Serial.printf("%02X", uid[j]);
            Serial.println();
        }
        Serial.printf("%u card(s), shared PIN %s\n", creds::count(), creds::pinSet() ? "set" : "NOT SET");
    } else if (!strcmp(cmd, "delete") && arg) {
        const int slot = atoi(arg);
        if (slot >= 0 && creds::remove((uint8_t)slot)) {
            evlog::append(evlog::EV_CARD_REMOVED, (uint8_t)slot);
            Serial.println("deleted");
        } else {
            Serial.println("no such slot");
        }
    } else if (!strcmp(cmd, "wipe")) {
        if (arg && !strcmp(arg, "yes")) {
            creds::wipeAll();
            evlog::append(evlog::EV_WIPE);
            Serial.println("all cards deleted and shared PIN cleared");
        } else {
            Serial.println("type 'wipe yes' to delete every card and the shared PIN");
        }
    } else if (!strcmp(cmd, "time")) {
        if (arg && !strncmp(arg, "set ", 4)) {
            uint32_t t;
            if (!rtc::parseIso(arg + 4, &t)) Serial.println("format: time set 2026-11-21T15:04:05 (UTC)");
            else if (!evlog::setTime(t)) Serial.println("no DS3231 found: time not set");
            else Serial.println("time set (logged)");
        } else {
            uint8_t st;
            const uint32_t t = evlog::now(&st);
            printTime(t, st);
            Serial.println();
        }
    } else if (!strcmp(cmd, "log")) {
        if (arg && !strcmp(arg, "verify")) {
            uint32_t n, bad, back;
            const bool ok = evlog::verify(&n, &bad, &back);
            if (ok) Serial.printf("[log] chain OK: %lu record(s) verified", (unsigned long)n);
            else Serial.printf("[log] CHAIN BROKEN at seq %lu after %lu good record(s)", (unsigned long)bad, (unsigned long)n);
            Serial.printf(", %lu record(s) with time going backwards\n", (unsigned long)back);
        } else if (arg && !strcmp(arg, "dump")) {
            evlog::dump();
        } else {
            const int n = arg && !strncmp(arg, "show", 4) && arg[4] == ' ' ? atoi(arg + 5) : 20;
            evlog::show(n > 0 ? (uint32_t)n : 20);
        }
#ifdef ATDS_SIM_DOOR
    } else if (!strcmp(cmd, "door") && arg && (!strcmp(arg, "open") || !strcmp(arg, "close"))) {
        gSimDoorOpen = !strcmp(arg, "open");
        Serial.printf("[sim] door %s\n", gSimDoorOpen ? "OPEN" : "closed");
    } else if (!strcmp(cmd, "sensors") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
        gSensorsOn = !strcmp(arg, "on");
        Serial.printf("[sim] SW-420 and PIR %s\n", gSensorsOn ? "ON" : "ignored");
#endif
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "lock")) {
        if (!gDoorClosed.stable) Serial.println("door is open");
        else if (gLock == proto::LOCK_LOCKED) Serial.println("already locked");
        else if (gLock == proto::LOCK_UNLOCKING || gLock == proto::LOCK_RELEASING) gThenLock = true;
        else if (gLock != proto::LOCK_LOCKING) {
            motorStop(now);
            startLock(now);
        }
    } else if (CONSOLE_TEST_COMMANDS && !strcmp(cmd, "unlock")) {
        requestUnlock(now);
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
        evlog::append(evlog::EV_LOCKOUT_RESET);
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
        } else if (gLineLen < sizeof(gLine) - 1 && c >= 0x20 && c <= 0x7E) {
            gLine[gLineLen++] = c;
        }
    }
}

}  // namespace

void setup() {
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
#ifdef ATDS_SIM_BOLT
    Serial.println("[int] TEST BUILD: SIMULATED BOLT, no motor. Console: door open|close, sensors on|off. Do not fit to a door.");
#endif
#ifdef ATDS_BENCH
    Serial.println("[int] BENCH BUILD: REAL MOTOR, door from console, end switches optional. Console: door open|close, sensors on|off. Do not fit to a door.");
#endif

    motor::begin();
    ledcSetup(BUZZER_LEDC_CH, 2000, 10);
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
    ledcWriteTone(BUZZER_LEDC_CH, 0);

    const uint32_t now = millis();
    gSwLocked.begin(now);
    gSwLatch.begin(now);
    gDoorClosed.begin(now);
    intrusion::begin();

    if (!creds::begin()) Serial.println("[creds] NVS open failed");
    Serial.printf("[creds] %u card(s) enrolled, shared PIN %s\n", creds::count(), creds::pinSet() ? "set" : "NOT SET");
    if (!creds::pinSet()) Serial.println("[creds] run 'enroll' to set the shared PIN and add the first card");

    Serial.println(rtc::begin(PIN_RTC_SDA, PIN_RTC_SCL) ? "[rtc] DS3231 found" : "[rtc] no DS3231: log uses boot count + uptime");
    evlog::begin(ATDS_PSK);

    const uint32_t lockS = lockoutSeconds(creds::failCount());
    if (lockS) {
        gLockoutUntil = now + lockS * 1000;   // survives reboot
        Serial.printf("[auth] lockout active after reboot: %lu s\n", (unsigned long)lockS);
    }

    web::loadCredentials();
    if (!espnow::begin(ATDS_MAC_INTERIOR, ATDS_MAC_EXTERIOR, ESPNOW_CHANNEL, 0, web::apSsid(), web::apPass())) {
        Serial.println("[int] ESP-NOW init failed, restarting in 5 s");
        delay(5000);
        ESP.restart();
    }
    gLink.begin(slink::Role::Interior, ATDS_PSK, ATDS_MAC_EXTERIOR, ATDS_MAC_INTERIOR, rng);
    web::begin();
    gLastRxMs = millis();

    simDoor(millis());
    gDoorWasClosed = gDoorClosed.stable;
    bootLockState(millis());
    Serial.printf("[int] PIR warm-up: intrusion input ignored for %lu s\n", (unsigned long)(PIR_WARMUP_MS / 1000));
}

void loop() {
    uint32_t now = millis();
    gSwLocked.update(now);
    gSwLatch.update(now);
    if (!SIM_DOOR) gDoorClosed.update(now);   // test builds: door comes from the console
    simDoor(now);

    serviceLink(now);
    // serviceLink can take ~0.5 s (PIN hash) and may have started the motor:
    // later steps must not see a time from before that.
    now = millis();
    motorService(now);
    updateLock(now);
    updateAlarmInputs(now);
    if (gAdmin != Admin::None && elapsed(now, gAdminSince, ENROLL_TIMEOUT_MS)) {
        Serial.println("[enroll] timed out: next card is treated as a normal login");
        endAdmin();
    }
    evlog::service(now);
    if (gPhonePinLen && !phonePinActive(now)) {
        Serial.println("[phone] PIN expired unused");
        clearPhonePin();
    }
    if (gStatusDirtyFwd) {
        gStatusDirty = true;
        gStatusDirtyFwd = false;
    }
    web::service(gMotorOn);   // no web work while the motor is driving
    updateOutputs(now);
    serviceConsole(now);
    delay(1);
}

// ------------------------------------------------------ dashboard (web) ---
// Called from web.cpp inside loop(), so they run in the same task as the lock
// logic. Nothing here can unlock: unlocking always needs the card.
namespace app {

static const char* buildName() {
#if defined(ATDS_SIM_BOLT)
    return "interior_sim (simulated bolt)";
#elif defined(ATDS_BENCH)
    return "interior_bench (real motor, console door)";
#else
    return "interior (production)";
#endif
}

void statusJson(String& out) {
    const uint32_t now = millis();
    uint8_t st;
    const uint32_t t = evlog::now(&st);
    char b[760];
    snprintf(b, sizeof(b),
             "{\"lock\":\"%s\",\"position\":%.2f,\"door\":\"%s\",\"sw1\":%d,\"sw2\":%d,\"armed\":%d,\"disarmed\":%d,"
             "\"motor\":\"%s\",\"alarm\":\"%s\",\"link\":%s,\"lastRx\":%lu,\"cards\":%u,\"maxCards\":%u,\"pinSet\":%s,"
             "\"fails\":%lu,\"lockout\":%u,\"adminMode\":\"%s\",\"phonePin\":%lu,\"logCount\":%lu,\"logHead\":%lu,"
             "\"time\":%lu,\"rtc\":%u,\"uptime\":%lu,\"vibration\":%d,\"motion\":%d,\"build\":\"%s\"}",
             lockName(gLock), (double)rel() / COUNTS_PER_TURN, gDoorClosed.stable ? "closed" : "open", gSwLocked.stable,
             gSwLatch.stable, gArmedPrev, gDisarmed, gMotorOn ? "running" : gHoldOn ? "holding" : "off",
             alarmName(gAlarm), gLink.established(now) ? "true" : "false", (unsigned long)((now - gLastRxMs) / 1000),
             creds::count(), MAX_CREDENTIALS, creds::pinSet() ? "true" : "false", (unsigned long)creds::failCount(),
             lockoutLeftS(now), gAdmin == Admin::Enroll ? "enroll" : gAdmin == Admin::SetPin ? "setpin" : "none",
             (unsigned long)(phonePinActive(now) ? (gPhonePinUntil - now + 999) / 1000 : 0),
             (unsigned long)evlog::count(), (unsigned long)evlog::headSeq(), (unsigned long)t, st,
             (unsigned long)(now / 1000), intrusion::vibrationActive(), intrusion::motionActive(), buildName());
    out = b;
}

// For the phone-PIN page: no admin login, so nothing sensitive.
void publicJson(String& out) {
    const uint32_t now = millis();
    char b[160];
    snprintf(b, sizeof(b), "{\"link\":%s,\"lock\":\"%s\",\"phonePin\":%lu,\"lockout\":%u}",
             gLink.established(now) ? "true" : "false", lockName(gLock),
             (unsigned long)(phonePinActive(now) ? (gPhonePinUntil - now + 999) / 1000 : 0), lockoutLeftS(now));
    out = b;
}

void cardsJson(String& out) {
    out = "{\"cards\":[";
    bool first = true;
    for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) {
        uint8_t uid[proto::UID_MAX], len;
        if (!creds::slotUid(i, uid, len)) continue;
        char hex[2 * proto::UID_MAX + 1];
        for (uint8_t j = 0; j < len; j++) snprintf(hex + 2 * j, 3, "%02X", uid[j]);
        out += String(first ? "" : ",") + "{\"slot\":" + i + ",\"uid\":\"" + hex + "\"}";
        first = false;
    }
    out += "]}";
}

bool lock(String& msg) {
    const uint32_t now = millis();
    if (!gDoorClosed.stable) {
        msg = "close the door first";
        return false;
    }
    if (gLock == proto::LOCK_LOCKED) {
        msg = "already locked";
        return true;
    }
    doLock(now);
    msg = "locking";
    return true;
}

bool clearAlarm(String& msg) {
    if (gAlarm == proto::ALARM_NONE) {
        msg = "no alarm";
        return true;
    }
    ::clearAlarm("dashboard");
    msg = "alarm cleared";
    return true;
}

bool resetLockout(String& msg) {
    creds::setFailCount(0);
    gLockoutUntil = 0;
    evlog::append(evlog::EV_LOCKOUT_RESET);
    msg = "lockout reset";
    return true;
}

bool startEnroll(String& msg) {
    startAdmin(Admin::Enroll, millis());
    msg = creds::pinSet() ? "tap the new card at the door and type the shared PIN, then #"
                          : "no shared PIN yet: tap a card and choose the PIN, then repeat both";
    return true;
}

bool startSetPin(String& msg) {
    if (!creds::count()) {
        msg = "enroll a card first";
        return false;
    }
    startAdmin(Admin::SetPin, millis());
    msg = "tap an enrolled card and type the new PIN, then #, then repeat both";
    return true;
}

bool cancelAdmin(String& msg) {
    endAdmin();
    msg = "cancelled";
    return true;
}

bool deleteCard(int slot, String& msg) {
    if (slot < 0 || !creds::remove((uint8_t)slot)) {
        msg = "no such slot";
        return false;
    }
    evlog::append(evlog::EV_CARD_REMOVED, (uint8_t)slot);
    msg = "card removed";
    return true;
}

bool setPhonePin(const char* pin, String& msg) {
    const size_t n = strlen(pin);
    bool ok = n >= proto::PIN_MIN && n <= proto::PIN_MAX;
    for (size_t i = 0; ok && i < n; i++) ok = pin[i] >= '0' && pin[i] <= '9';
    if (!ok) {
        msg = "PIN must be 4-8 digits";
        return false;
    }
    const uint32_t now = millis();
    if (lockoutLeftS(now)) {
        msg = "locked out, try again in " + String(lockoutLeftS(now)) + " s";
        return false;
    }
    clearPhonePin();
    memcpy(gPhonePin, pin, n);
    gPhonePinLen = (uint8_t)n;
    gPhonePinUntil = now + PHONE_PIN_TTL_MS;
    gStatusDirty = true;
    evlog::append(evlog::EV_PHONE_PIN);
    Serial.println("[phone] PIN received from the dashboard, waiting for card + # (60 s)");
    msg = "PIN ready: tap your card at the door and press # within 60 s";
    return true;
}

bool setTime(uint32_t unixS, String& msg) {
    if (!evlog::setTime(unixS)) {
        msg = "no DS3231 clock fitted";
        return false;
    }
    msg = "clock set (logged)";
    return true;
}

}  // namespace app
