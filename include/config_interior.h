// Interior module (bolt motor, sensors, buzzer).
// Wiring: "Proposed updated wiring list" (L298N channel A + JGB37-520 geared
// motor with encoder, PIR on its own pin, no potentiometer). The exterior is
// unchanged (Rev B). Defaults that still need team confirmation are marked TBC
// (docs/OPEN_QUESTIONS.md).
#pragma once
#include <stdint.h>
#include <Arduino.h>

// ---- Bolt motor: L298N channel A ------------------------------------------
// The L298N ENA jumper must be REMOVED: with it fitted, ENA is tied to the
// board's 5 V and would be shorted against GPIO23.
constexpr uint8_t PIN_MOTOR_IN1 = 18;
constexpr uint8_t PIN_MOTOR_IN2 = 19;
constexpr uint8_t PIN_MOTOR_ENA = 23;     // PWM
constexpr uint8_t MOTOR_LEDC_CH = 0;      // LEDC timer 0
constexpr uint32_t MOTOR_PWM_HZ = 1000;   // the L298N switches slowly; keep this low
constexpr uint8_t MOTOR_DUTY_PCT = 100;   // drive strength while moving
// TBC (Q1): which way is "unlock" depends on how M1/M2 and the bolt are fitted.
// false: unlock = IN1 HIGH, IN2 LOW.  true: swapped.
constexpr bool MOTOR_INVERT = false;
constexpr uint32_t MOTOR_BRAKE_MS = 250;           // brake (IN1 = IN2 = LOW, ENA on), then coast

// ---- Bolt sequence (turns of the motor output shaft) ----------------------
// Positions counted from "locked" (0) in the unlock (anticlockwise) direction:
//   card + PIN: 0 -> 3 turns (2 retract the bolt, 1 opens the latch), hold there
//   door seen open: back 1 turn to 2 (latch released, bolt stays retracted)
//   card + A:   2 -> 0 (bolt thrown). There is no automatic relock.
// Counts per output turn (Q25): confirmed on the bench, 'turns u 1' = exactly one turn.
constexpr int32_t COUNTS_PER_TURN = 3300;
constexpr int8_t ENC_UNLOCK_SIGN = 1;      // encoder counts up while driving "unlock" (bench: +786)
constexpr uint8_t UNLOCK_TURNS = 2;        // bolt retract
constexpr uint8_t LATCH_TURNS = 1;         // extra turn that opens the latch, then held
constexpr uint8_t LOCK_TURNS = 2;          // card + A: back to 0
constexpr int32_t MOVE_STOP_LEAD_COUNTS = 450;  // first guess at the coast after braking; learned per move
// Last part of each move at reduced power, so the motor stops close to the
// target instead of coasting ~0.27 turns past it (measured on the bench).
constexpr int32_t SLOW_ZONE_COUNTS = COUNTS_PER_TURN / 3;
constexpr uint8_t SLOW_DUTY_PCT = 45;
// Moves are made one turn at a time with a short stop in between, so each
// turn is felt separately (unlock: 1, pause, 2, pause, 3).
constexpr uint32_t TURN_PAUSE_MS = 300;
constexpr uint32_t TURN_TIME_MS = 700;     // rough time per turn, sets the move timeout
constexpr uint8_t HOLD_DUTY_PCT = 25;      // push used to keep the latch at its position
constexpr int32_t HOLD_BAND_COUNTS = 150;  // hold: push back only when the shaft drifts this far
constexpr uint32_t HOLD_MAX_MS = 15000;    // door not opened in time: release anyway
constexpr uint32_t DOOR_OPEN_CONFIRM_MS = 1000;   // reed open this long = door pushed open
// TBC (Q26): true if a spring returns the latch by itself: release = motor off.
constexpr bool RELEASE_BY_SPRING = false;
// SW1 must close at "locked" and SW2 at "latch held"; reaching the turn count
// without the switch is a FAULT. The bench/sim builds have no switches.
constexpr bool REQUIRE_END_SWITCHES = true;
constexpr uint8_t SEEK_EXTRA_TURNS = 1;    // position unknown: search this far past the end

// ---- Motor encoder (C1, C2) -----------------------------------------------
// GPIO35 is input only with no internal pull resistors. If the encoder outputs
// are open collector, fit external pull-ups to 3V3 (Q3). Never feed 5 V in.
constexpr uint8_t PIN_ENC_A = 33;   // C1
constexpr uint8_t PIN_ENC_B = 35;   // C2
// Stop and report FAULT when the encoder shows no movement while driving.
// Set to false if the encoder is not connected yet (timeout still applies).
constexpr bool ENCODER_STALL_DETECT = true;
constexpr uint32_t ENCODER_STARTUP_MS = 250;   // motor spin-up before stall checks
constexpr uint32_t ENCODER_STALL_MS = 250;     // no count for this long = stalled

// ---- Limit and reed switches (INPUT_PULLUP, close to GND) -----------------
constexpr uint8_t PIN_SW_LOCKED = 26;     // TBC (Q5): SW1 = bolt locked
constexpr uint8_t PIN_SW_UNLOCKED = 25;   // TBC (Q5): SW2 = bolt unlocked
constexpr uint8_t PIN_REED = 27;
constexpr int SWITCH_PRESSED_LEVEL = LOW;
constexpr int DOOR_CLOSED_LEVEL = LOW;    // TBC (Q6)
constexpr uint32_t SWITCH_DEBOUNCE_MS = 20;
constexpr uint32_t REED_DEBOUNCE_MS = 50;

// ---- Vibration: SW-420 DO on GPIO32 ---------------------------------------
constexpr uint8_t PIN_VIBRATION = 32;
constexpr int VIBRATION_ACTIVE_LEVEL = HIGH;   // TBC (Q7): check with the kick test
constexpr uint8_t VIBRATION_EVENTS = 3;        // knocks within the window that trip (Q11)
constexpr uint32_t VIBRATION_WINDOW_MS = 2000;
constexpr uint32_t VIBRATION_EDGE_MIN_GAP_MS = 30;   // one knock = one event

// ---- Motion: HC-SR501 OUT on GPIO34 (input only, no internal pulls) -------
constexpr uint8_t PIN_PIR = 34;
constexpr int PIR_ACTIVE_LEVEL = HIGH;         // HC-SR501 drives its output high on motion
constexpr uint32_t PIR_HOLD_MS = 500;          // ignore glitches shorter than this
constexpr uint32_t PIR_WARMUP_MS = 60000;
constexpr uint32_t ARM_DELAY_MS = 10000;       // sensors ignored right after locking

// ---- Buzzer and LED -------------------------------------------------------
constexpr uint8_t PIN_BUZZER = 14;
constexpr uint8_t BUZZER_LEDC_CH = 2;   // LEDC timer 1 (separate from the motor PWM)
constexpr uint8_t PIN_LED_RED = 4;
constexpr uint8_t PIN_ONBOARD_LED = 2;  // DevKit V1 blue LED, unused, for "identify"


// ---- Alarm (Q12, Q19) -----------------------------------------------------
constexpr uint32_t ALARM_SIREN_MS = 180000;
constexpr bool LINK_LOSS_IS_ALARM = true;
constexpr bool BOLT_TAMPER_IS_ALARM = true;   // bolt leaves "locked" without a command
constexpr uint32_t BOLT_TAMPER_MS = 300;
constexpr uint8_t RF_TAMPER_FAILS = 5;
constexpr uint32_t RF_TAMPER_WINDOW_MS = 10000;

// ---- Credentials and lockout (Q17) ----------------------------------------
constexpr uint8_t MAX_CREDENTIALS = 100;   // cards; all share one PIN
constexpr uint32_t PBKDF2_ITERATIONS = 4096;   // tune for about 200 ms (printed on enroll)
constexpr uint32_t LOCKOUT_THRESHOLD = 5;
constexpr uint32_t LOCKOUT_BASE_S = 30;
constexpr uint32_t LOCKOUT_MAX_S = 900;
constexpr uint32_t AUTH_MIN_INTERVAL_MS = 1000;
constexpr uint32_t ENROLL_TIMEOUT_MS = 120000;   // restarted while someone is typing a PIN

// ---- DS3231 real-time clock (I2C, optional) --------------------------------
constexpr uint8_t PIN_RTC_SDA = 21;
constexpr uint8_t PIN_RTC_SCL = 22;
constexpr uint32_t CLOCK_JUMP_TOLERANCE_S = 5;   // RTC vs uptime disagreement that gets logged

// ---- Event log (internal flash, LittleFS) ---------------------------------
constexpr uint32_t LOG_RECORDS_PER_FILE = 10000;   // 2 files x 10000 x 48 bytes = 960 KB

// ---- Serial console -------------------------------------------------------
// lock / unlock / arm / disarm / clear / resetlockout. Turn off for production.
constexpr bool CONSOLE_TEST_COMMANDS = true;
