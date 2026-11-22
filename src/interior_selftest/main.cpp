// Interior component kick test (env:interior_selftest). See docs/TESTING.md.
// Replaces the old interior_diag build; its live monitor and servo commands are kept.
//
// At boot it runs the automatic checks, which never move the servo: radio, LED,
// buzzer, switch states, potentiometer, GPIO32 idle level. Then type:
//   switches    press SW1, SW2, then open/close the door (reed)
//   pot         turn the potentiometer end to end
//   intrusion   trigger the SW-420, then the PIR, one at a time (answers Q7)
//   servo yes   MOVES THE BOLT: unlock, then lock, timed against the limit switches
//   s <us>      hold the servo at a pulse width (calibration, Q3)    r  release servo
//   b           beep           monitor   live view of every input (any key stops)
//   auto | all (auto + switches + pot + intrusion) | summary | help
// Any input stops an interactive test early.
#include <Arduino.h>

#include "config_interior.h"
#include "selftest_report.h"

namespace {

volatile uint32_t gEdges = 0;
void IRAM_ATTR onEdge() { gEdges = gEdges + 1; }

bool pressed(uint8_t pin) { return digitalRead(pin) == SWITCH_PRESSED_LEVEL; }
const char* lvl(int v) { return v ? "HIGH" : "LOW"; }

void beepHz(uint16_t hz, uint16_t ms) {
    ledcWriteTone(BUZZER_LEDC_CH, hz);
    delay(ms);
    ledcWriteTone(BUZZER_LEDC_CH, 0);
}

void servoUs(uint16_t us) {
    ledcAttachPin(PIN_SERVO, SERVO_LEDC_CH);
    ledcWrite(SERVO_LEDC_CH, (uint32_t)us * 65536UL / 20000UL);
}

void servoRelease() {
    ledcWrite(SERVO_LEDC_CH, 0);
    ledcDetachPin(PIN_SERVO);
    pinMode(PIN_SERVO, OUTPUT);
    digitalWrite(PIN_SERVO, LOW);
}

// ------------------------------------------------------------ automatic ---

void testLed() {
    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_LED_RED, HIGH);
        delay(250);
        digitalWrite(PIN_LED_RED, LOW);
        delay(250);
    }
    st::record("red LED", st::CHECK, "LED on GPIO4 should have blinked 3 times");
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

void testPotIdle() {
    int mn = 4095, mx = 0;
    long sum = 0;
    for (int i = 0; i < 64; i++) {
        const int v = analogRead(PIN_POT);
        mn = min(mn, v);
        mx = max(mx, v);
        sum += v;
        delay(2);
    }
    const int mean = (int)(sum / 64);
    if (mx - mn > 300)
        st::record("potentiometer (idle)", st::WARN, "readings jump %d..%d: GPIO34 is probably floating (pot not connected?)", mn, mx);
    else
        st::record("potentiometer (idle)", st::CHECK, "steady at %d of 4095 (spread %d). A floating pin can also read steady: run 'pot'", mean, mx - mn);
}

void testIntrusionIdle() {
    const uint32_t e0 = gEdges;
    int highs = 0;
    for (int i = 0; i < 200; i++) {
        highs += digitalRead(PIN_INTRUSION);
        delay(10);
    }
    const uint32_t edges = gEdges - e0;
    const char* warm = millis() < PIR_WARMUP_MS ? " (PIR still in its 60 s warm-up)" : "";
    const int idle = highs > 190 ? HIGH : highs < 10 ? LOW : -1;
    if (edges > 20 || idle < 0) {
        st::record("GPIO32 SW-420 + PIR (idle)", st::WARN, "line toggling (%lu edges, HIGH %d%% of 2 s) with nothing happening: floating or a sensor firing%s",
                   (unsigned long)edges, highs / 2, warm);
    } else if (idle == INTRUSION_ACTIVE_LEVEL) {
        st::record("GPIO32 SW-420 + PIR (idle)", st::WARN, "idle level is %s, which is the configured ACTIVE level: a sensor is on, the line floats, or INTRUSION_ACTIVE_LEVEL is wrong (Q7)%s",
                   lvl(idle), warm);
    } else {
        st::record("GPIO32 SW-420 + PIR (idle)", st::CHECK, "quiet at %s, %lu edges in 2 s. Run 'intrusion' to trigger each sensor%s",
                   lvl(idle), (unsigned long)edges, warm);
    }
}

// ---------------------------------------------------------- interactive ---

// Waits until `pin` changes level and changes back (20 ms debounce).
// Returns the transitions seen (0..2); flags any change on `other` meanwhile.
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
    if (t != 2) {
        st::record("reed switch (door)", st::FAIL, "GPIO27 %s: check reed to GND and GPIO27, and the magnet gap",
                   t ? "changed but never came back" : "never changed");
    } else if (closedLevel != DOOR_CLOSED_LEVEL) {
        st::record("reed switch (door)", st::WARN, "works, but door closed reads %s while DOOR_CLOSED_LEVEL is %s (Q6)",
                   lvl(closedLevel), lvl(DOOR_CLOSED_LEVEL));
    } else {
        st::record("reed switch (door)", st::PASS, "opens and closes; door closed reads %s as configured", lvl(closedLevel));
    }
}

void testPot() {
    Serial.println("\n>> Turn the potentiometer fully one way, then fully the other way (20 s)");
    int mn = 4095, mx = 0, last = -1000;
    const uint32_t t0 = millis();
    while (millis() - t0 < 20000 && !(mn < 200 && mx > 3900)) {
        const int v = analogRead(PIN_POT);
        mn = min(mn, v);
        mx = max(mx, v);
        if (abs(v - last) > 150) {
            Serial.printf("  %4d   (min %d, max %d)\n", v, mn, mx);
            last = v;
        }
        if (st::abortRequested()) break;
        delay(20);
    }
    if (mn < 300 && mx > 3800)
        st::record("potentiometer (range)", st::PASS, "full range %d..%d", mn, mx);
    else if (mx - mn < 200)
        st::record("potentiometer (range)", st::FAIL, "reading stuck around %d: check pot VCC 3V3, GND and wiper to GPIO34", (mn + mx) / 2);
    else
        st::record("potentiometer (range)", st::WARN, "only %d..%d: turn it all the way, or check the pot's end connections", mn, mx);
}

struct Activity {
    uint32_t edges;
    uint32_t longestActiveMs;
    int idle;
};

Activity watchIntrusion(uint32_t ms) {
    Activity a = {0, 0, digitalRead(PIN_INTRUSION)};
    const uint32_t e0 = gEdges, t0 = millis();
    uint32_t activeSince = 0, seenEdges = 0;
    bool active = false;
    while (millis() - t0 < ms) {
        const bool now = digitalRead(PIN_INTRUSION) != a.idle;
        if (now && !active) activeSince = millis();
        if (!now && active) a.longestActiveMs = max<uint32_t>(a.longestActiveMs, millis() - activeSince);
        active = now;
        if (gEdges - e0 != seenEdges) {
            seenEdges = gEdges - e0;
            Serial.printf("  edge %lu, line %s\n", (unsigned long)seenEdges, lvl(digitalRead(PIN_INTRUSION)));
        }
        if (st::abortRequested()) break;
        delay(2);
    }
    if (active) a.longestActiveMs = max<uint32_t>(a.longestActiveMs, millis() - activeSince);
    a.edges = gEdges - e0;
    return a;
}

void testIntrusion() {
    Serial.println("\n>> Cover the PIR (or face it at a wall). Tap the SW-420 board a few times (15 s)");
    const Activity v = watchIntrusion(15000);
    if (v.edges >= 2)
        st::record("SW-420 vibration (GPIO32)", st::PASS, "%lu edges from taps; idle %s, active %s", (unsigned long)v.edges, lvl(v.idle), lvl(!v.idle));
    else
        st::record("SW-420 vibration (GPIO32)", st::FAIL, "no activity: SW-420 not wired, its sensitivity screw too low, or its output is held by the PIR (tied outputs, Q7)");

    if (millis() < PIR_WARMUP_MS) {
        const uint32_t wait = PIR_WARMUP_MS - millis();
        Serial.printf("\n>> PIR warm-up: waiting %lu s. Keep still. Type anything to skip.\n", (unsigned long)(wait / 1000 + 1));
        const uint32_t t0 = millis();
        while (millis() - t0 < wait && !st::abortRequested()) delay(50);
    }
    Serial.println("\n>> Keep the SW-420 still. Uncover the PIR and wave a hand in front of it (15 s)");
    const Activity p = watchIntrusion(15000);
    if (p.edges >= 1 && p.longestActiveMs >= 300)
        st::record("PIR motion (GPIO32)", st::PASS, "pulse of %lu ms; idle %s, active %s", (unsigned long)p.longestActiveMs, lvl(p.idle), lvl(!p.idle));
    else if (p.edges >= 1)
        st::record("PIR motion (GPIO32)", st::WARN, "only short blips (%lu ms max): a PIR pulse is normally 1 s or more", (unsigned long)p.longestActiveMs);
    else
        st::record("PIR motion (GPIO32)", st::FAIL, "no activity: PIR not powered from VIN (5 V), not wired, or held by the SW-420 output (Q7)");

    const int idle = p.edges ? p.idle : v.idle;
    const int active = idle ? LOW : HIGH;
    if ((v.edges || p.edges) && active != INTRUSION_ACTIVE_LEVEL)
        st::record("GPIO32 active level", st::WARN, "sensors drive the line %s, but INTRUSION_ACTIVE_LEVEL is %s: change it (Q7)", lvl(!idle), lvl(INTRUSION_ACTIVE_LEVEL));
    else if (v.edges || p.edges)
        st::record("GPIO32 active level", st::PASS, "sensors drive the line %s, matches INTRUSION_ACTIVE_LEVEL", lvl(!idle));
}

uint32_t driveUntil(uint16_t us, uint8_t pin, uint32_t timeoutMs) {
    servoUs(us);
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (pressed(pin)) return millis() - t0;
        delay(2);
    }
    return 0;
}

void testServo() {
    Serial.println("\n>> SERVO: unlock, then lock (moves the bolt)");
    const uint32_t limit = SERVO_MOVE_TIMEOUT_MS * 2;
    const uint32_t tu = driveUntil(SERVO_UNLOCK_US, PIN_SW_UNLOCKED, limit);
    delay(300);
    const uint32_t tl = driveUntil(SERVO_LOCK_US, PIN_SW_LOCKED, limit);
    delay(300);
    servoRelease();
    if (tu && tl) {
        const bool slow = tu > SERVO_MOVE_TIMEOUT_MS || tl > SERVO_MOVE_TIMEOUT_MS;
        st::record("servo + limit switches", slow ? st::WARN : st::PASS, "unlock %lu ms, lock %lu ms (firmware allows %lu ms)%s",
                   (unsigned long)tu, (unsigned long)tl, (unsigned long)SERVO_MOVE_TIMEOUT_MS,
                   slow ? ": too slow, the main firmware would report FAULT" : "");
    } else if (!tu && !tl) {
        st::record("servo + limit switches", st::FAIL, "neither switch reached in %lu ms: check servo 5 V power, signal on GPIO18, pulse widths (Q3), limit switches", (unsigned long)limit);
    } else {
        st::record("servo + limit switches", st::FAIL, "%s switch never reached: adjust %s (Q3) or the switch position",
                   tu ? "locked (SW1)" : "unlocked (SW2)", tu ? "SERVO_LOCK_US" : "SERVO_UNLOCK_US");
    }
}

void monitor() {
    Serial.println("live monitor, type anything to stop");
    uint32_t seen = gEdges;
    while (!st::abortRequested()) {
        Serial.printf("SW1/locked %-8s SW2/unlocked %-8s REED %s  GPIO32 %s  POT %4d  edges %lu%s\n",
                      pressed(PIN_SW_LOCKED) ? "PRESSED" : "-", pressed(PIN_SW_UNLOCKED) ? "PRESSED" : "-",
                      digitalRead(PIN_REED) == DOOR_CLOSED_LEVEL ? "closed" : "OPEN  ",
                      lvl(digitalRead(PIN_INTRUSION)), analogRead(PIN_POT), (unsigned long)gEdges,
                      gEdges != seen ? "  <- edge" : "");
        seen = gEdges;
        delay(250);
    }
}

void runAuto() {
    Serial.println("\n---- automatic checks (servo is not moved) ----");
    st::testRadio();
    testLed();
    testBuzzer();
    testSwitchesIdle();
    testPotIdle();
    testIntrusionIdle();
    st::record("L293D", st::PASS, "EN (GPIO23) and IN2 (GPIO19) held LOW, driver off");
    st::summary();
}

void help() {
    Serial.println("commands: switches | pot | intrusion | servo yes | s <us> | r | b | monitor | auto | all | summary | help");
}

}  // namespace

void setup() {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    for (uint8_t p : {PIN_L293D_EN, PIN_L293D_IN2, PIN_SERVO, PIN_LED_RED}) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    pinMode(PIN_SW_LOCKED, INPUT_PULLUP);
    pinMode(PIN_SW_UNLOCKED, INPUT_PULLUP);
    pinMode(PIN_REED, INPUT_PULLUP);
    pinMode(PIN_INTRUSION, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_INTRUSION), onEdge, CHANGE);
    analogSetPinAttenuation(PIN_POT, ADC_11db);
    ledcSetup(SERVO_LEDC_CH, 50, 16);
    ledcSetup(BUZZER_LEDC_CH, 2000, 10);
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
    ledcWriteTone(BUZZER_LEDC_CH, 0);

    Serial.begin(115200);
    delay(50);
    Serial.println("\n[kick] INTERIOR component kick test. Hardware Rev B pin map, servo assumed on GPIO18.");
    runAuto();
    help();
}

void loop() {
    static char line[32];
    if (st::readLine(line, sizeof(line))) {
        if (!strcmp(line, "switches")) testSwitches();
        else if (!strcmp(line, "pot")) testPot();
        else if (!strcmp(line, "intrusion")) testIntrusion();
        else if (!strcmp(line, "servo yes")) testServo();
        else if (!strcmp(line, "servo")) { Serial.println("this moves the bolt: type 'servo yes'"); return; }
        else if (line[0] == 's' && line[1] == ' ') {
            const int us = atoi(line + 2);
            if (us >= 500 && us <= 2500) { servoUs(us); Serial.printf("servo held at %d us ('r' releases)\n", us); }
            else Serial.println("pulse must be 500..2500 us");
            return;
        }
        else if (!strcmp(line, "r")) { servoRelease(); Serial.println("servo released"); return; }
        else if (!strcmp(line, "b")) { beepHz(2000, 150); return; }
        else if (!strcmp(line, "monitor")) { monitor(); return; }
        else if (!strcmp(line, "auto")) { runAuto(); return; }
        else if (!strcmp(line, "all")) { runAuto(); testSwitches(); testPot(); testIntrusion(); }
        else if (!strcmp(line, "summary")) {}
        else { help(); return; }
        st::summary();
    }
    delay(5);
}
