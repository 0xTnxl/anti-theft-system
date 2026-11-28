#include "zend_link.h"

#include <Preferences.h>
#include <Zend.h>

#include "evlog.h"
#include "homenet.h"
#include "protocol.h"
#include "web.h"

namespace zendlink {
namespace {

constexpr uint32_t STACK = 16384;
constexpr int POLL_WAIT_S = 8;                    // long poll; queued alerts wait at most this long
constexpr uint32_t ANCHOR_EVERY_MS = 3600000UL;
constexpr uint32_t ANCHOR_SOON_MS = 60000;        // after an important event
constexpr uint32_t COMMAND_MAX_AGE_S = 120;
constexpr uint8_t ALERTS_PER_HOUR = 8;            // Zend allows 10
constexpr uint32_t SAME_ALERT_GAP_MS = 600000;
constexpr uint32_t MIN_HEAP = 70000;              // a TLS session needs ~40 KB

enum Kind : uint8_t { K_TEST, K_ALARM, K_LOCKOUT, K_FAULT, K_UPDATE, K_LOG, K_WIPE, K_COUNT };

Preferences gPrefs;
char gKey[96] = "", gSecret[96] = "";
Zend* gZend = nullptr;
SemaphoreHandle_t gMx = nullptr;      // guards the fields below (door thread <-> Zend task)
struct Alert {
    uint8_t kind;
    char text[120];
};
Alert gQueue[4];
uint8_t gQueued = 0;
uint32_t gSentAt[ALERTS_PER_HOUR] = {};   // ring of send times
uint8_t gSentIdx = 0;
uint32_t gKindAt[K_COUNT] = {};
bool gKindSeen[K_COUNT] = {};
uint32_t gAnchorDue = 0;
uint32_t gAnchoredSeq = 0;      // seq of our own ZEND_ANCHOR record (not worth anchoring again)
volatile bool gOnline = false;
volatile uint32_t gLastOkMs = 0;
volatile bool gReload = false;

struct Mx {
    Mx() { xSemaphoreTake(gMx, portMAX_DELAY); }
    ~Mx() { xSemaphoreGive(gMx); }
};

const char* alarmText(uint8_t cause) {
    switch (cause) {
        case proto::ALARM_FORCED_DOOR: return "Your door was opened while it was locked.";
        case proto::ALARM_INTRUSION: return "Strong shaking or knocking was felt at your door.";
        case proto::ALARM_TAMPER_LINK: return "The keypad outside stopped answering while the door was armed.";
        case proto::ALARM_TAMPER_RF: return "Someone may be interfering with the door's radio.";
        case proto::ALARM_BOLT_TAMPER: return "The bolt moved without anyone using a card.";
        case proto::ALARM_MOTION: return "Movement was seen inside while the door was armed.";
        default: return "The door alarm went off.";
    }
}

// Door thread: queue only.
void queueAlert(Kind k, const char* text) {
    Mx m;
    const uint32_t now = millis();
    if (gKindSeen[k] && now - gKindAt[k] < SAME_ALERT_GAP_MS) return;
    if (gQueued >= sizeof(gQueue) / sizeof(gQueue[0])) return;
    gKindSeen[k] = true;
    gKindAt[k] = now;
    gQueue[gQueued].kind = k;
    strlcpy(gQueue[gQueued].text, text, sizeof(gQueue[gQueued].text));
    gQueued++;
}

bool rateOk(uint32_t now) {
    const uint32_t oldest = gSentAt[gSentIdx];
    return !oldest || now - oldest >= 3600000UL;
}

void loadCreds() {
    gPrefs.getString("key", gKey, sizeof(gKey));
    gPrefs.getString("secret", gSecret, sizeof(gSecret));
    delete gZend;
    gZend = nullptr;
    if (gKey[0] && gSecret[0]) {
        gZend = new Zend(gKey, gSecret);
        gZend->setCursor(gPrefs.getLong("after", -1));
    }
}

void sendAlerts() {
    for (;;) {
        Alert a;
        {
            Mx m;
            if (!gQueued || !rateOk(millis())) return;
            a = gQueue[0];
            memmove(gQueue, gQueue + 1, (gQueued - 1) * sizeof(Alert));
            gQueued--;
        }
        const bool ok = gZend->alert(a.text);
        Serial.printf("[zend] alert %s: %s\n", ok ? "sent" : "FAILED", a.text);
        if (!ok) return;
        {
            Mx m;
            gSentAt[gSentIdx] = millis() | 1;
            gSentIdx = (gSentIdx + 1) % ALERTS_PER_HOUR;
        }
        web::lock();
        evlog::append(evlog::EV_ZEND_ALERT, evlog::NO_SLOT, a.kind);
        web::unlock();
    }
}

void anchor() {
    uint32_t seq;
    uint8_t tag[16];
    web::lock();
    seq = evlog::headSeq();
    evlog::headTag(tag);
    web::unlock();
    if (seq == gAnchoredSeq) return;   // nothing new since the last anchor
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", tag[i]);
    // reference "<seq>-<tag>": tools/verify_log.py compares it with the exported log.
    const String ref = String(seq) + "-" + hex;
    if (!gZend->reading("log_head", (float)seq, "", ref)) {
        Serial.printf("[zend] log anchor failed (%d)\n", gZend->lastStatus());
        return;
    }
    web::lock();
    evlog::append(evlog::EV_ZEND_ANCHOR, evlog::NO_SLOT, 0, seq);
    gAnchoredSeq = evlog::headSeq();
    web::unlock();
    Serial.printf("[zend] log anchored at seq %lu\n", (unsigned long)seq);
}

void ack(const ZendEvent& e, bool ok, const String& msg, evlog::ZendCommand c) {
    gZend->ack(e.commandId, ok, msg);
    web::lock();
    evlog::append(evlog::EV_ZEND_COMMAND, evlog::NO_SLOT, c, ok ? 1 : 0, (uint32_t)e.seq);
    web::unlock();
    Serial.printf("[zend] command %s: %s (%s)\n", e.data["name"].as<const char*>(), ok ? "done" : "refused",
                  msg.c_str());
}

void command(const ZendEvent& e) {
    const String name = e.data["name"] | "";
    const uint32_t now = homenet::unixNow();
    if (now && e.signedAt && (long)now - e.signedAt > (long)COMMAND_MAX_AGE_S) {
        ack(e, false, "That request is too old, please send it again.", evlog::ZC_STALE);
        return;
    }
    String msg;
    if (name == "lock") {
        web::lock();
        const bool ok = app::lock(msg);
        web::unlock();
        ack(e, ok, ok ? "Locking your door now." : "I couldn't lock it: " + msg, evlog::ZC_LOCK);
    } else if (name == "status") {
        web::lock();
        app::summary(msg);
        web::unlock();
        ack(e, true, msg, evlog::ZC_STATUS);
    } else {
        ack(e, false, "This door understands: lock, status. It never unlocks remotely.", evlog::ZC_UNKNOWN);
    }
}

void task(void*) {
    uint32_t lastAnchor = 0;
    bool anchoredOnce = false;
    for (;;) {
        if (gReload) {
            gReload = false;
            loadCreds();
        }
        if (!gZend || !homenet::connected() || ESP.getFreeHeap() < MIN_HEAP) {
            gOnline = false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        sendAlerts();
        const uint32_t now = millis();
        uint32_t due;
        {
            Mx m;
            due = gAnchorDue;
        }
        if (!anchoredOnce || now - lastAnchor >= ANCHOR_EVERY_MS || (due && (int32_t)(now - due) >= 0)) {
            anchor();
            anchoredOnce = true;
            lastAnchor = now;
            Mx m;
            gAnchorDue = 0;
        }
        ZendEvent e;
        const bool got = gZend->next(e, POLL_WAIT_S);
        const int st = gZend->lastStatus();
        gOnline = st == 200;
        if (gOnline) gLastOkMs = millis();
        if (st == 401 || st == 403) {
            Serial.println("[zend] Zend refused the device key: was this door removed in the Zend app? ('zend off')");
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        if (!got) {
            if (!gOnline) vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        gPrefs.putLong("after", gZend->cursor());   // never act on the same event twice
        if (e.type == "command") command(e);
        else if (e.type == "test") Serial.println("[zend] test event received from Zend");
    }
}

}  // namespace

void begin() {
    gMx = xSemaphoreCreateMutex();
    gPrefs.begin("atds_zend", false);
    loadCreds();
    evlog::setObserver(onEvent);
    xTaskCreatePinnedToCore(task, "zend", STACK, nullptr, 1, nullptr, 0);
    Serial.println(gZend ? "[zend] paired: alerts, remote lock and log anchoring on (needs home WiFi)"
                         : "[zend] not paired ('zend key ...' and 'zend secret ...')");
}

bool paired() { return gKey[0] && gSecret[0]; }
bool online() { return gOnline && homenet::connected(); }

bool setKey(const char* key) {
    if (strncmp(key, "zdevice_", 8) || strlen(key) >= sizeof(gKey)) return false;
    gPrefs.putString("key", key);
    gPrefs.remove("after");
    gReload = true;
    return true;
}

bool setSecret(const char* secret) {
    if (strncmp(secret, "zdsig_", 6) || strlen(secret) >= sizeof(gSecret)) return false;
    gPrefs.putString("secret", secret);
    gReload = true;
    return true;
}

void forget() {
    gPrefs.clear();
    gKey[0] = gSecret[0] = 0;
    gReload = true;
}

void testAlert() { queueAlert(K_TEST, "Hello from your door. Alerts are working."); }

void printStatus() {
    Serial.printf("Zend: %s, %s", paired() ? "paired" : "not paired",
                  !homenet::connected() ? "no home WiFi" : online() ? "online" : "not reachable");
    if (gLastOkMs) Serial.printf(", last contact %lu s ago", (unsigned long)((millis() - gLastOkMs) / 1000));
    Serial.printf(", %u alert(s) waiting\n", gQueued);
}

void statusJson(String& out) {
    out += String("\"zend\":{\"paired\":") + (paired() ? "true" : "false") + ",\"online\":" +
           (online() ? "true" : "false") + "}";
}

void onEvent(uint8_t ev, uint8_t detail, uint32_t arg1) {
    if (!gMx) return;
    char b[120];
    switch (ev) {
        case evlog::EV_ALARM: queueAlert(K_ALARM, alarmText(detail)); break;
        case evlog::EV_LOCKOUT:
            snprintf(b, sizeof(b), "Several wrong PINs were typed at your door. The keypad is paused for %lu s.",
                     (unsigned long)arg1);
            queueAlert(K_LOCKOUT, b);
            break;
        case evlog::EV_FAULT: queueAlert(K_FAULT, "The lock had trouble moving. Please check the door."); break;
        case evlog::EV_OTA_ROLLED_BACK:
        case evlog::EV_OTA_FAIL: queueAlert(K_UPDATE, "A software update for your door did not go through."); break;
        case evlog::EV_LOG_ROLLBACK: queueAlert(K_LOG, "Your door's history looks shorter than it should. Please check it."); break;
        case evlog::EV_WIPE: queueAlert(K_WIPE, "All cards and the PIN were removed from your door."); break;
        default: break;
    }
    switch (ev) {   // worth anchoring soon
        case evlog::EV_GRANTED:
        case evlog::EV_ALARM:
        case evlog::EV_ALARM_CLEARED:
        case evlog::EV_CARD_ADDED:
        case evlog::EV_CARD_REMOVED:
        case evlog::EV_PIN_CHANGED:
        case evlog::EV_WIPE:
        case evlog::EV_TIME_SET: {
            Mx m;
            if (!gAnchorDue) gAnchorDue = (millis() + ANCHOR_SOON_MS) | 1;
            break;
        }
        default: break;
    }
}

}  // namespace zendlink
