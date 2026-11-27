// Tamper-evident event log on internal flash (LittleFS, "spiffs" partition).
//
// Each 48-byte record = 32-byte body + 16-byte tag, where
//   tag = HMAC-SHA256(logKey, previous tag | body)[0..15]
//   logKey = HMAC-SHA256(PSK, "ATDS log key v1")
// so changing, inserting or removing any record breaks the chain. The newest
// (seq, tag) is also kept in NVS, which catches a truncated or rolled-back file.
// Only the interior writes records, from its own decisions; nothing received
// over the radio is logged as-is. Two files of LOG_RECORDS_PER_FILE records
// form a ring: when both are full the older file is dropped and an NVS
// "anchor" remembers where the retained chain starts.
//
// Time: each record carries the DS3231 time (if fitted), the boot counter and
// the uptime. Clock jumps, an RTC that lost power, and console time changes
// are logged as their own events. See docs/DESIGN.md section 5.1.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace evlog {

enum Event : uint8_t {
    EV_BOOT = 1,             // arg1 = reset reason
    EV_GRANTED = 2,          // slot = card
    EV_DENIED_PIN = 3,       // slot = card
    EV_DENIED_CARD = 4,
    EV_DENIED_NO_PIN = 5,    // no shared PIN set yet
    EV_LOCKOUT = 6,          // arg1 = seconds
    EV_LOCK_REQUEST = 7,     // slot = card; detail = 1 accepted, 0 refused (arg1 = reason)
    EV_LOCK_DENIED_CARD = 8,
    EV_LOCKED = 9,           // bolt reached "locked"
    EV_UNLOCKED = 10,        // bolt reached "latch held"
    EV_DOOR_OPEN = 11,
    EV_DOOR_CLOSED = 12,
    EV_ALARM = 13,           // detail = AlarmCause
    EV_ALARM_CLEARED = 14,
    EV_FAULT = 15,           // arg1 = encoder counts of the move
    EV_CARD_ADDED = 16,      // slot
    EV_CARD_REMOVED = 17,    // slot
    EV_PIN_CHANGED = 18,
    EV_WIPE = 19,
    EV_TIME_SET = 20,        // arg1 = old time, arg2 = new time
    EV_CLOCK_JUMP = 21,      // arg1 = expected time, arg2 = RTC time
    EV_CLOCK_BACKWARDS = 22, // at boot: arg1 = last logged time, arg2 = RTC time
    EV_TIME_INVALID = 23,    // RTC lost power (oscillator-stop flag)
    EV_LOG_ROLLBACK = 24,    // at boot: file ends before the NVS head; arg1 = file seq, arg2 = head seq
    EV_LOG_REPAIRED = 25,    // at boot: partial record removed after a power cut
    EV_LOCKOUT_RESET = 26,   // console
    EV_LATCH_RELEASED = 27,  // detail = 1 door opened, 0 hold timed out
    EV_WEB_LOGIN = 28,       // dashboard admin login
    EV_WEB_LOGIN_FAIL = 29,  // arg1 = consecutive failures
    EV_PHONE_PIN = 30,       // a PIN was entered on the dashboard (not its value)
    EV_WEB_ACTION = 31,      // detail = WebAction
    EV_OTA_START = 32,       // arg1 = version being installed
    EV_OTA_INSTALLED = 33,   // arg1 = version, restart follows
    EV_OTA_FAIL = 34,        // detail 1 = rejected (signature/version/target), 2 = transfer/verify; arg1 = version
    EV_OTA_CONFIRMED = 35,   // new firmware ran healthily; arg1 = version
    EV_OTA_ROLLED_BACK = 36, // the update did not survive and the old firmware returned; arg1 = failed version
};

enum RtcState : uint8_t { RTC_NONE = 0, RTC_OK = 1, RTC_LOST_POWER = 2, RTC_ERROR = 3 };

struct __attribute__((packed)) Body {
    uint32_t seq;
    uint32_t boot;
    uint32_t uptimeMs;
    uint32_t rtc;        // Unix seconds (UTC), 0 if unknown
    uint8_t rtcState;
    uint8_t event;
    uint8_t slot;        // card slot, 0xFF = none
    uint8_t detail;
    uint32_t arg1;
    uint32_t arg2;
    uint8_t reserved[4];
};
struct __attribute__((packed)) Record {
    Body body;
    uint8_t tag[16];
};
static_assert(sizeof(Body) == 32, "log body");
static_assert(sizeof(Record) == 48, "log record");

constexpr uint8_t NO_SLOT = 0xFF;

enum WebAction : uint8_t { WA_LOCK = 1, WA_CLEAR = 2, WA_RESET_LOCKOUT = 3, WA_ENROLL = 4, WA_SETPIN = 5,
                           WA_DELETE = 6, WA_TIME = 7, WA_PASSWORD = 8, WA_CANCEL = 9,
                           WA_NAME = 10 };

// Newest `lastN` records, oldest first.
typedef void (*RecordFn)(const Record& r, void* ctx);
void forEach(uint32_t lastN, RecordFn fn, void* ctx);

bool begin(const uint8_t psk[32]);   // mounts the filesystem, checks the head, logs EV_BOOT
void service(uint32_t nowMs);         // periodic clock-jump check
bool append(Event e, uint8_t slot = NO_SLOT, uint8_t detail = 0, uint32_t arg1 = 0, uint32_t arg2 = 0);

bool setTime(uint32_t unixS);         // console only: sets the RTC, logs EV_TIME_SET
uint32_t now(uint8_t* state);         // current RTC time as logged

uint32_t count();                     // retained records
uint32_t headSeq();
uint32_t bootCount();                 // this power-up's boot number
// Walks the whole retained chain. Returns true if it is intact. *checked =
// records verified, *badSeq = first failing seq (0 if none), *backwards =
// records whose RTC time went backwards.
bool verify(uint32_t* checked, uint32_t* badSeq, uint32_t* backwards);
void dump();                          // machine-readable, for tools/verify_log.py
void show(uint32_t last);             // human-readable, newest last
const char* eventName(uint8_t e);

}  // namespace evlog
