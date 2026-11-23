// L298N channel A driver for the bolt motor, plus the quadrature encoder.
// Used by the interior firmware and the interior kick test.
#pragma once
#include <stdint.h>

namespace motor {

enum class Dir : int8_t { Lock = -1, Stop = 0, Unlock = 1 };

void begin();                 // all driver pins LOW (motor off), encoder interrupts on
void drive(Dir d, uint8_t dutyPct);
void brake();                 // IN1 = IN2 = LOW with ENA on: short the motor
void coast();                 // ENA LOW: driver off
Dir direction();

int32_t count();              // encoder position (signed quadrature counts)
uint32_t lastEdgeMs();        // millis() of the last encoder edge
uint32_t badTransitions();    // both channels changed at once (noise / too fast)

// Call often while driving. True when the encoder has shown no movement for
// ENCODER_STALL_MS after the startup time (always false if detection is off).
bool stalled(uint32_t nowMs);

}  // namespace motor
