#include "espnow_transport.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

namespace espnow {
namespace {

QueueHandle_t gQueue = nullptr;
uint8_t gPeer[6];
volatile uint32_t gForeign = 0;
volatile uint32_t gOverflow = 0;

// Runs in the WiFi task: keep it short.
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || memcmp(mac, gPeer, 6) != 0) {
        gForeign = gForeign + 1;
        return;
    }
    if (!data || len <= 0 || len > (int)sizeof(Packet::data)) return;
    Packet p;
    p.len = (uint8_t)len;
    memcpy(p.data, data, len);
    if (xQueueSend(gQueue, &p, 0) != pdTRUE) gOverflow = gOverflow + 1;
}

}  // namespace

bool begin(const uint8_t myMac[6], const uint8_t peerMac[6], uint8_t channel, int8_t maxTxQdBm) {
    memcpy(gPeer, peerMac, 6);
    gQueue = xQueueCreate(8, sizeof(Packet));
    if (!gQueue) return false;

    // Arduino-ESP32 2.x resets the base MAC to the factory value inside
    // WiFi.mode() (tcpipInit), so esp_base_mac_addr_set() does not stick.
    // Set the STA interface MAC directly once WiFi is running instead.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    const esp_err_t sm = esp_wifi_set_mac(WIFI_IF_STA, myMac);

    uint8_t actual[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, actual);
    if (sm != ESP_OK || memcmp(actual, myMac, 6) != 0) {
        Serial.printf("[espnow] could not set STA MAC from secrets.h (err 0x%x), have "
                      "%02x:%02x:%02x:%02x:%02x:%02x\n",
                      sm, actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
        return false;
    }
    Serial.printf("[espnow] STA MAC %02x:%02x:%02x:%02x:%02x:%02x, channel %u\n", actual[0],
                  actual[1], actual[2], actual[3], actual[4], actual[5], channel);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (maxTxQdBm > 0) esp_wifi_set_max_tx_power(maxTxQdBm);

    if (esp_now_init() != ESP_OK) return false;
    if (esp_now_register_recv_cb(onRecv) != ESP_OK) return false;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peerMac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;   // payload security is done by secure_link (AES-256-GCM)
    return esp_now_add_peer(&peer) == ESP_OK;
}

bool send(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > ESP_NOW_MAX_DATA_LEN) return false;
    return esp_now_send(gPeer, data, len) == ESP_OK;
}

bool receive(Packet& out) {
    return gQueue && xQueueReceive(gQueue, &out, 0) == pdTRUE;
}

uint32_t foreignFrames() { return gForeign; }
uint32_t queueOverflows() { return gOverflow; }

}  // namespace espnow
