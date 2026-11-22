// Interior module (servo bolt, sensors, buzzer). Hardware Rev B, wiring unchanged.
// Defaults that still need team confirmation are marked TBC (docs/OPEN_QUESTIONS.md).
#pragma once
#include <stdint.h>
#include <Arduino.h>

// ---- Bolt servo (S3003) ---------------------------------------------------
// TBC (Q1): servo is not on the Rev B drawing. Assumed signal on GPIO18
// (the old L293D 1A line), powered from the 5 V rail.
constexpr uint8_t PIN_SERVO = 18;
constexpr uint8_t PIN_L293D_EN = 23;    // held LOW so the L293D stays off
constexpr uint8_t PIN_L293D_IN2 = 19;   // held LOW
constexpr uint8_t SERVO_LEDC_CH = 0;    // LEDC timer 0
constexpr uint16_t SERVO_LOCK_US = 1000;     // TBC (Q3)
constexpr uint16_t SERVO_UNLOCK_US = 2000;   // TBC (Q3)
constexpr uint32_t SERVO_MOVE_TIMEOUT_MS = 1500;
constexpr uint32_t SERVO_SETTLE_MS = 150;    // keep driving after the switch closes
constexpr uint32_t SERVO_RETRY_BACKOFF_MS = 400;

// ---- Limit and reed switches (INPUT_PULLUP, close to GND) -----------------
constexpr uint8_t PIN_SW_LOCKED = 26;     // TBC (Q5): SW1 = bolt locked
constexpr uint8_t PIN_SW_UNLOCKED = 25;   // TBC (Q5): SW2 = bolt unlocked
constexpr uint8_t PIN_REED = 27;
constexpr int SWITCH_PRESSED_LEVEL = LOW;
constexpr int DOOR_CLOSED_LEVEL = LOW;    // TBC (Q6)
constexpr uint32_t SWITCH_DEBOUNCE_MS = 20;
constexpr uint32_t REED_DEBOUNCE_MS = 50;

// ---- Intrusion input: SW-420 DO and PIR OUT tied together on GPIO32 -------
constexpr uint8_t PIN_INTRUSION = 32;
constexpr int INTRUSION_ACTIVE_LEVEL = HIGH;   // TBC (Q7): measure with interior_selftest ("intrusion")
constexpr uint32_t INTRUSION_WINDOW_MS = 2000;
constexpr uint32_t INTRUSION_EDGE_MIN_GAP_MS = 30;  // one knock = one event
constexpr uint32_t INTRUSION_HOLD_MS = 1000;        // PIR gives one long pulse
constexpr uint32_t PIR_WARMUP_MS = 60000;
constexpr uint32_t ARM_DELAY_MS = 10000;            // intrusion ignored right after locking

// ---- Potentiometer: intrusion sensitivity (Q11) ---------------------------
constexpr uint8_t PIN_POT = 34;
constexpr uint8_t SENSITIVITY_MIN_EVENTS = 1;    // pot at max -> 1 event trips
constexpr uint8_t SENSITIVITY_MAX_EVENTS = 10;   // pot at min -> 10 events

// ---- Buzzer and LED -------------------------------------------------------
constexpr uint8_t PIN_BUZZER = 14;
constexpr uint8_t BUZZER_LEDC_CH = 2;   // LEDC timer 1 (separate from servo)
constexpr uint8_t PIN_LED_RED = 4;

// ---- Auto-relock (Q18) ----------------------------------------------------
constexpr uint32_t RELOCK_IF_NOT_OPENED_MS = 10000;
constexpr uint32_t RELOCK_AFTER_CLOSE_MS = 3000;

// ---- Alarm (Q12, Q19) -----------------------------------------------------
constexpr uint32_t ALARM_SIREN_MS = 180000;
constexpr bool LINK_LOSS_IS_ALARM = true;
constexpr bool BOLT_TAMPER_IS_ALARM = true;   // bolt leaves "locked" without a command
constexpr uint32_t BOLT_TAMPER_MS = 300;
constexpr uint8_t RF_TAMPER_FAILS = 5;
constexpr uint32_t RF_TAMPER_WINDOW_MS = 10000;

// ---- Credentials and lockout (Q17) ----------------------------------------
constexpr uint8_t MAX_CREDENTIALS = 20;
constexpr uint32_t PBKDF2_ITERATIONS = 4096;   // tune for about 200 ms (printed on enroll)
constexpr uint32_t LOCKOUT_THRESHOLD = 5;
constexpr uint32_t LOCKOUT_BASE_S = 30;
constexpr uint32_t LOCKOUT_MAX_S = 900;
constexpr uint32_t AUTH_MIN_INTERVAL_MS = 1000;
constexpr uint32_t ENROLL_TIMEOUT_MS = 60000;

// ---- Serial console -------------------------------------------------------
// lock / unlock / arm / disarm / clear / resetlockout. Turn off for production.
constexpr bool CONSOLE_TEST_COMMANDS = true;
