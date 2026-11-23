// Intrusion sensors, now on separate pins:
//   SW-420 vibration on GPIO32: trips on VIBRATION_EVENTS knocks within VIBRATION_WINDOW_MS.
//   HC-SR501 motion on GPIO34:  trips when the output stays active for PIR_HOLD_MS.
#pragma once
#include <stdint.h>

namespace intrusion {

enum Trip : uint8_t { NONE = 0, VIBRATION = 1, MOTION = 2 };

void begin();
// While !armed, events are discarded. Returns which sensor tripped (vibration first).
Trip update(uint32_t nowMs, bool armed);
bool vibrationActive();
bool motionActive();
uint32_t vibrationEdges();   // diagnostics

}  // namespace intrusion
