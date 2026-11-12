// Exterior module (keypad, RC522, OLED, LEDs). Hardware Rev B, wiring unchanged.
// Defaults that still need team confirmation are marked TBC (docs/OPEN_QUESTIONS.md).
#pragma once
#include <stdint.h>

// ---- Keypad (SW1) ---------------------------------------------------------
// C4 (GPIO23) is shared with RC522 MOSI: the keypad is only scanned while SPI
// is stopped (PIN phase). See docs/DESIGN.md section 3.1.
constexpr uint8_t PIN_KP_ROWS[4] = {13, 14, 27, 26};   // R1..R4
constexpr uint8_t PIN_KP_COLS[4] = {25, 33, 32, 23};   // C1..C4
// TBC (Q4): C4 assumed to be the A/B/C/D column.
constexpr char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};
constexpr char KEY_CLEAR = '*';
constexpr char KEY_SUBMIT = '#';
constexpr uint32_t KEYPAD_SCAN_MS = 10;
constexpr uint8_t KEYPAD_DEBOUNCE_SCANS = 3;

// ---- RC522 (MOD2) ---------------------------------------------------------
constexpr uint8_t PIN_SPI_SCK = 18;
constexpr uint8_t PIN_SPI_MISO = 19;
constexpr uint8_t PIN_SPI_MOSI = 23;   // shared with keypad C4
constexpr uint8_t PIN_RC522_CS = 5;
// RC522 RST is wired to GPIO22 (OLED SCL). The firmware never drives it as RST.
constexpr uint32_t RC522_SPI_HZ = 4000000;
constexpr uint32_t RFID_POLL_MS = 200;
// Every I2C transfer pulses RST low (hard power-down). Wait this long after the
// last I2C activity before talking to the RC522 again.
constexpr uint32_t RC522_RESET_SETTLE_MS = 50;
// Ignore cards for this long after returning to the "tap card" screen.
constexpr uint32_t CARD_COOLDOWN_MS = 1500;

// ---- OLED (SSD1306 128x32, I2C) -------------------------------------------
constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;   // shared with RC522 RST
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint32_t OLED_I2C_HZ = 400000;
constexpr bool OLED_FLIP = false;     // rotate 180 degrees
constexpr uint32_t OLED_SLEEP_MS = 30000;

// ---- LEDs (active high) ---------------------------------------------------
constexpr uint8_t PIN_LED_GREEN = 4;
constexpr uint8_t PIN_LED_YELLOW = 17;
constexpr uint8_t PIN_LED_RED = 16;

// ---- UI timing ------------------------------------------------------------
constexpr uint32_t PIN_ENTRY_TIMEOUT_MS = 15000;
constexpr uint32_t VERIFY_TIMEOUT_MS = 3000;
constexpr uint32_t RESULT_OK_MS = 3000;
constexpr uint32_t RESULT_FAIL_MS = 2000;

// ---- Link -----------------------------------------------------------------
constexpr uint32_t HANDSHAKE_RETRY_MS = 3000;
// Session counts as confirmed only after the first STATUS from the interior.
constexpr uint32_t SESSION_CONFIRM_MS = 3000;

// ---- Power (AMS1117 heat mitigation, Q9) ----------------------------------
constexpr uint32_t CPU_MHZ = 80;
// Max WiFi TX power in 0.25 dBm units (34 = 8.5 dBm). 0 = SDK default.
constexpr int8_t WIFI_MAX_TX_QDBM = 34;
