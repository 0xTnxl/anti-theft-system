// Combined intrusion input on GPIO32 (SW-420 DO and HC-SR501 OUT tied together).
// The firmware cannot tell which sensor fired. It trips when either
//   - at least N active edges arrive within INTRUSION_WINDOW_MS (vibration), or
//   - the line stays active for INTRUSION_HOLD_MS (PIR gives one long pulse).
// N comes from the potentiometer.
#pragma once
#include <stdint.h>

namespace intrusion {

void begin();
// Returns true when the filter trips. While !armed, events are discarded.
bool update(uint32_t nowMs, bool armed, uint8_t requiredEvents);
bool levelActive();
uint32_t totalEdges();   // diagnostics

}  // namespace intrusion
