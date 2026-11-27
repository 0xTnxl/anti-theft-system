#include "web.h"

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

#include "bootloader_random.h"
#include "evlog.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "rtc.h"
#include "web_page.h"

namespace web {
namespace {

constexpr const char* SSID = "ATDS-Door";
constexpr uint32_t ADMIN_ITER = 1000;          // ~115 ms per login check
constexpr uint32_t SESSION_MS = 12UL * 3600 * 1000;
constexpr uint8_t MAX_SESSIONS = 4;
constexpr uint8_t LOGIN_MAX_FAILS = 5;
constexpr uint32_t LOGIN_BLOCK_MS = 60000;
constexpr uint32_t PHONE_MIN_INTERVAL_MS = 2000;

WebServer gServer(80);
Preferences gPrefs;
char gApPass[33] = "";
char gInitPass[17] = "";   // generated admin password, shown by 'wifi' until changed

struct __attribute__((packed)) AdminHash {
    uint8_t salt[16];
    uint8_t hash[32];
} gAdmin = {};
bool gAdminSet = false;

struct Session {
    uint8_t token[16];
    uint32_t expires;
    bool used;
} gSessions[MAX_SESSIONS] = {};

uint8_t gLoginFails = 0;
uint32_t gLoginBlockUntil = 0;
uint32_t gLastPhoneMs = 0;
bool gPhoneSeen = false;
bool gStarted = false;

// ------------------------------------------------------------- helpers ---

void randomPassword(char* out, size_t len) {
    static const char A[] = "abcdefghjkmnpqrstuvwxyzACDEFGHJKLMNPQRTUVWXY3479";   // no look-alikes
    for (size_t i = 0; i < len; i++) out[i] = A[esp_random() % (sizeof(A) - 1)];
    out[len] = 0;
}

bool pbkdf2(const char* pass, const uint8_t salt[16], uint8_t out[32]) {
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    const bool ok = mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
                    mbedtls_pkcs5_pbkdf2_hmac(&c, (const unsigned char*)pass, strlen(pass), salt, 16, ADMIN_ITER, 32,
                                              out) == 0;
    mbedtls_md_free(&c);
    return ok;
}

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

void storeAdmin(const char* pass) {
    esp_fill_random(gAdmin.salt, sizeof(gAdmin.salt));
    pbkdf2(pass, gAdmin.salt, gAdmin.hash);
    gPrefs.putBytes("admin", &gAdmin, sizeof(gAdmin));
    gAdminSet = true;
}

bool checkAdmin(const char* pass) {
    if (!gAdminSet) return false;
    uint8_t h[32];
    const bool ok = pbkdf2(pass, gAdmin.salt, h) && ctEqual(h, gAdmin.hash, 32);
    mbedtls_platform_zeroize(h, sizeof(h));
    return ok;
}

void toHex(const uint8_t* d, size_t n, char* out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[2 * n] = 0;
}

bool fromHex(const char* s, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 2; k++) {
            const char c = s[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else return false;
        }
        out[i] = v;
    }
    return true;
}

// ------------------------------------------------------------ sessions ---

bool loggedIn() {
    const String c = gServer.header("Cookie");
    const int i = c.indexOf("atds=");
    if (i < 0 || c.length() < (unsigned)i + 5 + 32) return false;
    uint8_t tok[16];
    if (!fromHex(c.c_str() + i + 5, tok, 16)) return false;
    const uint32_t now = millis();
    bool ok = false;
    for (Session& s : gSessions) {
        if (!s.used) continue;
        if ((int32_t)(s.expires - now) <= 0) {
            s.used = false;
            continue;
        }
        ok |= ctEqual(s.token, tok, 16);
    }
    return ok;
}

void newSession() {
    Session* slot = &gSessions[0];
    for (Session& s : gSessions) {
        if (!s.used) {
            slot = &s;
            break;
        }
        if ((int32_t)(s.expires - slot->expires) < 0) slot = &s;   // replace the oldest
    }
    esp_fill_random(slot->token, 16);
    slot->expires = millis() + SESSION_MS;
    slot->used = true;
    char hex[33];
    toHex(slot->token, 16, hex);
    gServer.sendHeader("Set-Cookie", String("atds=") + hex + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200");
}

// ------------------------------------------------------------ responses ---

void common() {
    gServer.sendHeader("Cache-Control", "no-store");
    gServer.sendHeader("X-Content-Type-Options", "nosniff");
    gServer.sendHeader("X-Frame-Options", "DENY");
    gServer.sendHeader("Referrer-Policy", "no-referrer");
}

void json(int code, const String& body) {
    common();
    gServer.send(code, "application/json", body);
}

void result(bool ok, const String& msg) {
    String m = msg;
    m.replace("\\", "\\\\");
    m.replace("\"", "\\\"");
    json(ok ? 200 : 400, String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + m + "\"}");
}

// POSTs must carry X-ATDS: 1. Browsers cannot add that header to a cross-site
// form post, so this (with SameSite=Strict cookies) blocks request forgery.
bool postOk() {
    if (gServer.header("X-ATDS") != "1") {
        result(false, "missing request header");
        return false;
    }
    return true;
}

bool requireAdmin() {
    if (loggedIn()) return true;
    json(401, "{\"ok\":false,\"msg\":\"login required\"}");
    return false;
}

// ------------------------------------------------------------- handlers ---

void hPage() {
    common();
    gServer.sendHeader("Content-Security-Policy",
                       "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'");
    gServer.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

void hMe() { json(200, String("{\"admin\":") + (loggedIn() ? "true" : "false") + "}"); }

void hLogin() {
    if (!postOk()) return;
    const uint32_t now = millis();
    if ((int32_t)(gLoginBlockUntil - now) > 0) {
        result(false, "too many attempts, wait " + String((gLoginBlockUntil - now) / 1000 + 1) + " s");
        return;
    }
    const String p = gServer.arg("password");
    if (p.length() && p.length() <= 64 && checkAdmin(p.c_str())) {
        gLoginFails = 0;
        newSession();
        evlog::append(evlog::EV_WEB_LOGIN);
        Serial.println("[web] admin logged in");
        result(true, "ok");
        return;
    }
    gLoginFails++;
    evlog::append(evlog::EV_WEB_LOGIN_FAIL, evlog::NO_SLOT, 0, gLoginFails);
    Serial.printf("[web] admin login failed (%u)\n", gLoginFails);
    if (gLoginFails >= LOGIN_MAX_FAILS) {
        gLoginFails = 0;
        gLoginBlockUntil = now + LOGIN_BLOCK_MS;
    }
    result(false, "wrong password");
}

void hLogout() {
    if (!postOk()) return;
    for (Session& s : gSessions) s.used = false;   // single-door system: log every device out
    gServer.sendHeader("Set-Cookie", "atds=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    result(true, "logged out");
}

void hPublic() {
    String o;
    app::publicJson(o);
    json(200, o);
}

void hPhonePin() {
    if (!postOk()) return;
    const uint32_t now = millis();
    if (gPhoneSeen && now - gLastPhoneMs < PHONE_MIN_INTERVAL_MS) {
        result(false, "slow down");
        return;
    }
    gPhoneSeen = true;
    gLastPhoneMs = now;
    String pin = gServer.arg("pin");
    String msg;
    const bool ok = app::setPhonePin(pin.c_str(), msg);
    for (unsigned i = 0; i < pin.length(); i++) pin.setCharAt(i, '0');   // overwrite the copy
    result(ok, msg);
}

void hStatus() {
    if (!requireAdmin()) return;
    String o;
    app::statusJson(o);
    json(200, o);
}

void hCards() {
    if (!requireAdmin()) return;
    String o;
    app::cardsJson(o);
    json(200, o);
}

struct EvCtx {
    String* out;
    bool first;
};

void evToJson(const evlog::Record& r, void* p) {
    EvCtx* c = (EvCtx*)p;
    char b[200];
    snprintf(b, sizeof(b),
             "%s{\"seq\":%lu,\"t\":%lu,\"st\":%u,\"boot\":%lu,\"up\":%lu,\"ev\":\"%s\",\"slot\":%d,\"d\":%u,\"a1\":%lu,\"a2\":%lu}",
             c->first ? "" : ",", (unsigned long)r.body.seq, (unsigned long)r.body.rtc, r.body.rtcState,
             (unsigned long)r.body.boot, (unsigned long)(r.body.uptimeMs / 1000), evlog::eventName(r.body.event),
             r.body.slot == evlog::NO_SLOT ? -1 : r.body.slot, r.body.detail, (unsigned long)r.body.arg1,
             (unsigned long)r.body.arg2);
    *c->out += b;
    c->first = false;
}

void hEvents() {
    if (!requireAdmin()) return;
    const int n = gServer.hasArg("n") ? constrain(gServer.arg("n").toInt(), 1, 200) : 50;
    String o;
    o.reserve(n * 140 + 64);
    o = "{\"head\":" + String(evlog::headSeq()) + ",\"events\":[";
    EvCtx c = {&o, true};
    evlog::forEach(n, evToJson, &c);
    o += "]}";
    json(200, o);
}

void hVerify() {
    if (!requireAdmin()) return;
    uint32_t n, bad, back;
    const bool ok = evlog::verify(&n, &bad, &back);
    json(200, String("{\"ok\":") + (ok ? "true" : "false") + ",\"checked\":" + n + ",\"bad\":" + bad +
                  ",\"backwards\":" + back + "}");
}

void hAction() {
    if (!postOk() || !requireAdmin()) return;
    const String a = gServer.arg("a");
    String msg;
    bool ok = false;
    uint8_t code = 0;
    if (a == "lock") { code = evlog::WA_LOCK; ok = app::lock(msg); }
    else if (a == "clear") { code = evlog::WA_CLEAR; ok = app::clearAlarm(msg); }
    else if (a == "resetlockout") { code = evlog::WA_RESET_LOCKOUT; ok = app::resetLockout(msg); }
    else if (a == "enroll") { code = evlog::WA_ENROLL; ok = app::startEnroll(msg); }
    else if (a == "setpin") { code = evlog::WA_SETPIN; ok = app::startSetPin(msg); }
    else if (a == "cancel") { code = evlog::WA_CANCEL; ok = app::cancelAdmin(msg); }
    else if (a == "delete") { code = evlog::WA_DELETE; ok = app::deleteCard(gServer.arg("slot").toInt(), msg); }
    else if (a == "time") {
        code = evlog::WA_TIME;
        const long t = gServer.arg("t").toInt();
        ok = t > 1700000000L && app::setTime((uint32_t)t, msg);
        if (t <= 1700000000L) msg = "bad time";
    } else if (a == "password") {
        code = evlog::WA_PASSWORD;
        const String oldp = gServer.arg("old"), newp = gServer.arg("new");
        if (!checkAdmin(oldp.c_str())) msg = "current password is wrong";
        else if (newp.length() < 8 || newp.length() > 64) msg = "new password must be 8-64 characters";
        else {
            storeAdmin(newp.c_str());
            gInitPass[0] = 0;
            gPrefs.remove("init");
            ok = true;
            msg = "password changed";
        }
    } else {
        result(false, "unknown action");
        return;
    }
    evlog::append(evlog::EV_WEB_ACTION, evlog::NO_SLOT, code, ok ? 1 : 0);
    Serial.printf("[web] action %s: %s\n", a.c_str(), msg.c_str());
    result(ok, msg);
}

void hNotFound() {
    common();
    gServer.send(404, "text/plain", "not found");
}

}  // namespace

void loadCredentials() {
    gPrefs.begin("atds_web", false);
    // The radio is not running yet, so enable the bootloader entropy source
    // for the hardware RNG while generating first-boot passwords.
    bootloader_random_enable();
    if (gPrefs.getString("ap", gApPass, sizeof(gApPass)) < 8) {
        randomPassword(gApPass, 12);
        gPrefs.putString("ap", gApPass);
    }
    if (gPrefs.getBytesLength("admin") == sizeof(gAdmin)) {
        gPrefs.getBytes("admin", &gAdmin, sizeof(gAdmin));
        gAdminSet = true;
        gPrefs.getString("init", gInitPass, sizeof(gInitPass));
    } else {
        randomPassword(gInitPass, 10);
        storeAdmin(gInitPass);
        gPrefs.putString("init", gInitPass);
    }
    bootloader_random_disable();
}

const char* apSsid() { return SSID; }
const char* apPass() { return gApPass; }

void begin() {
    static const char* hdrs[] = {"Cookie", "X-ATDS"};
    gServer.collectHeaders(hdrs, 2);
    gServer.on("/", HTTP_GET, hPage);
    gServer.on("/api/me", HTTP_GET, hMe);
    gServer.on("/api/login", HTTP_POST, hLogin);
    gServer.on("/api/logout", HTTP_POST, hLogout);
    gServer.on("/api/public", HTTP_GET, hPublic);
    gServer.on("/api/phonepin", HTTP_POST, hPhonePin);
    gServer.on("/api/status", HTTP_GET, hStatus);
    gServer.on("/api/cards", HTTP_GET, hCards);
    gServer.on("/api/events", HTTP_GET, hEvents);
    gServer.on("/api/verify", HTTP_GET, hVerify);
    gServer.on("/api/action", HTTP_POST, hAction);
    gServer.onNotFound(hNotFound);
    gServer.begin();
    gStarted = true;
    Serial.println("[web] dashboard ready. Type 'wifi' for the WiFi and admin passwords");
}

void service(bool busy) {
    if (gStarted && !busy) gServer.handleClient();
}

void printCredentials() {
    Serial.printf("WiFi network : %s\n", SSID);
    Serial.printf("WiFi password: %s\n", gApPass);
    Serial.printf("Dashboard    : http://%s\n", WiFi.softAPIP().toString().c_str());
    if (gInitPass[0]) Serial.printf("Admin password (first-boot, change it in the dashboard): %s\n", gInitPass);
    else Serial.println("Admin password: changed by the user (reset with 'webpass <new>')");
    Serial.printf("Clients connected: %u\n", WiFi.softAPgetStationNum());
}

bool setApPass(const char* p) {
    if (strlen(p) < 8 || strlen(p) > 32) return false;
    strncpy(gApPass, p, sizeof(gApPass) - 1);
    gPrefs.putString("ap", gApPass);
    return true;
}

bool setAdminPass(const char* p) {
    if (strlen(p) < 8 || strlen(p) > 64) return false;
    storeAdmin(p);
    gInitPass[0] = 0;
    gPrefs.remove("init");
    for (Session& s : gSessions) s.used = false;
    return true;
}

}  // namespace web
