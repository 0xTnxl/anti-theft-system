// The door on Zend (lib/Zend, from zendfi/zend-devices). Needs the home WiFi
// (homenet.h) and the device key + signing secret from `zend device pair`.
//
//   alerts    alarms, lockouts, faults and failed updates are pushed to the
//             owner's phone (max 8 an hour, the same kind at most every 10 min)
//   commands  from the owner's Zend app (Settings > Devices > Send a command):
//               lock     locks the door (same rules as the dashboard)
//               status   answers with a short sentence about the door
//             There is no unlock command, and there will not be one.
//             A command signed more than 2 minutes ago is refused.
//   anchor    the event log's newest chain tag is sent as a signed reading
//             ("log_head") every hour and shortly after important events, so
//             history before the last anchor cannot be rewritten even by
//             someone holding the log key.
//
// Zend calls block (TLS + long poll), so they run in their own task. Door
// actions are taken under web::lock(), like the dashboard's.
#pragma once
#include <Arduino.h>

namespace zendlink {

void begin();                 // after homenet::begin()
bool paired();
bool online();                // last call to Zend succeeded
void printStatus();
void statusJson(String& out); // fragment: "zend":{...}
// Console: zend key <zdevice_...> | zend secret <zdsig_...> | zend off | zend test
bool setKey(const char* key);
bool setSecret(const char* secret);
void forget();
void testAlert();
// Called by evlog after every record (door thread, lock held): never blocks.
void onEvent(uint8_t event, uint8_t detail, uint32_t arg1);

}  // namespace zendlink
