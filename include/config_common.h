// Settings shared by both modules.
#pragma once
#include <stdint.h>

// ESP-NOW radio channel. Both boards must match.
constexpr uint8_t ESPNOW_CHANNEL = 1;

// Exterior sends HEARTBEAT this often; interior answers with STATUS.
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 2000;

// Exterior: no frame from interior for this long -> drop session, re-handshake.
constexpr uint32_t EXT_LINK_TIMEOUT_MS = 7000;

// Interior: no frame from exterior for this long while armed -> TAMPER_LINK.
constexpr uint32_t INT_LINK_TIMEOUT_MS = 10000;
