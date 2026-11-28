// Which boards can talk to Zend directly, and the TLS client each one uses.
//
//   ESP32 family            WiFiClientSecure (mbedTLS), with the roots in ZendRoots.h
//   ESP8266                 BearSSL::WiFiClientSecure, with the roots in ZendRoots.h (needs the
//                           time, so the library sets it from NTP on first use)
//   Arduino UNO R4 WiFi     WiFiS3's WiFiSSLClient (roots in the board's WiFi chip)
//   MKR WiFi 1010, Nano 33 IoT, Nano RP2040 Connect, MKR Vidor 4000
//                           WiFiNINA's WiFiSSLClient (roots in the NINA firmware)
//
// Every other board (Uno, Nano, Mega, Leonardo, …) has no network: use ZendSerial.h and a bridge.
#pragma once

#if defined(ARDUINO_ARCH_ESP32)
#define ZEND_HAS_NETWORK 1
#define ZEND_PLATFORM "esp32"
#define ZEND_MAX_BODY 16384
#define ZEND_EVENT_BATCH 5
#elif defined(ARDUINO_ARCH_ESP8266)
#define ZEND_HAS_NETWORK 1
#define ZEND_PLATFORM "esp8266"
#define ZEND_MAX_BODY 4096
#define ZEND_EVENT_BATCH 1
#elif defined(ARDUINO_UNOR4_WIFI)
#define ZEND_HAS_NETWORK 1
#define ZEND_PLATFORM "uno-r4-wifi"
#define ZEND_MAX_BODY 4096
#define ZEND_EVENT_BATCH 1
#elif defined(ARDUINO_SAMD_MKRWIFI1010) || defined(ARDUINO_SAMD_NANO_33_IOT) || \
    defined(ARDUINO_NANO_RP2040_CONNECT) || defined(ARDUINO_SAMD_MKRVIDOR4000)
#define ZEND_HAS_NETWORK 1
#define ZEND_PLATFORM "wifinina"
#define ZEND_MAX_BODY 4096
#define ZEND_EVENT_BATCH 1
#else
#define ZEND_HAS_NETWORK 0
#endif
