#include "web.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include <esp_random.h>
#include <string.h>

#include "bootloader_random.h"
#include "config_interior.h"
#include "credentials.h"
#include "devcert.h"
#include "evlog.h"
#include "homenet.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "ota.h"
#include "tls_guard.h"
#include "version.h"
#include "web_page.h"
#include "zend_link.h"

namespace web {
namespace {

constexpr const char* SSID = "ATDS-Door";
constexpr uint32_t ADMIN_ITER = 1000;          // ~115 ms per login check
constexpr uint32_t SESSION_MS = 12UL * 3600 * 1000;
constexpr uint8_t MAX_SESSIONS = 4;
constexpr uint8_t LOGIN_MAX_FAILS = 5;
constexpr uint32_t LOGIN_BLOCK_MS = 60000;
constexpr uint32_t PHONE_MIN_INTERVAL_MS = 2000;
constexpr size_t MAX_FORM = 2048;

httpd_handle_t gHttps = nullptr;
WebServer gSetup(80);   // plain HTTP: setup page and ca.crt only
DNSServer gDns;         // answers door.usezend.app with this door (phones have no internet here)
uint32_t gCertAt = 0;   // when a new certificate was installed (server restarts shortly after)
SemaphoreHandle_t gLock = nullptr;
Preferences gPrefs;
char gApPass[33] = "";
char gInitPass[17] = "";   // generated admin password, shown by 'wifi' until changed
volatile bool gReboot = false;

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

struct Guard {
    Guard() { lock(); }
    ~Guard() { unlock(); }
};

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
    AdminHash h;
    esp_fill_random(h.salt, sizeof(h.salt));
    pbkdf2(pass, h.salt, h.hash);
    gAdmin = h;
    gPrefs.putBytes("admin", &gAdmin, sizeof(gAdmin));
    gAdminSet = true;
}

// The slow hash runs without the lock, so the door is never held up by a login.
bool checkAdmin(const char* pass) {
    AdminHash h;
    {
        Guard g;
        if (!gAdminSet) return false;
        h = gAdmin;
    }
    uint8_t out[32];
    const bool ok = pbkdf2(pass, h.salt, out) && ctEqual(out, h.hash, 32);
    mbedtls_platform_zeroize(out, sizeof(out));
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

String urlDecode(const char* s, size_t n) {
    String o;
    o.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (c == '+') o += ' ';
        else if (c == '%' && i + 2 < n + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            const char h[3] = {s[i + 1], s[i + 2], 0};
            o += (char)strtol(h, nullptr, 16);
            i += 2;
        } else o += c;
    }
    return o;
}

// ------------------------------------------------------------- request ---
// Small wrapper over esp_http_server: headers, form/query arguments, and
// response headers whose strings live until the response is sent.
struct Req {
    httpd_req_t* r;
    String query, body, cookie;
    bool bodyRead = false;

    explicit Req(httpd_req_t* rq) : r(rq) {
        const size_t q = httpd_req_get_url_query_len(r);
        if (q && q < 512) {
            char buf[513];
            if (httpd_req_get_url_query_str(r, buf, sizeof(buf)) == ESP_OK) query = buf;
        }
    }
    String header(const char* name) {
        const size_t n = httpd_req_get_hdr_value_len(r, name);
        if (!n || n > 1024) return String();
        char* b = (char*)malloc(n + 1);
        if (!b) return String();
        String s;
        if (httpd_req_get_hdr_value_str(r, name, b, n + 1) == ESP_OK) s = b;
        free(b);
        return s;
    }
    bool readBody() {
        if (bodyRead) return true;
        bodyRead = true;
        if (r->content_len > MAX_FORM) return false;
        size_t left = r->content_len;
        char buf[256];
        while (left) {
            const int n = httpd_req_recv(r, buf, min(left, sizeof(buf)));
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) return false;
            body.concat(buf, n);
            left -= n;
        }
        return true;
    }
    static String find(const String& s, const char* key) {
        const size_t kl = strlen(key);
        int i = 0;
        while (i < (int)s.length()) {
            int amp = s.indexOf('&', i);
            if (amp < 0) amp = s.length();
            if ((size_t)(amp - i) > kl && s[i + kl] == '=' && !strncmp(s.c_str() + i, key, kl))
                return urlDecode(s.c_str() + i + kl + 1, amp - i - kl - 1);
            if ((size_t)(amp - i) == kl && !strncmp(s.c_str() + i, key, kl)) return String();
            i = amp + 1;
        }
        return String();
    }
    bool has(const char* key) {
        readBody();
        const String k = String(key) + "=";
        return query.startsWith(k) || query.indexOf("&" + k) >= 0 || body.startsWith(k) || body.indexOf("&" + k) >= 0;
    }
    String arg(const char* key) {
        String v = find(query, key);
        if (v.length()) return v;
        readBody();
        return find(body, key);
    }
};

void common(httpd_req_t* r) {
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
}

esp_err_t json(Req& q, int code, const String& body) {
    common(q.r);
    httpd_resp_set_status(q.r, code == 200 ? "200 OK" : code == 401 ? "401 Unauthorized" : code == 404 ? "404 Not Found"
                                                                                            : "400 Bad Request");
    httpd_resp_set_type(q.r, "application/json");
    return httpd_resp_send(q.r, body.c_str(), body.length());
}

esp_err_t result(Req& q, bool ok, const String& msg) {
    String m = msg;
    m.replace("\\", "\\\\");
    m.replace("\"", "\\\"");
    return json(q, ok ? 200 : 400, String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + m + "\"}");
}

// ------------------------------------------------------------ sessions ---

bool loggedIn(Req& q) {
    const String c = q.header("Cookie");
    const int i = c.indexOf("atds=");
    if (i < 0 || c.length() < (unsigned)i + 5 + 32) return false;
    uint8_t tok[16];
    if (!fromHex(c.c_str() + i + 5, tok, 16)) return false;
    Guard g;
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

void newSession(Req& q) {
    Guard g;
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
    q.cookie = String("atds=") + hex + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=43200";
    httpd_resp_set_hdr(q.r, "Set-Cookie", q.cookie.c_str());
}

// POSTs must carry X-ATDS: 1. Browsers cannot add that header to a cross-site
// form post, so this (with SameSite=Strict cookies) blocks request forgery.
bool postOk(Req& q) {
    if (q.header("X-ATDS") != "1") {
        result(q, false, "missing request header");
        return false;
    }
    return true;
}

bool requireAdmin(Req& q) {
    if (loggedIn(q)) return true;
    json(q, 401, "{\"ok\":false,\"msg\":\"login required\"}");
    return false;
}

// ------------------------------------------------------------- handlers ---

esp_err_t hPage(httpd_req_t* r) {
    common(r);
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, DASHBOARD_HTML, strlen(DASHBOARD_HTML));
}

esp_err_t hMe(httpd_req_t* r) {
    Req q(r);
    return json(q, 200, String("{\"admin\":") + (loggedIn(q) ? "true" : "false") + "}");
}

esp_err_t hLogin(httpd_req_t* r) {
    Req q(r);
    if (!postOk(q)) return ESP_OK;
    const uint32_t now = millis();
    {
        Guard g;
        if ((int32_t)(gLoginBlockUntil - now) > 0)
            return result(q, false, "too many attempts, wait " + String((gLoginBlockUntil - now) / 1000 + 1) + " s");
    }
    String p = q.arg("password");
    const bool ok = p.length() && p.length() <= 64 && checkAdmin(p.c_str());
    for (unsigned i = 0; i < p.length(); i++) p.setCharAt(i, '0');
    Guard g;
    if (ok) {
        gLoginFails = 0;
        newSession(q);
        evlog::append(evlog::EV_WEB_LOGIN);
        Serial.println("[web] admin logged in");
        return result(q, true, "ok");
    }
    gLoginFails++;
    evlog::append(evlog::EV_WEB_LOGIN_FAIL, evlog::NO_SLOT, 0, gLoginFails);
    Serial.printf("[web] admin login failed (%u)\n", gLoginFails);
    if (gLoginFails >= LOGIN_MAX_FAILS) {
        gLoginFails = 0;
        gLoginBlockUntil = now + LOGIN_BLOCK_MS;
    }
    return result(q, false, "wrong password");
}

esp_err_t hLogout(httpd_req_t* r) {
    Req q(r);
    if (!postOk(q)) return ESP_OK;
    {
        Guard g;
        for (Session& s : gSessions) s.used = false;   // single-door system: log every device out
    }
    httpd_resp_set_hdr(r, "Set-Cookie", "atds=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    return result(q, true, "logged out");
}

esp_err_t hPublic(httpd_req_t* r) {
    Req q(r);
    String o;
    {
        Guard g;
        app::publicJson(o);
    }
    return json(q, 200, o);
}

esp_err_t hPhonePin(httpd_req_t* r) {
    Req q(r);
    if (!postOk(q)) return ESP_OK;
    String pin = q.arg("pin");
    String msg;
    bool ok;
    {
        Guard g;
        const uint32_t now = millis();
        if (gPhoneSeen && now - gLastPhoneMs < PHONE_MIN_INTERVAL_MS) {
            ok = false;
            msg = "slow down";
        } else {
            gPhoneSeen = true;
            gLastPhoneMs = now;
            ok = app::setPhonePin(pin.c_str(), msg);
        }
    }
    for (unsigned i = 0; i < pin.length(); i++) pin.setCharAt(i, '0');   // overwrite the copies
    for (unsigned i = 0; i < q.body.length(); i++) q.body.setCharAt(i, '0');
    return result(q, ok, msg);
}

esp_err_t hStatus(httpd_req_t* r) {
    Req q(r);
    if (!requireAdmin(q)) return ESP_OK;
    String o;
    {
        Guard g;
        app::statusJson(o);
    }
    return json(q, 200, o);
}

esp_err_t hCards(httpd_req_t* r) {
    Req q(r);
    if (!requireAdmin(q)) return ESP_OK;
    String o;
    {
        Guard g;
        app::cardsJson(o);
    }
    return json(q, 200, o);
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

esp_err_t hEvents(httpd_req_t* r) {
    Req q(r);
    if (!requireAdmin(q)) return ESP_OK;
    const String ns = q.arg("n");
    const int n = ns.length() ? constrain(ns.toInt(), 1, 200) : 50;
    String o;
    o.reserve(n * 140 + 64);
    {
        Guard g;
        o = "{\"head\":" + String(evlog::headSeq()) + ",\"events\":[";
        EvCtx c = {&o, true};
        evlog::forEach(n, evToJson, &c);
    }
    o += "]}";
    return json(q, 200, o);
}

esp_err_t hVerify(httpd_req_t* r) {
    Req q(r);
    if (!requireAdmin(q)) return ESP_OK;
    uint32_t n, bad, back;
    bool ok;
    {
        Guard g;
        ok = evlog::verify(&n, &bad, &back);
    }
    return json(q, 200, String("{\"ok\":") + (ok ? "true" : "false") + ",\"checked\":" + n + ",\"bad\":" + bad +
                            ",\"backwards\":" + back + "}");
}

bool validName(const String& n) {
    if (n.length() > 20) return false;
    for (unsigned i = 0; i < n.length(); i++) {
        const char c = n[i];
        if (!(isalnum((unsigned char)c) || c == ' ' || c == '\'' || c == '-' || c == '.')) return false;
    }
    return true;
}

void nameKey(uint8_t slot, char k[6]) { snprintf(k, 6, "n%u", slot); }

esp_err_t hAction(httpd_req_t* r) {
    Req q(r);
    if (!postOk(q) || !requireAdmin(q)) return ESP_OK;
    const String a = q.arg("a");
    String msg;
    bool ok = false;
    uint8_t code = 0;
    if (a == "password") {   // slow hash: checked outside the lock
        code = evlog::WA_PASSWORD;
        const String oldp = q.arg("old"), newp = q.arg("new");
        if (!checkAdmin(oldp.c_str())) msg = "current password is wrong";
        else if (newp.length() < 8 || newp.length() > 64) msg = "new password must be 8-64 characters";
        else {
            Guard g;
            storeAdmin(newp.c_str());
            gInitPass[0] = 0;
            gPrefs.remove("init");
            ok = true;
            msg = "password changed";
        }
        Guard g;
        evlog::append(evlog::EV_WEB_ACTION, evlog::NO_SLOT, code, ok ? 1 : 0);
        return result(q, ok, msg);
    }
    Guard g;
    if (a == "lock") { code = evlog::WA_LOCK; ok = app::lock(msg); }
    else if (a == "clear") { code = evlog::WA_CLEAR; ok = app::clearAlarm(msg); }
    else if (a == "resetlockout") { code = evlog::WA_RESET_LOCKOUT; ok = app::resetLockout(msg); }
    else if (a == "enroll") { code = evlog::WA_ENROLL; ok = app::startEnroll(msg); }
    else if (a == "setpin") { code = evlog::WA_SETPIN; ok = app::startSetPin(msg); }
    else if (a == "cancel") { code = evlog::WA_CANCEL; ok = app::cancelAdmin(msg); }
    else if (a == "delete") { code = evlog::WA_DELETE; ok = app::deleteCard(q.arg("slot").toInt(), msg); }
    else if (a == "time") {
        code = evlog::WA_TIME;
        const long t = q.arg("t").toInt();
        if (t <= 1700000000L) msg = "bad time";
        else ok = app::setTime((uint32_t)t, msg);
    } else if (a == "name") {
        code = evlog::WA_NAME;
        const int slot = q.arg("slot").toInt();
        String n = q.arg("name");
        n.trim();
        if (slot < 0 || !creds::slotUsed((uint8_t)slot)) msg = "no such card";
        else if (!validName(n)) msg = "names can use letters, numbers and spaces, up to 20";
        else {
            char k[6];
            nameKey((uint8_t)slot, k);
            if (n.length()) gPrefs.putString(k, n);
            else gPrefs.remove(k);
            ok = true;
            msg = n.length() ? "name saved" : "name removed";
        }
    } else if (a == "homewifi") {
        code = evlog::WA_HOMEWIFI;
        String ssid = q.arg("ssid"), pass = q.arg("pass");
        ssid.trim();
        ok = homenet::set(ssid.c_str(), pass.c_str());
        msg = !ok ? "network name up to 32 characters, password 8 to 64"
              : ssid.length() ? "joining your home WiFi" : "home WiFi switched off";
        for (unsigned i = 0; i < pass.length(); i++) pass.setCharAt(i, '0');
        for (unsigned i = 0; i < q.body.length(); i++) q.body.setCharAt(i, '0');
    } else if (a == "zend") {
        code = evlog::WA_ZEND;
        if (q.arg("off") == "1") {
            zendlink::forget();
            ok = true;
            msg = "this door is no longer linked to Zend";
        } else {
            String key = q.arg("key"), sec = q.arg("secret");
            key.trim();
            sec.trim();
            if (!key.startsWith("zdevice_") || !sec.startsWith("zdsig_")) msg = "the key starts with zdevice_ and the secret with zdsig_";
            else ok = zendlink::setKey(key.c_str()) && zendlink::setSecret(sec.c_str());
            if (ok) msg = "linked to Zend";
            else if (!msg.length()) msg = "that key or secret is too long";
            for (unsigned i = 0; i < sec.length(); i++) sec.setCharAt(i, '0');
            for (unsigned i = 0; i < q.body.length(); i++) q.body.setCharAt(i, '0');
        }
    } else {
        return result(q, false, "unknown action");
    }
    evlog::append(evlog::EV_WEB_ACTION, evlog::NO_SLOT, code, ok ? 1 : 0);
    Serial.printf("[web] action %s: %s\n", a.c_str(), msg.c_str());
    return result(q, ok, msg);
}

// Signed OTA: streams the package straight into the inactive slot. Not under
// the door lock (takes ~20 s), only the checks and the log entries are.
esp_err_t hOta(httpd_req_t* r) {
    Req q(r);
    if (!postOk(q) || !requireAdmin(q)) return ESP_OK;
    String msg;
    {
        Guard g;
        if (!app::updateAllowed(msg)) return result(q, false, msg);
    }
    if (r->content_len <= ota::HEADER_LEN) return result(q, false, "no update file received");
    uint8_t header[ota::HEADER_LEN];
    size_t got = 0;
    while (got < sizeof(header)) {
        const int n = httpd_req_recv(r, (char*)header + got, sizeof(header) - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return result(q, false, "upload interrupted");
        got += n;
    }
    const char* err = nullptr;
    uint32_t version = 0;
    ota::Session* s = ota::begin(header, r->content_len - ota::HEADER_LEN, &err, &version);
    {
        Guard g;
        evlog::append(s ? evlog::EV_OTA_START : evlog::EV_OTA_FAIL, evlog::NO_SLOT, s ? 0 : 1, version);
    }
    if (!s) {
        Serial.printf("[ota] rejected: %s\n", err);
        return result(q, false, err);
    }
    Serial.printf("[ota] receiving signed firmware v%lu (%u bytes)\n", (unsigned long)version,
                  (unsigned)(r->content_len - ota::HEADER_LEN));
    static uint8_t buf[4096];
    size_t left = r->content_len - ota::HEADER_LEN;
    bool ok = true;
    while (left && ok) {
        const int n = httpd_req_recv(r, (char*)buf, min(left, sizeof(buf)));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            err = "upload interrupted";
            ok = false;
            break;
        }
        ok = ota::write(s, buf, n, &err);
        left -= n;
    }
    if (ok) ok = ota::finish(s, &err);
    else ota::abort(s);
    {
        Guard g;
        evlog::append(ok ? evlog::EV_OTA_INSTALLED : evlog::EV_OTA_FAIL, evlog::NO_SLOT, ok ? 0 : 2, version);
    }
    Serial.printf("[ota] %s\n", ok ? "verified and installed, restarting" : err);
    if (ok) gReboot = true;
    return result(q, ok, ok ? "update installed, the door restarts now" : err);
}

// Certificate for https://door.usezend.app (tools/renew_cert.py, or the app).
// GET: what is installed, plus the door's certificate request (CSR).
esp_err_t hCertGet(httpd_req_t* r) {
    Req q(r);
    if (!requireAdmin(q)) return ESP_OK;
    String csr;
    {
        Guard g;
        devcert::csrPem(csr);
    }
    csr.replace("\n", "\\n");
    return json(q, 200, String("{\"host\":\"") + devcert::HOST + "\",\"installed\":" +
                            (devcert::installed() || devcert::pending() ? "true" : "false") +
                            ",\"notAfter\":" + devcert::notAfter() + ",\"issuer\":\"" + devcert::issuer() +
                            "\",\"staging\":" + (devcert::staging() ? "true" : "false") + ",\"csr\":\"" + csr + "\"}");
}

// POST: the full-chain PEM as the raw request body.
esp_err_t hCertPut(httpd_req_t* r) {
    Req q(r);
    if (!postOk(q) || !requireAdmin(q)) return ESP_OK;
    const size_t len = r->content_len;
    if (!len || len > devcert::MAX_CHAIN) return result(q, false, "certificate file is empty or too big");
    char* buf = (char*)malloc(len + 1);
    if (!buf) return result(q, false, "not enough memory");
    size_t got = 0;
    while (got < len) {
        const int n = httpd_req_recv(r, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    buf[got] = 0;
    String msg;
    bool ok = false;
    if (got != len) msg = "upload interrupted";
    else {
        Guard g;
        uint8_t st;
        const uint32_t t = evlog::now(&st);
        ok = devcert::install(buf, strlen(buf), st == evlog::RTC_OK ? t : 0, msg);
        evlog::append(evlog::EV_WEB_ACTION, evlog::NO_SLOT, evlog::WA_CERT, ok ? 1 : 0);
        if (ok) gCertAt = millis() | 1;
    }
    free(buf);
    Serial.printf("[cert] upload: %s\n", msg.c_str());
    return result(q, ok, ok ? msg + ", reconnecting in a moment" : msg);
}

esp_err_t hNotFound(httpd_req_t* r, httpd_err_code_t) {
    common(r);
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_send(r, "not found", HTTPD_RESP_USE_STRLEN);
}

// --------------------------------------------------------- setup (http) ---

const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Set up your door app</title>
<style>body{margin:0;font:17px/1.55 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#f5f6f8;color:#13171c}
main{max-width:520px;margin:0 auto;padding:2rem 1.3rem}h1{font-size:1.7rem;margin:0 0 .4rem}.c{background:#fff;border:1px solid #e6e8ec;border-radius:20px;padding:1.2rem 1.3rem;margin:1rem 0}
.b{display:block;text-align:center;background:#2f6bff;color:#fff;padding:1rem;border-radius:14px;font-weight:650;text-decoration:none;margin:.8rem 0}
.g{background:#eef1f5;color:#13171c}ol{padding-left:1.2rem;margin:.4rem 0}li{margin:.3rem 0}small{color:#5d6672}</style></head><body><main>
<h1>Set up your door app</h1><p>This one-time step lets your phone talk to the door securely.</p>
<div class="c"><b>1. Install the door certificate</b><a class="b" href="/ca.crt">Download certificate</a>
<small><b>iPhone:</b> tap Allow, then Settings &gt; Profile Downloaded &gt; Install. Then Settings &gt; General &gt; About &gt; Certificate Trust Settings, and switch on "ATDS Door local CA".<br>
<b>Android:</b> Settings &gt; Security &gt; Encryption &amp; credentials &gt; Install a certificate &gt; CA certificate, and pick the downloaded file.</small></div>
<div class="c"><b>2. Open the app</b><a class="b" href="https://192.168.4.1/">Open the door app</a>
<small>If your phone still warns that the connection is not private, step 1 is not finished.</small></div>
</main></body></html>)HTML";

void toHttps() {
    gSetup.sendHeader("Location", devcert::installed() ? String("https://") + devcert::HOST + "/" : "https://192.168.4.1/");
    gSetup.send(301, "text/plain", "use https");
}

void setupPage() {
    if (devcert::installed()) {   // a trusted certificate: nothing to set up
        toHttps();
        return;
    }
    gSetup.sendHeader("Cache-Control", "no-store");
    gSetup.send_P(200, "text/html; charset=utf-8", SETUP_HTML);
}

void caCert() {
    gSetup.sendHeader("Content-Disposition", "attachment; filename=\"atds-door-ca.crt\"");
    gSetup.send_P(200, "application/x-x509-ca-cert", (const char*)TLS_CA_DER, sizeof(TLS_CA_DER));
}


void reg(const char* uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = m;
    u.handler = fn;
    httpd_register_uri_handler(gHttps, &u);
}

}  // namespace

void lock() { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(gLock); }
bool rebootRequested() { return gReboot; }

void loadCredentials() {
    if (!gLock) gLock = xSemaphoreCreateRecursiveMutex();
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

namespace {
bool startHttps() {
    httpd_ssl_config_t c = HTTPD_SSL_CONFIG_DEFAULT();
    if (devcert::installed()) {   // https://door.usezend.app, trusted by every phone
        c.cacert_pem = (const uint8_t*)devcert::certPem();
        c.cacert_len = strlen(devcert::certPem()) + 1;
        c.prvtkey_pem = (const uint8_t*)devcert::keyPem();
        c.prvtkey_len = strlen(devcert::keyPem()) + 1;
    } else {                      // https://192.168.4.1 with the local CA
        c.cacert_pem = (const uint8_t*)TLS_SERVER_CERT;
        c.cacert_len = sizeof(TLS_SERVER_CERT);
        c.prvtkey_pem = (const uint8_t*)TLS_SERVER_KEY;
        c.prvtkey_len = sizeof(TLS_SERVER_KEY);
    }
    c.httpd.max_open_sockets = 3;   // each TLS session needs ~40 KB of RAM
    c.httpd.max_uri_handlers = 16;
    c.httpd.max_resp_headers = 10;
    c.httpd.stack_size = 10240;
    c.httpd.lru_purge_enable = true;
    c.httpd.recv_wait_timeout = 10;
    c.httpd.send_wait_timeout = 10;
    if (httpd_ssl_start(&gHttps, &c) != ESP_OK) {
        Serial.println("[web] HTTPS server failed to start");
        gHttps = nullptr;
        return false;
    }
    reg("/", HTTP_GET, hPage);
    reg("/api/me", HTTP_GET, hMe);
    reg("/api/login", HTTP_POST, hLogin);
    reg("/api/logout", HTTP_POST, hLogout);
    reg("/api/public", HTTP_GET, hPublic);
    reg("/api/phonepin", HTTP_POST, hPhonePin);
    reg("/api/status", HTTP_GET, hStatus);
    reg("/api/cards", HTTP_GET, hCards);
    reg("/api/events", HTTP_GET, hEvents);
    reg("/api/verify", HTTP_GET, hVerify);
    reg("/api/action", HTTP_POST, hAction);
    reg("/api/ota", HTTP_POST, hOta);
    reg("/api/cert", HTTP_GET, hCertGet);
    reg("/api/cert", HTTP_POST, hCertPut);
    httpd_register_err_handler(gHttps, HTTPD_404_NOT_FOUND, hNotFound);
    return true;
}

const char* appUrl() { return devcert::installed() ? "https://door.usezend.app" : "https://192.168.4.1"; }
}  // namespace

void begin() {
    devcert::begin();   // the radio is on now, so the hardware RNG is good for a new key
    startHttps();
    gSetup.on("/", HTTP_GET, setupPage);
    gSetup.on("/ca.crt", HTTP_GET, caCert);
    gSetup.onNotFound(toHttps);
    gSetup.begin();
    // Only door.usezend.app is answered; other names get "does not exist".
    gDns.start(53, devcert::HOST, WiFi.softAPIP());
    Serial.printf("[web] app on %s%s. Type 'wifi' for the passwords\n", appUrl(),
                  devcert::installed() ? " (also https://192.168.4.1)" : " (setup page on http)");
}

void service() {
    gSetup.handleClient();
    gDns.processNextRequest();
    // A new certificate: restart HTTPS once the upload's reply has gone out.
    if (gCertAt && millis() - gCertAt > 1500 && devcert::pending()) {
        gCertAt = 0;
        if (gHttps) httpd_ssl_stop(gHttps);
        gHttps = nullptr;
        devcert::activate();
        Serial.println(startHttps() ? "[cert] HTTPS restarted with the new certificate" : "[cert] HTTPS restart failed");
    }
}

void certInstalled() { gCertAt = millis() | 1; }


void printCredentials() {
    Serial.printf("WiFi network : %s\n", SSID);
    Serial.printf("WiFi password: %s\n", gApPass);
    if (devcert::installed()) {
        Serial.printf("Door app     : https://%s\n", devcert::HOST);
    } else {
        Serial.printf("First time   : http://%s  (installs the certificate)\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("Door app     : https://%s\n", WiFi.softAPIP().toString().c_str());
    }
    if (gInitPass[0]) Serial.printf("Admin password (first-boot, change it in the app): %s\n", gInitPass);
    else Serial.println("Admin password: changed by the user (reset with 'webpass <new>')");
    Serial.printf("Firmware v%d, clients connected: %u, free heap %u bytes\n", ATDS_FW_VERSION, WiFi.softAPgetStationNum(),
                  (unsigned)ESP.getFreeHeap());
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

void cardName(uint8_t slot, char* out, size_t cap) {
    char k[6];
    nameKey(slot, k);
    out[0] = 0;
    if (gPrefs.isKey(k)) gPrefs.getString(k, out, cap);
}

void forgetName(uint8_t slot) {
    char k[6];
    nameKey(slot, k);
    if (gPrefs.isKey(k)) gPrefs.remove(k);
}

void forgetAllNames() {
    for (uint8_t i = 0; i < MAX_CREDENTIALS; i++) forgetName(i);
}

}  // namespace web
