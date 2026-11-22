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

void showOled(const char* top, const char* big) {
    if (!gOledOk) return;
    gOled.clear();
    gOled.textCentered(0, top);
    gOled.textCentered(14, big, 2);
    gOled.commit();
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
        return;
    }
    gOledOk = gOled.begin(PIN_I2C_SDA, PIN_I2C_SCL, OLED_ADDR, OLED_I2C_HZ, OLED_FLIP);
    if (!gOledOk) {
        st::record("OLED (I2C)", st::FAIL, "0x%02X answered the scan but init failed", OLED_ADDR);
        return;
    }
    showOled("<-- KICK TEST -->", "OLED OK");
    st::record("OLED (I2C)", st::PASS, "SSD1306 at 0x%02X initialised. CHECK: screen shows 'KICK TEST / OLED OK' (I2C devices: %s)", OLED_ADDR, found);
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
        showOled("<-- KICK TEST -->", "OLED OK");
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

void runAuto() {
    Serial.println("\n---- automatic checks ----");
    st::testRadio();
    testLeds();
    testOled();
    testRc522();
    testKeypadIdle();
    st::summary();
}

void help() {
    Serial.println("commands: keys | card | leds | auto | all | summary | help");
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
    Serial.println("\n[kick] EXTERIOR component kick test. Hardware Rev B pin map.");
    runAuto();
    help();
}

void loop() {
    static char line[32];
    if (st::readLine(line, sizeof(line))) {
        if (!strcmp(line, "keys")) testKeys();
        else if (!strcmp(line, "card")) testCard();
        else if (!strcmp(line, "leds")) testLeds();
        else if (!strcmp(line, "auto")) { runAuto(); return; }
        else if (!strcmp(line, "all")) { runAuto(); testKeys(); testCard(); }
        else if (!strcmp(line, "summary")) {}
        else { help(); return; }
        st::summary();
    }
    delay(5);
}
