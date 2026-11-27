// Signed OTA updates (format: tools/sign_firmware.py).
//
// Order of checks: header magic, target, version (no downgrade), length,
// ECDSA P-256 signature over the header (before any flash write), then the
// SHA-256 of the image while it streams into the inactive OTA slot. Only a
// fully verified image is marked bootable. After the reboot the new firmware
// must call confirm() once it has run healthily, otherwise the bootloader
// rolls back to the previous version on the next reset.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ota {

constexpr size_t HEADER_LEN = 160;

struct Session;   // one upload at a time

// Returns nullptr and sets *err on rejection (nothing written).
Session* begin(const uint8_t header[HEADER_LEN], size_t bodyLen, const char** err, uint32_t* version);
bool write(Session* s, const uint8_t* data, size_t len, const char** err);
bool finish(Session* s, const char** err);   // verifies the hash, sets the boot slot, frees s
void abort(Session* s);

// Boot-time bookkeeping.
bool pendingVerify();     // running a freshly installed image that is not confirmed yet
void confirm();           // mark the running image good (cancels rollback)
bool rolledBack(uint32_t* failedVersion);   // the last update did not survive and was undone

}  // namespace ota
