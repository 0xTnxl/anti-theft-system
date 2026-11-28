// The door's own HTTPS identity for https://door.usezend.app.
//
// The private key is made on the door the first time it starts and never
// leaves it (NVS). The PC tool tools/renew_cert.py takes the door's
// certificate request (CSR), gets a Let's Encrypt certificate for it, and
// sends the certificate back. A certificate is public, so only the admin can
// upload one, and the door accepts it only if it matches the door's own key and
// name. Until one is installed the door keeps using the local-CA certificate
// from tools/gen_device_keys.py (https://192.168.4.1).
//
// On production boards, turn on flash encryption so the key cannot be read
// out of a stolen board (docs/DASHBOARD.md).
#pragma once
#include <Arduino.h>

namespace devcert {

constexpr const char* HOST = "door.usezend.app";
constexpr size_t MAX_CHAIN = 8192;

bool begin();                       // after the radio is on (hardware RNG): load or create the key
bool installed();                   // a valid certificate for HOST is stored
const char* certPem();              // chain, NUL-terminated (valid while installed)
const char* keyPem();
uint32_t notAfter();                // Unix seconds, 0 if none
bool staging();                     // test certificate from the Let's Encrypt staging CA
const char* issuer();               // issuer CN, e.g. "R11"
bool csrPem(String& out);           // certificate request for HOST, signed by the door's key
// Checks and stores a PEM chain. nowUnix = 0 skips the date check (no trusted clock).
// pem must be NUL-terminated at pem[len]. The new chain is used after the
// HTTPS server restarts (web.cpp does that: pending() then activate()).
bool install(const char* pem, size_t len, uint32_t nowUnix, String& msg);
bool pending();
void activate();                    // only while the HTTPS server is stopped

}  // namespace devcert
