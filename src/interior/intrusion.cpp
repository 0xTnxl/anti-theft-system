#include "intrusion.h"

#include <Arduino.h>

#include "config_interior.h"

namespace intrusion {
namespace {

constexpr uint8_t RING = 16;
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t gEdges[RING];
volatile uint8_t gHead = 0;
volatile uint32_t gTotal = 0;
volatile uint32_t gLastEdgeMs = 0;
uint32_t gPirActiveSince = 0;
bool gPirWasActive = false;

void IRAM_ATTR onVibration() {
    const uint32_t now = millis();
    portENTER_CRITICAL_ISR(&gMux);
    if (now - gLastEdgeMs >= VIBRATION_EDGE_MIN_GAP_MS) {
        gEdges[gHead] = now;
        gHead = (gHead + 1) % RING;
        gTotal = gTotal + 1;
        gLastEdgeMs = now;
    }
    portEXIT_CRITICAL_ISR(&gMux);
}

}  // namespace

void begin() {
    pinMode(PIN_VIBRATION, INPUT);
    pinMode(PIN_PIR, INPUT);   // GPIO34: input only, the HC-SR501 drives the line itself
    attachInterrupt(digitalPinToInterrupt(PIN_VIBRATION), onVibration,
                    VIBRATION_ACTIVE_LEVEL == HIGH ? RISING : FALLING);
}

bool vibrationActive() { return digitalRead(PIN_VIBRATION) == VIBRATION_ACTIVE_LEVEL; }
bool motionActive() { return digitalRead(PIN_PIR) == PIR_ACTIVE_LEVEL; }
uint32_t vibrationEdges() { return gTotal; }

Trip update(uint32_t nowMs, bool armed) {
    const bool pir = motionActive();
    if (!armed || !pir) gPirActiveSince = nowMs;   // hold time only counts while armed
    gPirWasActive = pir;

    uint8_t recent = 0;
    portENTER_CRITICAL(&gMux);
    for (uint8_t i = 0; i < RING; i++) {
        if (!armed) gEdges[i] = 0;
        else if (gEdges[i] && nowMs - gEdges[i] <= VIBRATION_WINDOW_MS) recent++;
    }
    portEXIT_CRITICAL(&gMux);

    if (!armed) return NONE;
    if (recent >= VIBRATION_EVENTS) return VIBRATION;
    if (pir && nowMs - gPirActiveSince >= PIR_HOLD_MS) return MOTION;
    return NONE;
}

}  // namespace intrusion
