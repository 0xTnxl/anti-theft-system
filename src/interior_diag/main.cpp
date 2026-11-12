// Interior diagnostic build: answers the hardware questions in
// docs/OPEN_QUESTIONS.md (#3, #5, #6, #7) on the real board.
//
// Prints all inputs every 250 ms and every GPIO32 edge as it happens.
// Commands (type + Enter):
//   s <us>   drive the servo with a pulse width, e.g. "s 1500"
//   r        release the servo (no pulses)
//   b        short beep
//   q        toggle the periodic print (edges are always shown)
//
// GPIO32 test (Q7): trigger the SW-420 alone (tap it), then the PIR alone
// (wave a hand after its 60 s warm-up). Note the idle level and the level
// each sensor produces. If one sensor's activity never shows up, the two
// outputs are fighting and the wiring has to change.
#include <Arduino.h>

#include "config_interior.h"

namespace {

volatile uint32_t gRise = 0, gFall = 0;
uint32_t gSeenRise = 0, gSeenFall = 0;
bool gPeriodic = true;
char gLine[32];
uint8_t gLen = 0;

void IRAM_ATTR onEdge() {
    if (digitalRead(PIN_INTRUSION)) gRise = gRise + 1;
    else gFall = gFall + 1;
}

const char* pressed(uint8_t pin) { return digitalRead(pin) == SWITCH_PRESSED_LEVEL ? "PRESSED" : "open   "; }

void command(char* c) {
    if (c[0] == 's') {
        const int us = atoi(c + 1);
        if (us >= 500 && us <= 2500) {
            ledcAttachPin(PIN_SERVO, SERVO_LEDC_CH);
            ledcWrite(SERVO_LEDC_CH, (uint32_t)us * 65536UL / 20000UL);
            Serial.printf("servo %d us\n", us);
        } else {
            Serial.println("pulse must be 500..2500 us");
        }
    } else if (c[0] == 'r') {
        ledcDetachPin(PIN_SERVO);
        digitalWrite(PIN_SERVO, LOW);
        Serial.println("servo released");
    } else if (c[0] == 'b') {
        ledcWriteTone(BUZZER_LEDC_CH, 2000);
        delay(150);
        ledcWriteTone(BUZZER_LEDC_CH, 0);
    } else if (c[0] == 'q') {
        gPeriodic = !gPeriodic;
    }
}

}  // namespace

void setup() {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    pinMode(PIN_L293D_EN, OUTPUT);
    digitalWrite(PIN_L293D_EN, LOW);
    pinMode(PIN_L293D_IN2, OUTPUT);
    digitalWrite(PIN_L293D_IN2, LOW);
    pinMode(PIN_SERVO, OUTPUT);
    digitalWrite(PIN_SERVO, LOW);
    pinMode(PIN_LED_RED, OUTPUT);
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
    Serial.println("\n[diag] interior diagnostics. Commands: s <us> | r | b | q");
    Serial.printf("[diag] SW_LOCKED=GPIO%u SW_UNLOCKED=GPIO%u REED=GPIO%u INTRUSION=GPIO%u\n",
                  PIN_SW_LOCKED, PIN_SW_UNLOCKED, PIN_REED, PIN_INTRUSION);
}

void loop() {
    static uint32_t last = 0;
    const uint32_t now = millis();

    if (gRise != gSeenRise || gFall != gSeenFall) {
        Serial.printf("[%7lu ms] GPIO32 edge: rising=%lu falling=%lu level=%d\n",
                      (unsigned long)now, (unsigned long)gRise, (unsigned long)gFall,
                      digitalRead(PIN_INTRUSION));
        gSeenRise = gRise;
        gSeenFall = gFall;
    }

    if (gPeriodic && now - last >= 250) {
        last = now;
        Serial.printf("SW_LOCKED %s  SW_UNLOCKED %s  REED %s  GPIO32=%d  POT=%4d\n",
                      pressed(PIN_SW_LOCKED), pressed(PIN_SW_UNLOCKED),
                      digitalRead(PIN_REED) == LOW ? "closed" : "open  ",
                      digitalRead(PIN_INTRUSION), analogRead(PIN_POT));
    }
    digitalWrite(PIN_LED_RED, (now / 500) & 1);

    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            gLine[gLen] = 0;
            if (gLen) command(gLine);
            gLen = 0;
        } else if (gLen < sizeof(gLine) - 1) {
            gLine[gLen++] = c;
        }
    }
    delay(2);
}
