// Firmware version. Bump it for every OTA release: the door refuses signed
// updates with a lower version than the one it runs (no downgrade attacks).
// tools/sign_firmware.py reads it from here.
#pragma once
#define ATDS_FW_VERSION 1
