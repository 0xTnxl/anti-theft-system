// Application messages carried inside the secure link (docs/DESIGN.md 4.4).
// All multi-byte fields are little endian (both ends are ESP32).
// Every type has one fixed payload size, so frame length leaks nothing
// (for example PIN length).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace proto {

enum MsgType : uint8_t {
    AUTH_REQUEST = 0x20,   // exterior -> interior
    AUTH_RESULT = 0x21,    // interior -> exterior
    HEARTBEAT = 0x30,      // exterior -> interior
    STATUS = 0x31,         // interior -> exterior
};

constexpr size_t UID_MAX = 10;
constexpr size_t PIN_MIN = 4;
constexpr size_t PIN_MAX = 8;

enum AuthCode : uint8_t {
    AUTH_GRANTED = 1,
    AUTH_DENIED = 2,
    AUTH_LOCKED_OUT = 3,
    AUTH_FAULT = 4,
    AUTH_BUSY = 5,
    AUTH_ENROLL_AGAIN = 6,   // enroll step 1 stored: tap the same card, same PIN again
    AUTH_ENROLL_OK = 7,
    AUTH_ENROLL_FAIL = 8,
};

enum LockState : uint8_t {
    LOCK_UNKNOWN = 0,
    LOCK_LOCKED = 1,
    LOCK_UNLOCKING = 2,
    LOCK_UNLOCKED = 3,
    LOCK_LOCKING = 4,
    LOCK_FAULT = 5,
};

enum AlarmCause : uint8_t {
    ALARM_NONE = 0,
    ALARM_FORCED_DOOR = 1,   // door opened while bolt locked
    ALARM_INTRUSION = 2,     // SW-420 vibration (GPIO32)
    ALARM_TAMPER_LINK = 3,   // exterior silent while armed
    ALARM_TAMPER_RF = 4,     // repeated authentication failures on the radio link
    ALARM_BOLT_TAMPER = 5,   // bolt left "locked" without a command
    ALARM_MOTION = 6,        // HC-SR501 PIR (GPIO34)
};

enum StatusFlags : uint8_t {
    STATUS_ARMED = 0x01,
    STATUS_DISARMED = 0x02,   // alarms suppressed (serial "disarm")
    STATUS_ENROLL = 0x04,
    STATUS_DOOR_OPEN = 0x08,
};

struct __attribute__((packed)) AuthRequest {
    uint32_t reqId;
    uint8_t uidLen;
    uint8_t uid[UID_MAX];   // zero padded
    uint8_t pinLen;
    char pin[PIN_MAX];      // ASCII digits, zero padded, not terminated
};

struct __attribute__((packed)) AuthResult {
    uint32_t reqId;
    uint8_t code;           // AuthCode
    uint8_t attemptsLeft;   // before lockout
    uint16_t lockoutS;      // remaining lockout seconds when code == AUTH_LOCKED_OUT
};

struct __attribute__((packed)) Heartbeat {
    uint32_t uptimeS;
    uint8_t extState;
    uint8_t reserved[3];
};

struct __attribute__((packed)) Status {
    uint8_t lock;    // LockState
    uint8_t alarm;   // AlarmCause
    uint8_t flags;   // StatusFlags
    uint8_t reserved;
};

static_assert(sizeof(AuthRequest) == 24, "AuthRequest layout");
static_assert(sizeof(AuthResult) == 8, "AuthResult layout");
static_assert(sizeof(Heartbeat) == 8, "Heartbeat layout");
static_assert(sizeof(Status) == 4, "Status layout");

// Exact payload size for a type, 0 for unknown types.
inline size_t payloadSize(uint8_t type) {
    switch (type) {
        case AUTH_REQUEST: return sizeof(AuthRequest);
        case AUTH_RESULT: return sizeof(AuthResult);
        case HEARTBEAT: return sizeof(Heartbeat);
        case STATUS: return sizeof(Status);
        default: return 0;
    }
}

}  // namespace proto
