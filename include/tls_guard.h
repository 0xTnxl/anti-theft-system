// Pulls in the generated HTTPS certificate and OTA public key, with a clear
// error if they were never generated.
#pragma once
#if __has_include("tls_secrets.h") && __has_include("ota_pubkey.h")
#include "ota_pubkey.h"
#include "tls_secrets.h"
#else
#error "HTTPS / OTA keys missing. Run:  python3 tools/gen_device_keys.py"
#endif
