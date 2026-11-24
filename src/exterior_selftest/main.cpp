// Exterior component kick test (env:exterior_selftest). See docs/TESTING.md.
//
// At boot it runs the automatic checks (radio, LEDs, OLED, RC522, shared
// GPIO22, keypad stuck-key scan) and prints a summary. Then type:
//   keys    press all 16 keys (60 s)       card   tap a card (15 s)
//   leds    light each LED again           auto   re-run the automatic checks
//   all     auto + keys + card             summary / help
// Any input stops an interactive test early.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

#include "config_exterior.h"
#include "keypad.h"
#include "oled.h"
#include "rc522.h"
#include "selftest_report.h"

namespace {

Oled gOled;
Rc522 gRfid;
Keypad gKeypad;
bool gOledOk = false;
bool gRfidOk = false;

bool gMarquee = false;   // scrolling OK screen active (see showOkScreen)

void showOled(const char* top, const char* big) {
    gMarquee = false;
    if (!gOledOk) return;
    gOled.clear();
    gOled.textCentered(0, top);
    gOled.textCentered(14, big, 2);
    gOled.commit();
}

// "OLED OK" screen: the phrase scrolls right to left in large text. It runs from
// loop() while the board is idle; any other screen (showOled) stops it.
const char* const MARQUEE = "BLESSED IS GOAT";
constexpr uint32_t MARQUEE_STEP_MS = 30;   // 2 px per step = about 65 px/s
int gMarqueeX = 0;
uint32_t gMarqueeLastMs = 0;

void showOkScreen() {
    if (!gOledOk) return;
    gMarquee = true;
    gMarqueeX = Oled::W;   // start just off the right edge
    gMarqueeLastMs = 0;
}

void marqueeStep() {
    if (!gMarquee || !gOledOk || millis() - gMarqueeLastMs < MARQUEE_STEP_MS) return;
    gMarqueeLastMs = millis();
    gOled.clear();
    gOled.text(gMarqueeX, 17, MARQUEE, 2);   // lower half: the top rows were cut off on the bench panel
    gOled.commit();
    gMarqueeX -= 2;
    if (gMarqueeX < -Oled::textWidth(MARQUEE, 2)) gMarqueeX = Oled::W;   // loop
}

const char* rc522Name(uint8_t v) {
    switch (v) {
        case 0x91: return "MFRC522 v1.0";
        case 0x92: return "MFRC522 v2.0";
        case 0x88:
        case 0xB2: return "FM17522 clone";
        case 0x12: return "counterfeit MFRC522 (usually works)";
        default: return nullptr;
    }
}

void testLeds() {
    const struct {
        uint8_t pin;
        const char* name;
    } leds[] = {{PIN_LED_GREEN, "green"}, {PIN_LED_YELLOW, "yellow"}, {PIN_LED_RED, "red"}};
    for (const auto& l : leds) {
        Serial.printf("  %s LED (GPIO%u) on\n", l.name, l.pin);
        digitalWrite(l.pin, HIGH);
        delay(700);
        digitalWrite(l.pin, LOW);
        delay(200);
    }
    st::record("LEDs", st::CHECK, "green (GPIO4), yellow (GPIO17), red (GPIO16) should each have lit once, in that order");
}

// Buzzer on GPIO16 (shared with the red LED): the three production patterns.
void beep(uint16_t hz, uint16_t ms) {
    if (BUZZER_ACTIVE) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(ms);
        digitalWrite(PIN_BUZZER, LOW);
        return;
    }
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
    ledcWriteTone(BUZZER_LEDC_CH, hz);
    delay(ms);
    ledcWriteTone(BUZZER_LEDC_CH, 0);
    ledcDetachPin(PIN_BUZZER);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
}

void testBuzzer() {
    Serial.println("  buzzer: GRANTED (two rising chirps)");
    beep(1800, 80); delay(60); beep(2400, 140);
    delay(600);
    Serial.println("  buzzer: DENIED (one low tone)");
    beep(400, 450);
    delay(600);
    Serial.println("  buzzer: TAMPER (alternating, 2 s)");
    for (int i = 0; i < 4; i++) { beep(1000, 250); beep(1800, 250); }
    st::record("buzzer", st::CHECK, "should have played granted (2 chirps), denied (low tone), tamper (2 s siren); red LED glows while it sounds");
}

// Reads a line with the internal pull-down on. An OLED module has its own
// pull-up resistors (about 4.7-10k), which win against the ~45k pull-down, so a
// HIGH reading means a powered module is on that pin.
int levelWithPulldown(uint8_t pin) {
    pinMode(pin, INPUT_PULLDOWN);
    delay(5);
    const int v = digitalRead(pin);
    pinMode(pin, INPUT);
    return v;
}

int scanBus(int sda, int scl, char* found, size_t cap) {
    Wire.begin(sda, scl, 100000);
    int count = 0;
    found[0] = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            count++;
            const size_t n = strlen(found);
            snprintf(found + n, cap - n, "%s0x%02X", n ? " " : "", a);
        }
    }
    Wire.end();
    return count;
}

void diagnoseI2c() {
    Wire.end();
    pinMode(PIN_I2C_SDA, INPUT);
    pinMode(PIN_I2C_SCL, INPUT);
    delay(5);
    const int sdaFloat = digitalRead(PIN_I2C_SDA), sclFloat = digitalRead(PIN_I2C_SCL);
    const int sdaPull = levelWithPulldown(PIN_I2C_SDA), sclPull = levelWithPulldown(PIN_I2C_SCL);
    st::record("I2C lines", (sdaPull && sclPull) ? st::WARN : st::FAIL,
               "SDA GPIO21 %s, SCL GPIO22 %s with pull-down (no pull-down: %s / %s). %s",
               sdaPull ? "HIGH" : "LOW", sclPull ? "HIGH" : "LOW", sdaFloat ? "HIGH" : "LOW",
               sclFloat ? "HIGH" : "LOW",
               sdaPull && sclPull ? "Pull-ups present: a module is powered on both lines, but nothing answered"
               : !sdaPull && !sclPull ? "No pull-up on either line: OLED not powered (VCC/GND) or not on GPIO21/22"
               : "Pull-up on one line only: the other wire is loose or on the wrong pin");

    char found[64];
    const int swapped = scanBus(PIN_I2C_SCL, PIN_I2C_SDA, found, sizeof(found));
    if (swapped)
        st::record("I2C SDA/SCL swapped?", st::FAIL, "with SDA and SCL swapped, found %s: the OLED's SDA and SCL wires are crossed. Swap them", found);
    else
        st::record("I2C SDA/SCL swapped?", st::PASS, "nothing with the lines swapped either, so they are not simply crossed");
}

void testOled() {
    gOledOk = false;
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
    char found[64] = "";
    int count = 0;
    bool has3c = false;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            count++;
            has3c |= a == OLED_ADDR;
            const size_t n = strlen(found);
            snprintf(found + n, sizeof(found) - n, "%s0x%02X", n ? " " : "", a);
        }
    }
    Wire.end();
    if (!has3c) {
        if (count)
            st::record("OLED (I2C)", st::FAIL, "nothing at 0x%02X, but found %s: wrong address? (OLED_ADDR)", OLED_ADDR, found);
        else
            st::record("OLED (I2C)", st::FAIL, "no I2C devices at all: check OLED VCC (3V3), GND, SDA GPIO21, SCL GPIO22");
        diagnoseI2c();
        return;
    }
    gOledOk = gOled.begin(PIN_I2C_SDA, PIN_I2C_SCL, OLED_ADDR, OLED_I2C_HZ, OLED_FLIP);
    if (!gOledOk) {
        st::record("OLED (I2C)", st::FAIL, "0x%02X answered the scan but init failed", OLED_ADDR);
        return;
    }
    showOkScreen();
    st::record("OLED (I2C)", st::PASS, "SSD1306 at 0x%02X initialised. CHECK: 'BLESSED IS THE GOAT' scrolls across the screen (I2C devices: %s)", OLED_ADDR, found);
}

void testRc522() {
    gRfidOk = false;
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
    gRfid.begin(PIN_RC522_CS, RC522_SPI_HZ);
    delay(RC522_RESET_SETTLE_MS);
    const bool ok = gRfid.init();
    const uint8_t v = gRfid.version();
    const char* chip = rc522Name(v);
    if (v == 0x00 || v == 0xFF) {
        st::record("RC522 (SPI)", st::FAIL, "no SPI response (VersionReg 0x%02X): check 3.3V, GND, SDA/CS GPIO5, SCK GPIO18, MOSI GPIO23, MISO GPIO19", v);
    } else if (!ok) {
        st::record("RC522 (SPI)", st::FAIL, "VersionReg 0x%02X but registers did not keep their values", v);
    } else {
        gRfidOk = true;
        st::record("RC522 (SPI)", chip ? st::PASS : st::WARN, "chip answers: %s (VersionReg 0x%02X), antenna on", chip ? chip : "unknown version", v);
    }

    // Rev B: GPIO22 is both OLED SCL and RC522 RST. Check the firmware workaround.
    if (gRfidOk && gOledOk) {
        showOled("<-- KICK TEST -->", "RST TEST");
        delay(RC522_RESET_SETTLE_MS);
        const bool wasReset = !gRfid.isConfigured();
        const bool back = gRfid.init();
        if (wasReset && back)
            st::record("shared GPIO22 (SCL / RST)", st::PASS, "I2C traffic reset the RC522 as expected; firmware re-init works");
        else if (back)
            st::record("shared GPIO22 (SCL / RST)", st::WARN, "RC522 kept its settings during I2C traffic: is RC522 RST really on GPIO22?");
        else
            st::record("shared GPIO22 (SCL / RST)", st::FAIL, "RC522 did not come back after I2C traffic");
        showOkScreen();
    } else {
        st::record("shared GPIO22 (SCL / RST)", st::SKIP, "needs both the OLED and the RC522 working");
    }
    if (gRfidOk) gRfid.antennaOff();
    SPI.end();
}

void testKeypadIdle() {
    gKeypad.begin();
    char stuck = 0;
    for (int i = 0; i < 20 && !stuck; i++) {
        stuck = gKeypad.scan();
        delay(10);
    }
    gKeypad.end();
    if (stuck)
        st::record("keypad (idle)", st::FAIL, "key '%c' reads pressed with nothing touched: stuck key, short, or row/column wiring", stuck);
    else
        st::record("keypad (idle)", st::CHECK, "no stuck keys. A missing keypad also looks like this: run 'keys'");
}

// Wire tester: finds which two of the 8 keypad wires each key press connects,
// whatever the keypad's pin order. Each pin is driven LOW in turn, the other 7
// are read with pull-ups.
const char* kpName(uint8_t i) {
    static char buf[8][12];
    snprintf(buf[i], sizeof(buf[i]), "%s%u(GPIO%u)", i < 4 ? "R" : "C", (i & 3) + 1,
             i < 4 ? PIN_KP_ROWS[i] : PIN_KP_COLS[i - 4]);
    return buf[i];
}

uint8_t kpPin(uint8_t i) { return i < 4 ? PIN_KP_ROWS[i] : PIN_KP_COLS[i - 4]; }

// Returns the first connected pair (a < b) as a * 8 + b, or -1.
int kpConnectedPair() {
    for (uint8_t i = 0; i < 8; i++) pinMode(kpPin(i), INPUT_PULLUP);
    int found = -1;
    for (uint8_t i = 0; i < 8 && found < 0; i++) {
        pinMode(kpPin(i), OUTPUT);
        digitalWrite(kpPin(i), LOW);
        delayMicroseconds(50);
        for (uint8_t j = i + 1; j < 8; j++)
            if (digitalRead(kpPin(j)) == LOW) { found = i * 8 + j; break; }
        pinMode(kpPin(i), INPUT_PULLUP);
    }
    return found;
}

void testKeyPins() {
    showOled("KEYPAD WIRES", "PRESS KEYS");
    const int idle = kpConnectedPair();
    if (idle >= 0)
        Serial.printf("  !! with no key pressed, %s and %s are connected: short or a key stuck down\n",
                      kpName(idle / 8), kpName(idle % 8));
    Serial.println("\n>> Press each key once, slowly, in label order 1 2 3 A 4 5 6 B 7 8 9 C * 0 # D (90 s).");
    Serial.println("   Each press prints the two wires it connects. Type anything to stop.");
    int last = idle;
    uint8_t presses = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 90000) {
        const int p = kpConnectedPair();
        if (p != last && p >= 0) {
            const uint8_t a = p / 8, b = p % 8;
            presses++;
            if (a < 4 && b >= 4)
                Serial.printf("  press %2u: %s + %s  (current map reads this as '%c')\n", presses, kpName(a),
                              kpName(b), KEYMAP[a][b - 4]);
            else
                Serial.printf("  press %2u: %s + %s  (two %s: the current pin map cannot read this key)\n",
                              presses, kpName(a), kpName(b), a < 4 ? "row pins" : "column pins");
        }
        last = p;
        if (st::abortRequested()) break;
        delay(10);
    }
    for (uint8_t i = 0; i < 8; i++) pinMode(kpPin(i), INPUT);
    st::record("keypad wires", st::CHECK, "%u presses seen. Send the press list to the developer to fix the pin order", presses);
    showOkScreen();
}

void testKeys() {
    Serial.println("\n>> Press every key once, any order (60 s). Type anything to stop.");
    showOled("PRESS EVERY KEY", "0/16");
    gKeypad.begin();
    bool seen[4][4] = {};
    int count = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 60000 && count < 16) {
        const char k = gKeypad.poll(millis());
        if (k) {
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    if (KEYMAP[r][c] == k && !seen[r][c]) {
                        seen[r][c] = true;
                        count++;
                    }
            Serial.printf("  key '%c'  (%d/16)\n", k, count);
            char big[20];
            snprintf(big, sizeof(big), "%c  %d/16", k, count);
            showOled("PRESS EVERY KEY", big);
        }
        if (st::abortRequested()) break;
        delay(1);
    }
    gKeypad.end();
    if (count == 16) {
        st::record("keypad (all 16 keys)", st::PASS, "every key detected");
        return;
    }
    char missing[64] = "", hint[96] = "";
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!seen[r][c]) {
                const size_t n = strlen(missing);
                snprintf(missing + n, sizeof(missing) - n, "%c", KEYMAP[r][c]);
            }
    for (int r = 0; r < 4; r++) {
        bool any = false;
        for (int c = 0; c < 4; c++) any |= seen[r][c];
        if (!any && count) {
            const size_t n = strlen(hint);
            snprintf(hint + n, sizeof(hint) - n, " row R%d (GPIO%u) dead?", r + 1, PIN_KP_ROWS[r]);
        }
    }
    for (int c = 0; c < 4; c++) {
        bool any = false;
        for (int r = 0; r < 4; r++) any |= seen[r][c];
        if (!any && count) {
            const size_t n = strlen(hint);
            snprintf(hint + n, sizeof(hint) - n, " col C%d (GPIO%u) dead?", c + 1, PIN_KP_COLS[c]);
        }
    }
    st::record("keypad (all 16 keys)", st::FAIL, "%d/16 keys, missing: %s.%s", count, missing,
               count ? hint : " No key at all: keypad not connected?");
}

void testCard() {
    Serial.println("\n>> Tap a card or tag on the reader (15 s). Type anything to stop.");
    showOled("KICK TEST", "TAP CARD");
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
    gRfid.begin(PIN_RC522_CS, RC522_SPI_HZ);
    delay(RC522_RESET_SETTLE_MS);   // the OLED update above reset the RC522
    if (!gRfid.init()) {
        st::record("RC522 card read", st::FAIL, "RC522 not responding (see the RC522 (SPI) result)");
        SPI.end();
        return;
    }
    bool got = false;
    const uint32_t t0 = millis();
    while (millis() - t0 < 15000 && !got) {
        uint8_t uid[10], len = 0;
        if (gRfid.readUid(uid, len)) {
            char hex[24] = "";
            for (uint8_t i = 0; i < len; i++) snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02X", uid[i]);
            st::record("RC522 card read", st::PASS, "read UID %s (%u bytes)", hex, len);
            got = true;
        }
        if (st::abortRequested()) break;
        delay(100);
    }
    if (!got) st::record("RC522 card read", st::FAIL, "no card read in 15 s: needs a 13.56 MHz ISO 14443A card (MIFARE), held flat on the reader");
    gRfid.antennaOff();
    SPI.end();
    showOled("<-- KICK TEST -->", got ? "CARD OK" : "NO CARD");
}

// Visibility check. Step 1 bypasses screen memory; steps 2 and 3 draw from
// memory with the SSD1306 and SH1106 column layouts.
void oledCmds(const uint8_t* c, size_t n) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write(c, n);
    Wire.endTransmission();
}

void testOledVisible() {
    if (!gOledOk) {
        st::record("OLED visible", st::SKIP, "OLED did not initialise (see the OLED (I2C) result)");
        return;
    }
    Serial.println("\n>> Watch the OLED. Step 1 (5 s): hardware all-pixels-on, bypasses screen memory");
    const uint8_t on[] = {0x8D, 0x14, 0x81, 0xFF, 0xA5, 0xAF};   // charge pump, max contrast, all on, display on
    oledCmds(on, sizeof(on));
    delay(5000);
    const uint8_t back[] = {0xA4, 0x81, 0x8F};   // show memory again, normal contrast
    oledCmds(back, sizeof(back));

    const uint32_t e0 = gOled.writeErrors();
    const struct { uint8_t off; const char* name; } layouts[] = {{0, "SSD1306"}, {2, "SH1106"}};
    for (int k = 0; k < 2; k++) {
        Serial.printf(">> Step %d (%s layout, column offset %u): ALL WHITE 3 s, then '%s' 3 s\n",
                      k + 2, layouts[k].name, layouts[k].off, layouts[k].name);
        gOled.setColumnOffset(layouts[k].off);
        gOled.clear();
        for (int y = 0; y < Oled::H; y += 8) gOled.text(0, y, "#####################");
        gOled.commit();
        delay(3000);
        showOled("OLED TEST", layouts[k].name);
        delay(3000);
    }
    gOled.setColumnOffset(OLED_COL_OFFSET);
    const uint32_t errs = gOled.writeErrors() - e0;
    Serial.printf("  I2C writes not acknowledged during the test: %lu\n", (unsigned long)errs);
    st::record("OLED visible", errs ? st::FAIL : st::CHECK,
               "%lu failed I2C writes. Step 1 dark = panel/power. Readable text in step 2 = SSD1306 (OLED_COL_OFFSET 0); only in step 3 = SH1106 (set 2); neither = tell the developer",
               (unsigned long)errs);
    showOkScreen();
}

// Which rows does the panel show? Labels one line per 8-row page, first with the
// current COM pin setting (0x02, for 128x32 panels), then with 0x12 (128x64 style).
void testOledRows() {
    if (!gOledOk) {
        st::record("OLED rows", st::SKIP, "OLED did not initialise");
        return;
    }
    gMarquee = false;
    const uint8_t coms[] = {0x02, 0x12};
    for (int k = 0; k < 2; k++) {
        Serial.printf("\n>> Step %c (6 s, COM pins 0x%02X): which of the 4 lines are fully readable?\n", 'A' + k, coms[k]);
        const uint8_t c[] = {0xDA, coms[k]};
        Wire.beginTransmission(OLED_ADDR);
        Wire.write((uint8_t)0x00);
        Wire.write(c, sizeof(c));
        Wire.endTransmission();
        gOled.clear();
        gOled.text(0, 0, k ? "B1 ROWS 0-7" : "A1 ROWS 0-7");
        gOled.text(0, 8, k ? "B2 ROWS 8-15" : "A2 ROWS 8-15");
        gOled.text(0, 16, k ? "B3 ROWS 16-23" : "A3 ROWS 16-23");
        gOled.text(0, 24, k ? "B4 ROWS 24-31" : "A4 ROWS 24-31");
        gOled.forceRedraw();
        gOled.commit();
        delay(6000);
    }
    const uint8_t back[] = {0xDA, 0x02};
    Wire.beginTransmission(OLED_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write(back, sizeof(back));
    Wire.endTransmission();
    st::record("OLED rows", st::CHECK, "report which labels were readable: all of A1-A4 = row setup is fine; all of B1-B4 = panel needs COM 0x12; tell the developer");
    showOkScreen();
}

void runAuto() {
    Serial.println("\n---- automatic checks ----");
    st::testRadio();
    testLeds();
    testBuzzer();
    testOled();
    testRc522();
    testKeypadIdle();
    st::summary();
}

void help() {
    Serial.println("commands: keys | keypins | card | oled | rows | leds | buzzer | auto | all | summary | help");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(50);
    for (uint8_t p : {PIN_LED_GREEN, PIN_LED_YELLOW, PIN_LED_RED}) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    gKeypad.end();   // all keypad pins high impedance
    pinMode(PIN_RC522_CS, OUTPUT);
    digitalWrite(PIN_RC522_CS, HIGH);
    if (!BUZZER_ACTIVE) ledcSetup(BUZZER_LEDC_CH, 2000, 10);
    Serial.println("\n[kick] EXTERIOR component kick test. Hardware Rev B pin map.");
    runAuto();
    help();
}

void loop() {
    static char line[32];
    marqueeStep();
    if (st::readLine(line, sizeof(line))) {
        if (!strcmp(line, "keys")) testKeys();
        else if (!strcmp(line, "keypins")) testKeyPins();
        else if (!strcmp(line, "card")) testCard();
        else if (!strcmp(line, "leds")) testLeds();
        else if (!strcmp(line, "buzzer")) testBuzzer();
        else if (!strcmp(line, "oled")) testOledVisible();
        else if (!strcmp(line, "rows")) testOledRows();
        else if (!strcmp(line, "auto")) { runAuto(); return; }
        else if (!strcmp(line, "all")) { runAuto(); testKeys(); testCard(); }
        else if (!strcmp(line, "summary")) {}
        else if (!strcmp(line, "help")) { help(); return; }
        else { st::unknownCommand(line); help(); return; }
        st::summary();
    }
    delay(5);
}
