#include "motor.h"

#include <Arduino.h>

#include "config_interior.h"

namespace motor {
namespace {

portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
volatile int32_t gCount = 0;
volatile uint32_t gLastEdgeMs = 0;
volatile uint32_t gBad = 0;
volatile uint8_t gPrev = 0;
Dir gDir = Dir::Stop;
uint32_t gDriveSinceMs = 0;
int32_t gCountAtDrive = 0;

// Quadrature table indexed by (previous AB << 2) | current AB.
// +1 / -1 for a valid step, 0 for no change, 2 for an invalid double change.
const int8_t QDEC[16] = {0, 1, -1, 2, -1, 0, 2, 1, 1, 2, 0, -1, 2, -1, 1, 0};

void IRAM_ATTR onEdge() {
    const uint8_t ab = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
    portENTER_CRITICAL_ISR(&gMux);
    const int8_t step = QDEC[(gPrev << 2) | ab];
    if (step == 2) gBad = gBad + 1;
    else if (step) gCount = gCount + step;
    gPrev = ab;
    gLastEdgeMs = millis();
    portEXIT_CRITICAL_ISR(&gMux);
}

void pins(bool in1, bool in2) {
    digitalWrite(PIN_MOTOR_IN1, in1);
    digitalWrite(PIN_MOTOR_IN2, in2);
}

uint32_t dutyRaw(uint8_t pct) { return (uint32_t)min<uint8_t>(pct, 100) * 1023 / 100; }

}  // namespace

void begin() {
    pinMode(PIN_MOTOR_IN1, OUTPUT);
    pinMode(PIN_MOTOR_IN2, OUTPUT);
    pins(false, false);
    ledcSetup(MOTOR_LEDC_CH, MOTOR_PWM_HZ, 10);
    ledcAttachPin(PIN_MOTOR_ENA, MOTOR_LEDC_CH);
    ledcWrite(MOTOR_LEDC_CH, 0);
    gDir = Dir::Stop;

    pinMode(PIN_ENC_A, INPUT);   // no pull-ups: GPIO35 has none, keep both channels alike
    pinMode(PIN_ENC_B, INPUT);
    gPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), onEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), onEdge, CHANGE);
}

void drive(Dir d, uint8_t dutyPct) {
    if (d == Dir::Stop) {
        coast();
        return;
    }
    const bool fwd = (d == Dir::Unlock) != MOTOR_INVERT;
    ledcWrite(MOTOR_LEDC_CH, 0);   // never switch direction with the bridge on
    pins(fwd, !fwd);
    ledcWrite(MOTOR_LEDC_CH, dutyRaw(dutyPct));
    gDir = d;
    gDriveSinceMs = millis();
    gCountAtDrive = count();
}

void brake() {
    pins(false, false);
    ledcWrite(MOTOR_LEDC_CH, dutyRaw(100));
    gDir = Dir::Stop;
}

void coast() {
    ledcWrite(MOTOR_LEDC_CH, 0);
    pins(false, false);
    gDir = Dir::Stop;
}

Dir direction() { return gDir; }

int32_t count() {
    portENTER_CRITICAL(&gMux);
    const int32_t c = gCount;
    portEXIT_CRITICAL(&gMux);
    return c;
}

uint32_t lastEdgeMs() { return gLastEdgeMs; }
uint32_t badTransitions() { return gBad; }

bool stalled(uint32_t nowMs) {
    if (!ENCODER_STALL_DETECT || gDir == Dir::Stop) return false;
    // Signed differences: a caller's "now" may be slightly older than the
    // drive start, which must read as "just started", not as a huge interval.
    const int32_t running = (int32_t)(nowMs - gDriveSinceMs);
    if (running < (int32_t)ENCODER_STARTUP_MS) return false;
    const bool movedAtAll = count() != gCountAtDrive;
    const int32_t since = movedAtAll ? (int32_t)(nowMs - gLastEdgeMs) : running - (int32_t)ENCODER_STARTUP_MS;
    return since >= (int32_t)ENCODER_STALL_MS;
}

}  // namespace motor
