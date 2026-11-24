// Interior component kick test (env:interior_selftest). See docs/TESTING.md.
// Wiring: proposed updated list (L298N channel A, JGB37-520 motor + encoder,
// SW-420 on GPIO32, PIR on GPIO34, no potentiometer).
//
// At boot it runs the automatic checks, which never move the motor: radio, LED,
// buzzer, switch states, encoder idle lines, SW-420 and PIR idle levels. Then type:
//   switches     press SW1, SW2, then open/close the door (reed)
//   encoder      turn the motor shaft by hand: counts must change
//   vibration    tap the SW-420           motion   wave at the PIR
//   jog u <ms>   MOVES THE MOTOR towards unlock for <ms> (max 1000), stops at the switch
//   jog l <ms>   same towards lock
//   motor yes    MOVES THE BOLT: unlock, then lock, timed against the limit switches
//   b  beep      monitor  live view of every input (any key stops)
//   auto | all (auto + switches + encoder + vibration + motion) | summary | help
// Any input stops an interactive test early.
#include <Arduino.h>

#include "config_interior.h"
#include "motor.h"
#include "selftest_report.h"

namespace {

volatile uint32_t gVibEdges = 0, gPirEdges = 0;
void IRAM_ATTR onVib() { gVibEdges = gVibEdges + 1; }
void IRAM_ATTR onPir() { gPirEdges = gPirEdges + 1; }

bool pressed(uint8_t pin) { return digitalRead(pin) == SWITCH_PRESSED_LEVEL; }
const char* lvl(int v) { return v ? "HIGH" : "LOW"; }

void beepHz(uint16_t hz, uint16_t ms) {
    ledcWriteTone(BUZZER_LEDC_CH, hz);
    delay(ms);
    ledcWriteTone(BUZZER_LEDC_CH, 0);
}

void stopMotor() {
    motor::brake();
    delay(MOTOR_BRAKE_MS);
    motor::coast();
}

// ------------------------------------------------------------ automatic ---

void testLed() {
    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_LED_RED, HIGH);
        digitalWrite(PIN_ONBOARD_LED, HIGH);
        delay(250);
        digitalWrite(PIN_LED_RED, LOW);
        digitalWrite(PIN_ONBOARD_LED, LOW);
        delay(250);
    }
    st::record("red LED", st::CHECK, "LED on GPIO4 (and the onboard blue LED) should have blinked 3 times");
}

void testBuzzer() {
    beepHz(1000, 200);
    delay(100);
    beepHz(2000, 200);
    delay(100);
    beepHz(3000, 200);
    st::record("buzzer", st::CHECK, "3 rising beeps should have sounded (GPIO14)");
}

void testSwitchesIdle() {
    const bool l = pressed(PIN_SW_LOCKED), u = pressed(PIN_SW_UNLOCKED);
    const bool closed = digitalRead(PIN_REED) == DOOR_CLOSED_LEVEL;
    if (l && u) {
        st::record("limit switches (idle)", st::FAIL, "SW1 and SW2 both read pressed: the bolt cannot be in both places, check wiring");
    } else {
        st::record("limit switches (idle)", st::CHECK, "SW1/locked (GPIO26) %s, SW2/unlocked (GPIO25) %s. A disconnected switch reads 'released': run 'switches'",
                   l ? "pressed" : "released", u ? "pressed" : "released");
    }
    st::record("reed switch (idle)", st::CHECK, "door reads %s (GPIO27 %s). A disconnected reed reads 'open': run 'switches'",
               closed ? "closed" : "open", lvl(digitalRead(PIN_REED)));
}

// Samples a pin for 2 s. Returns edges seen; sets idle to HIGH/LOW or -1 if mixed.
uint32_t sampleIdle(uint8_t pin, volatile uint32_t& edges, int& idle, int& highPct) {
    const uint32_t e0 = edges;
    int highs = 0;
    for (int i = 0; i < 200; i++) {
        highs += digitalRead(pin);
        delay(10);
    }
    highPct = highs / 2;
    idle = highs > 190 ? HIGH : highs < 10 ? LOW : -1;
    return edges - e0;
}

void testVibrationIdle() {
    int idle, pct;
    const uint32_t e = sampleIdle(PIN_VIBRATION, gVibEdges, idle, pct);
    if (e > 20 || idle < 0)
        st::record("SW-420 (idle)", st::WARN, "GPIO32 toggling (%lu edges, HIGH %d%% of 2 s) while still: floating, or sensitivity screw too high", (unsigned long)e, pct);
    else if (idle == VIBRATION_ACTIVE_LEVEL)
        st::record("SW-420 (idle)", st::WARN, "GPIO32 idles %s, which is the configured ACTIVE level: sensor triggered, floating, or VIBRATION_ACTIVE_LEVEL wrong (Q7)", lvl(idle));
    else
        st::record("SW-420 (idle)", st::CHECK, "GPIO32 quiet at %s. Run 'vibration'", lvl(idle));
}

void testPirIdle() {
    int idle, pct;
    const uint32_t e = sampleIdle(PIN_PIR, gPirEdges, idle, pct);
    const char* warm = millis() < PIR_WARMUP_MS ? " (PIR still in its 60 s warm-up)" : "";
    if (e > 20 || idle < 0)
        st::record("PIR (idle)", st::WARN, "GPIO34 toggling (%lu edges, HIGH %d%%): floating (GPIO34 has no pull resistor) or motion in view%s", (unsigned long)e, pct, warm);
    else if (idle == PIR_ACTIVE_LEVEL)
        st::record("PIR (idle)", st::WARN, "GPIO34 held %s (active): motion in view, re-trigger mode, or still warming up%s", lvl(idle), warm);
    else
        st::record("PIR (idle)", st::CHECK, "GPIO34 quiet at %s. Run 'motion'%s", lvl(idle), warm);
}

void testEncoderIdle() {
    const int32_t c0 = motor::count();
    const uint32_t b0 = motor::badTransitions();
    delay(500);
    const int32_t moved = motor::count() - c0;
    const uint32_t bad = motor::badTransitions() - b0;
    const int a = digitalRead(PIN_ENC_A), b = digitalRead(PIN_ENC_B);
    if (moved || bad)
        st::record("encoder (idle)", st::WARN, "counts change with the motor off (%ld counts, %lu bad steps): floating C1/C2 or noise. Open-collector outputs need pull-ups to 3V3 (Q3)",
                   (long)moved, (unsigned long)bad);
    else
        st::record("encoder (idle)", st::CHECK, "steady, C1 (GPIO33) %s, C2 (GPIO35) %s. A disconnected encoder looks the same: run 'encoder'", lvl(a), lvl(b));
}

// ---------------------------------------------------------- interactive ---

// Waits until `pin` changes level and changes back (20 ms debounce).
int watchToggle(uint8_t pin, uint32_t ms, int other, bool& otherMoved) {
    int stable = digitalRead(pin), cand = stable, trans = 0;
    const int other0 = other >= 0 ? digitalRead(other) : 0;
    uint32_t candSince = millis();
    const uint32_t t0 = millis();
    while (millis() - t0 < ms && trans < 2) {
        const int r = digitalRead(pin);
        if (r != cand) {
            cand = r;
            candSince = millis();
        } else if (cand != stable && millis() - candSince >= 20) {
            stable = cand;
            trans++;
            Serial.printf("  GPIO%u -> %s\n", pin, lvl(stable));
        }
        if (other >= 0 && digitalRead(other) != other0) otherMoved = true;
        if (st::abortRequested()) break;
        delay(2);
    }
    return trans;
}

void testOneSwitch(const char* test, const char* prompt, uint8_t pin, uint8_t other, const char* otherName) {
    Serial.printf("\n>> %s (15 s)\n", prompt);
    bool otherMoved = false;
    const int t = watchToggle(pin, 15000, other, otherMoved);
    if (t == 2)
        st::record(test, st::PASS, "pressed and released, GPIO%u follows", pin);
    else if (otherMoved)
        st::record(test, st::FAIL, "%s changed instead: the two limit switches are swapped (Q5)", otherName);
    else
        st::record(test, st::FAIL, "GPIO%u %s: check switch COM to GND and NO to GPIO%u", pin,
                   t ? "changed but never came back" : "never changed", pin);
}

void testSwitches() {
    testOneSwitch("SW1 locked switch", "Press and release SW1, the bolt-LOCKED switch", PIN_SW_LOCKED, PIN_SW_UNLOCKED, "SW2");
    testOneSwitch("SW2 unlocked switch", "Press and release SW2, the bolt-UNLOCKED switch", PIN_SW_UNLOCKED, PIN_SW_LOCKED, "SW1");

    Serial.println("\n>> Start with the door CLOSED (magnet at the reed). Open the door, then close it again (20 s)");
    const int closedLevel = digitalRead(PIN_REED);
    bool unused = false;
    const int t = watchToggle(PIN_REED, 20000, -1, unused);
    if (t != 2)
        st::record("reed switch (door)", st::FAIL, "GPIO27 %s: check reed to GND and GPIO27, and the magnet gap",
                   t ? "changed but never came back" : "never changed");
    else if (closedLevel != DOOR_CLOSED_LEVEL)
        st::record("reed switch (door)", st::WARN, "works, but door closed reads %s while DOOR_CLOSED_LEVEL is %s (Q6)",
                   lvl(closedLevel), lvl(DOOR_CLOSED_LEVEL));
    else
        st::record("reed switch (door)", st::PASS, "opens and closes; door closed reads %s as configured", lvl(closedLevel));
}

void testEncoder() {
    Serial.println("\n>> Motor is OFF. Turn the motor output shaft (or the bolt) by hand, both ways (15 s)");
    const int32_t c0 = motor::count();
    const uint32_t b0 = motor::badTransitions();
    int32_t mn = 0, mx = 0, last = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 15000) {
        const int32_t d = motor::count() - c0;
        mn = min(mn, d);
        mx = max(mx, d);
        if (abs(d - last) >= 20) {
            Serial.printf("  count %ld\n", (long)d);
            last = d;
        }
        if (mx >= 50 && mn <= -50) break;
        if (st::abortRequested()) break;
        delay(10);
    }
    const uint32_t bad = motor::badTransitions() - b0;
    if (mx >= 50 && mn <= -50)
        st::record("encoder (by hand)", bad > 10 ? st::WARN : st::PASS, "counts went %ld..%ld, both directions decoded%s",
                   (long)mn, (long)mx, bad > 10 ? ", but with invalid steps: noise or missing pull-ups" : "");
    else if (mx || mn)
        st::record("encoder (by hand)", st::WARN, "counts only %ld..%ld: one direction only means one channel (C1 GPIO33 or C2 GPIO35) is dead", (long)mn, (long)mx);
    else
        st::record("encoder (by hand)", st::FAIL, "no counts: check encoder VCC/GND, C1 to GPIO33, C2 to GPIO35, pull-ups if open collector (Q3)");
}

void testVibration() {
    Serial.println("\n>> Tap the SW-420 board a few times (15 s)");
    const uint32_t e0 = gVibEdges, t0 = millis();
    uint32_t seen = 0;
    while (millis() - t0 < 15000 && gVibEdges - e0 < 6) {
        if (gVibEdges - e0 != seen) {
            seen = gVibEdges - e0;
            Serial.printf("  edge %lu, line %s\n", (unsigned long)seen, lvl(digitalRead(PIN_VIBRATION)));
        }
        if (st::abortRequested()) break;
        delay(5);
    }
    if (gVibEdges - e0 >= 2)
        st::record("SW-420 vibration", st::PASS, "%lu edges from taps on GPIO32", (unsigned long)(gVibEdges - e0));
    else
        st::record("SW-420 vibration", st::FAIL, "no activity on GPIO32: check SW-420 VCC 3V3, GND, DO, and its sensitivity screw");
}

void testMotion() {
    if (millis() < PIR_WARMUP_MS) {
        const uint32_t wait = PIR_WARMUP_MS - millis();
        Serial.printf("\n>> PIR warm-up: waiting %lu s. Keep still. Type anything to skip.\n", (unsigned long)(wait / 1000 + 1));
        const uint32_t t0 = millis();
        while (millis() - t0 < wait && !st::abortRequested()) delay(50);
    }
    Serial.println("\n>> Wave a hand in front of the PIR (15 s)");
    const int idle = digitalRead(PIN_PIR);
    uint32_t longest = 0, since = 0;
    bool active = false;
    const uint32_t t0 = millis();
    while (millis() - t0 < 15000) {
        const bool a = digitalRead(PIN_PIR) != idle;
        if (a && !active) {
            since = millis();
            Serial.printf("  GPIO34 -> %s\n", lvl(!idle));
        }
        if (!a && active) longest = max<uint32_t>(longest, millis() - since);
        active = a;
        if (longest >= 500) break;
        if (st::abortRequested()) break;
        delay(5);
    }
    if (active) longest = max<uint32_t>(longest, millis() - since);
    const int act = idle ? LOW : HIGH;
    if (longest >= 300 && act == PIR_ACTIVE_LEVEL)
        st::record("PIR motion", st::PASS, "pulse of %lu+ ms on GPIO34, active %s as configured", (unsigned long)longest, lvl(act));
    else if (longest >= 300)
        st::record("PIR motion", st::WARN, "pulse seen, but active %s while PIR_ACTIVE_LEVEL is %s", lvl(act), lvl(PIR_ACTIVE_LEVEL));
    else if (longest)
        st::record("PIR motion", st::WARN, "only short blips (%lu ms): a PIR pulse is normally 1 s or more", (unsigned long)longest);
    else
        st::record("PIR motion", st::FAIL, "no activity on GPIO34: check PIR VCC (VIN), GND, OUT to GPIO34");
}

struct Move {
    uint32_t ms;       // 0 = switch not reached
    int32_t counts;
    bool stalled;
};

// Drives until `stopPin` is pressed, a stall, or the timeout. Always stops the motor.
Move driveTo(motor::Dir d, uint8_t stopPin, uint32_t timeoutMs) {
    Move m = {0, 0, false};
    const int32_t c0 = motor::count();
    if (pressed(stopPin)) return {1, 0, false};
    motor::drive(d, MOTOR_DUTY_PCT);
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (pressed(stopPin)) {
            m.ms = millis() - t0;
            break;
        }
        if (motor::stalled(millis())) {
            m.stalled = true;
            break;
        }
        delay(1);
    }
    stopMotor();
    m.counts = motor::count() - c0;
    return m;
}

void jog(char which, int ms) {
    ms = constrain(ms, 20, 1000);
    const bool unlock = which == 'u';
    const motor::Dir d = unlock ? motor::Dir::Unlock : motor::Dir::Lock;
    const uint8_t stopPin = unlock ? PIN_SW_UNLOCKED : PIN_SW_LOCKED;
    const Move m = driveTo(d, stopPin, (uint32_t)ms);
    Serial.printf("jog %s %d ms: encoder %+ld counts%s%s\n", unlock ? "unlock" : "lock", ms, (long)m.counts,
                  m.ms ? ", stopped at the limit switch" : "", m.stalled ? ", STALLED (no encoder movement)" : "");
}

void testMotor() {
    Serial.println("\n>> MOTOR: unlock, then lock (moves the bolt)");
    const uint32_t limit = MOTOR_MOVE_TIMEOUT_MS * 2;
    const Move u = driveTo(motor::Dir::Unlock, PIN_SW_UNLOCKED, limit);
    delay(300);
    const Move l = driveTo(motor::Dir::Lock, PIN_SW_LOCKED, limit);
    char note[160];
    snprintf(note, sizeof(note), "unlock %lu ms / %+ld counts, lock %lu ms / %+ld counts (timeout %lu ms)",
             (unsigned long)u.ms, (long)u.counts, (unsigned long)l.ms, (long)l.counts,
             (unsigned long)MOTOR_MOVE_TIMEOUT_MS);
    if (u.ms && l.ms) {
        const bool slow = u.ms > MOTOR_MOVE_TIMEOUT_MS || l.ms > MOTOR_MOVE_TIMEOUT_MS;
        const bool noEnc = u.counts == 0 && l.counts == 0 && u.ms > 1 && l.ms > 1;
        st::record("motor + limit switches", slow || noEnc ? st::WARN : st::PASS, "%s%s%s", note,
                   slow ? ": too slow, raise MOTOR_MOVE_TIMEOUT_MS (Q2)" : "",
                   noEnc ? ": no encoder counts, set ENCODER_STALL_DETECT false or fix the encoder" : "");
    } else if (u.stalled || l.stalled) {
        st::record("motor + limit switches", st::FAIL, "%s: STALLED. Bolt jammed, motor supply missing, or encoder not working (Q3)", note);
    } else if (!u.ms && l.ms) {
        st::record("motor + limit switches", st::FAIL, "%s: unlock never reached SW2. If the bolt moved the wrong way, set MOTOR_INVERT (Q1)", note);
    } else if (u.ms && !l.ms) {
        st::record("motor + limit switches", st::FAIL, "%s: lock never reached SW1. If the bolt moved the wrong way, set MOTOR_INVERT (Q1)", note);
    } else if (abs(u.counts) > 100 && abs(l.counts) > 100) {
        st::record("motor + limit switches", st::FAIL, "%s: motor turned both ways for the whole time limit but no limit switch closed. The bolt must press SW2 at full unlock and SW1 at full lock", note);
    } else {
        st::record("motor + limit switches", st::FAIL, "%s: no switch reached and little movement. Check L298N motor supply, ENA jumper removed, IN1/IN2/ENA wiring (Q4)", note);
    }
}

void monitor() {
    Serial.println("live monitor, type anything to stop");
    uint32_t vib = gVibEdges;
    while (!st::abortRequested()) {
        Serial.printf("SW1/locked %-8s SW2/unlocked %-8s REED %s  SW-420 %s%s  PIR %s  encoder %ld\n",
                      pressed(PIN_SW_LOCKED) ? "PRESSED" : "-", pressed(PIN_SW_UNLOCKED) ? "PRESSED" : "-",
                      digitalRead(PIN_REED) == DOOR_CLOSED_LEVEL ? "closed" : "OPEN  ",
                      lvl(digitalRead(PIN_VIBRATION)), gVibEdges != vib ? "(edge)" : "      ",
                      lvl(digitalRead(PIN_PIR)), (long)motor::count());
        vib = gVibEdges;
        delay(250);
    }
}

void runAuto() {
    Serial.println("\n---- automatic checks (motor is not moved) ----");
    st::testRadio();
    testLed();
    testBuzzer();
    testSwitchesIdle();
    testEncoderIdle();
    testVibrationIdle();
    testPirIdle();
    st::record("L298N outputs", st::PASS, "IN1 (GPIO18), IN2 (GPIO19), ENA (GPIO23) held LOW, motor off");
    st::summary();
}

void help() {
    Serial.println("commands: switches | encoder | vibration | motion | jog u|l <ms> | motor yes | b | monitor | auto | all | summary | help");
}

}  // namespace

void setup() {
    for (uint8_t p : {PIN_MOTOR_IN1, PIN_MOTOR_IN2, PIN_MOTOR_ENA, PIN_BUZZER, PIN_LED_RED, PIN_ONBOARD_LED}) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    motor::begin();
    pinMode(PIN_SW_LOCKED, INPUT_PULLUP);
    pinMode(PIN_SW_UNLOCKED, INPUT_PULLUP);
    pinMode(PIN_REED, INPUT_PULLUP);
    pinMode(PIN_VIBRATION, INPUT);
    pinMode(PIN_PIR, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_VIBRATION), onVib, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR), onPir, CHANGE);
    ledcSetup(BUZZER_LEDC_CH, 2000, 10);
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
    ledcWriteTone(BUZZER_LEDC_CH, 0);

    Serial.begin(115200);
    delay(50);
    Serial.println("\n[kick] INTERIOR component kick test. L298N + encoder wiring.");
    runAuto();
    help();
}

void loop() {
    static char line[32];
    if (st::readLine(line, sizeof(line))) {
        if (!strcmp(line, "switches")) testSwitches();
        else if (!strcmp(line, "encoder")) testEncoder();
        else if (!strcmp(line, "vibration")) testVibration();
        else if (!strcmp(line, "motion")) testMotion();
        else if (!strcmp(line, "motor yes")) testMotor();
        else if (!strcmp(line, "motor")) { Serial.println("this moves the bolt: type 'motor yes'"); return; }
        else if (!strncmp(line, "jog ", 4) && (line[4] == 'u' || line[4] == 'l')) { jog(line[4], atoi(line + 5)); return; }
        else if (!strcmp(line, "b")) { beepHz(2000, 150); return; }
        else if (!strcmp(line, "monitor")) { monitor(); return; }
        else if (!strcmp(line, "auto")) { runAuto(); return; }
        else if (!strcmp(line, "all")) { runAuto(); testSwitches(); testEncoder(); testVibration(); testMotion(); }
        else if (!strcmp(line, "summary")) {}
        else if (!strcmp(line, "help")) { help(); return; }
        else { st::unknownCommand(line); help(); return; }
        st::summary();
    }
    delay(5);
}
