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
uint32_t gActiveSince = 0;
bool gWasActive = false;

void IRAM_ATTR onEdge() {
    const uint32_t now = millis();
    portENTER_CRITICAL_ISR(&gMux);
    if (now - gLastEdgeMs >= INTRUSION_EDGE_MIN_GAP_MS) {
        gEdges[gHead] = now;
        gHead = (gHead + 1) % RING;
        gTotal = gTotal + 1;
        gLastEdgeMs = now;
    }
    portEXIT_CRITICAL_ISR(&gMux);
}

}  // namespace

void begin() {
    pinMode(PIN_INTRUSION, INPUT);   // both sensor modules drive the line
    attachInterrupt(digitalPinToInterrupt(PIN_INTRUSION), onEdge,
                    INTRUSION_ACTIVE_LEVEL == HIGH ? RISING : FALLING);
}

bool levelActive() { return digitalRead(PIN_INTRUSION) == INTRUSION_ACTIVE_LEVEL; }

uint32_t totalEdges() { return gTotal; }

bool update(uint32_t nowMs, bool armed, uint8_t requiredEvents) {
    const bool active = levelActive();
    if (active && !gWasActive) gActiveSince = nowMs;
    gWasActive = active;

    uint8_t recent = 0;
    portENTER_CRITICAL(&gMux);
    for (uint8_t i = 0; i < RING; i++) {
        if (!armed) gEdges[i] = 0;
        else if (gEdges[i] && nowMs - gEdges[i] <= INTRUSION_WINDOW_MS) recent++;
    }
    portEXIT_CRITICAL(&gMux);

    if (!armed) {
        gActiveSince = nowMs;   // hold time only counts while armed
        return false;
    }
    if (recent >= requiredEvents) return true;
    return active && nowMs - gActiveSince >= INTRUSION_HOLD_MS;
}

}  // namespace intrusion
