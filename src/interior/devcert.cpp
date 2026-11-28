#include "devcert.h"

#include <Preferences.h>
#include <esp_random.h>
#include <string.h>

#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "rtc.h"

namespace devcert {
namespace {

Preferences gPrefs;
mbedtls_pk_context gPk;
bool gKeyOk = false;
char gKeyPem[300] = "";
char* gChain = nullptr;      // chain the HTTPS server is using
char* gNext = nullptr;       // freshly installed, waiting for the server restart
uint32_t gNotAfter = 0, gNextNotAfter = 0;
bool gStaging = false, gNextStaging = false;
char gIssuer[40] = "", gNextIssuer[40] = "";

int rng(void*, unsigned char* out, size_t n) {
    esp_fill_random(out, n);
    return 0;
}

uint32_t toUnix(const mbedtls_x509_time& t) { return rtc::fromCivil(t.year, t.mon, t.day, t.hour, t.min, t.sec); }

bool samePublicKey(mbedtls_pk_context* a, mbedtls_pk_context* b) {
    unsigned char da[160], db[160];
    const int la = mbedtls_pk_write_pubkey_der(a, da, sizeof(da));
    const int lb = mbedtls_pk_write_pubkey_der(b, db, sizeof(db));
    return la > 0 && la == lb && !memcmp(da + sizeof(da) - la, db + sizeof(db) - lb, la);
}

bool namesHost(const mbedtls_x509_crt& c) {
    const size_t hl = strlen(HOST);
    for (const mbedtls_x509_sequence* s = &c.subject_alt_names; s && s->buf.p; s = s->next) {
        if (s->buf.tag != (MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2) || s->buf.len != hl) continue;
        if (!strncasecmp((const char*)s->buf.p, HOST, hl)) return true;
    }
    return false;
}

void issuerCn(const mbedtls_x509_crt& c, char* out, size_t cap) {
    char dn[160];
    out[0] = 0;
    if (mbedtls_x509_dn_gets(dn, sizeof(dn), &c.issuer) < 0) return;
    const char* cn = strstr(dn, "CN=");
    strlcpy(out, cn ? cn + 3 : dn, cap);
}

// Parses and checks a chain. On success fills the outputs.
bool check(const char* pem, size_t len, uint32_t nowUnix, String& msg, uint32_t* notAfter, bool* stagingOut,
           char* issuerOut) {
    if (!gKeyOk) {
        msg = "the door has no key yet";
        return false;
    }
    if (!len || len > MAX_CHAIN || pem[len] != 0) {
        msg = "certificate file is empty or too big";
        return false;
    }
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    bool ok = false;
    do {
        if (mbedtls_x509_crt_parse(&chain, (const unsigned char*)pem, len + 1) != 0) {
            msg = "that is not a certificate file (expected the full-chain .pem)";
            break;
        }
        if (!samePublicKey(&chain.pk, &gPk)) {
            msg = "this certificate belongs to a different key, not this door";
            break;
        }
        if (!namesHost(chain)) {
            msg = String("this certificate is not for ") + HOST;
            break;
        }
        if (!chain.next) {
            msg = "the chain is incomplete: upload the full-chain file, not the single certificate";
            break;
        }
        // The leaf must really be signed by the intermediate that comes with it.
        // Dates are checked below against the door's clock, not the system time.
        uint32_t flags = 0;
        mbedtls_x509_crt_verify(&chain, chain.next, nullptr, HOST, &flags, nullptr, nullptr);
        flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
        if (flags) {
            msg = "the certificate chain does not verify";
            break;
        }
        const uint32_t from = toUnix(chain.valid_from), to = toUnix(chain.valid_to);
        if (nowUnix && nowUnix >= to) {
            msg = "this certificate has already expired";
            break;
        }
        if (nowUnix && nowUnix + 86400 < from) {
            msg = "this certificate is not valid yet: check the door clock";
            break;
        }
        *notAfter = to;
        issuerCn(chain, issuerOut, sizeof(gIssuer));
        char dn[160];
        *stagingOut = mbedtls_x509_dn_gets(dn, sizeof(dn), &chain.issuer) > 0 && strstr(dn, "STAGING");
        ok = true;
    } while (false);
    mbedtls_x509_crt_free(&chain);
    return ok;
}

}  // namespace

bool begin() {
    gPrefs.begin("atds_cert", false);
    mbedtls_pk_init(&gPk);
    if (gPrefs.getString("key", gKeyPem, sizeof(gKeyPem)) > 0 &&
        mbedtls_pk_parse_key(&gPk, (const unsigned char*)gKeyPem, strlen(gKeyPem) + 1, nullptr, 0) == 0) {
        gKeyOk = true;
    } else {
        Serial.println("[cert] creating this door's private key (stays on the door)");
        mbedtls_pk_free(&gPk);
        mbedtls_pk_init(&gPk);
        gKeyOk = mbedtls_pk_setup(&gPk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
                 mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(gPk), rng, nullptr) == 0 &&
                 mbedtls_pk_write_key_pem(&gPk, (unsigned char*)gKeyPem, sizeof(gKeyPem)) == 0;
        if (gKeyOk) {
            gPrefs.putString("key", gKeyPem);
            gPrefs.remove("chain");   // a certificate for an old key is useless
        } else {
            Serial.println("[cert] key generation failed");
            return false;
        }
    }
    const size_t n = gPrefs.getBytesLength("chain");
    if (n && n <= MAX_CHAIN) {
        char* buf = (char*)malloc(n + 1);
        if (buf) {
            gPrefs.getBytes("chain", buf, n);
            buf[n] = 0;
            String msg;
            if (check(buf, strlen(buf), 0, msg, &gNotAfter, &gStaging, gIssuer)) {
                gChain = buf;
            } else {
                Serial.printf("[cert] stored certificate ignored: %s\n", msg.c_str());
                free(buf);
            }
        }
    }
    return true;
}

bool installed() { return gChain != nullptr; }
const char* certPem() { return gChain; }
const char* keyPem() { return gKeyPem; }
uint32_t notAfter() { return gNext ? gNextNotAfter : gNotAfter; }
bool staging() { return gNext ? gNextStaging : gStaging; }
const char* issuer() { return gNext ? gNextIssuer : gIssuer; }
bool pending() { return gNext != nullptr; }

void activate() {
    if (!gNext) return;
    free(gChain);
    gChain = gNext;
    gNext = nullptr;
    gNotAfter = gNextNotAfter;
    gStaging = gNextStaging;
    memcpy(gIssuer, gNextIssuer, sizeof(gIssuer));
}

bool csrPem(String& out) {
    if (!gKeyOk) return false;
    // subjectAltName = SEQUENCE { [2] dNSName HOST }
    const size_t hl = strlen(HOST);
    unsigned char san[64];
    if (hl + 4 > sizeof(san)) return false;
    san[0] = 0x30;
    san[1] = (unsigned char)(hl + 2);
    san[2] = 0x82;
    san[3] = (unsigned char)hl;
    memcpy(san + 4, HOST, hl);
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &gPk);
    const String subject = String("CN=") + HOST;
    unsigned char* buf = (unsigned char*)malloc(1024);
    bool ok = buf && mbedtls_x509write_csr_set_subject_name(&csr, subject.c_str()) == 0 &&
              mbedtls_x509write_csr_set_extension(&csr, MBEDTLS_OID_SUBJECT_ALT_NAME,
                                                  MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), san, hl + 4) == 0 &&
              mbedtls_x509write_csr_pem(&csr, buf, 1024, rng, nullptr) == 0;
    if (ok) out = (const char*)buf;
    free(buf);
    mbedtls_x509write_csr_free(&csr);
    return ok;
}

bool install(const char* pem, size_t len, uint32_t nowUnix, String& msg) {
    uint32_t to;
    bool stg;
    char iss[sizeof(gIssuer)];
    if (!check(pem, len, nowUnix, msg, &to, &stg, iss)) return false;
    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        msg = "not enough memory";
        return false;
    }
    memcpy(buf, pem, len + 1);
    if (gPrefs.putBytes("chain", buf, len) != len) {
        free(buf);
        msg = "could not save the certificate";
        return false;
    }
    free(gNext);
    gNext = buf;
    gNextNotAfter = to;
    gNextStaging = stg;
    memcpy(gNextIssuer, iss, sizeof(gNextIssuer));
    msg = stg ? "test certificate installed (phones will still warn)" : "certificate installed";
    return true;
}

}  // namespace devcert
