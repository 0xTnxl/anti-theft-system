#include "secure_link.h"

#include <string.h>

#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

namespace slink {
namespace {

// Handshake frame layout: hdr(4) | pub(32) | nonce(16) | tag(32), HS3: hdr(4) | tag(32)
constexpr size_t HS_HDR = 4;
constexpr size_t PUB = 32;
constexpr size_t NONCE = 16;
constexpr size_t MACTAG = 32;
constexpr size_t HS12_LEN = HS_HDR + PUB + NONCE + MACTAG;   // 84
constexpr size_t HS3_LEN = HS_HDR + MACTAG;                  // 36
constexpr size_t OFF_PUB = HS_HDR;
constexpr size_t OFF_NONCE = HS_HDR + PUB;
constexpr size_t OFF_TAG = HS_HDR + PUB + NONCE;

const char KDF_INFO[] = "ATDS v1 keys";

const mbedtls_md_info_t* sha256() { return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); }

void wipe(void* p, size_t n) { mbedtls_platform_zeroize(p, n); }

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

void putU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint64_t getU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// Streaming HMAC-SHA256. mbedtls_md_free zeroizes the context.
class Hmac {
public:
    Hmac(const uint8_t* key, size_t keyLen) {
        mbedtls_md_init(&ctx_);
        ok_ = mbedtls_md_setup(&ctx_, sha256(), 1) == 0 &&
              mbedtls_md_hmac_starts(&ctx_, key, keyLen) == 0;
    }
    ~Hmac() { mbedtls_md_free(&ctx_); }
    void add(const void* d, size_t n) {
        if (ok_) ok_ = mbedtls_md_hmac_update(&ctx_, (const unsigned char*)d, n) == 0;
    }
    bool finish(uint8_t out[32]) {
        if (ok_) ok_ = mbedtls_md_hmac_finish(&ctx_, out) == 0;
        return ok_;
    }

private:
    mbedtls_md_context_t ctx_;
    bool ok_;
};

int rngAdapter(void* p, unsigned char* out, size_t n) {
    (*static_cast<RngFn*>(p))(out, n);
    return 0;
}

bool x25519Keygen(RngFn* rng, uint8_t priv[32], uint8_t pub[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_ecdh_gen_public(&grp, &d, &q, rngAdapter, rng) == 0 &&
              mbedtls_mpi_write_binary_le(&d, priv, 32) == 0 &&
              mbedtls_mpi_write_binary_le(&q.X, pub, 32) == 0;
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool x25519Shared(RngFn* rng, const uint8_t priv[32], const uint8_t peerPub[32],
                  uint8_t out[32]) {
    uint8_t u[32];
    memcpy(u, peerPub, 32);
    u[31] &= 0x7F;   // RFC 7748: ignore the top bit
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point p;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&p);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, priv, 32) == 0 &&
              mbedtls_mpi_read_binary_le(&p.X, u, 32) == 0 &&
              mbedtls_mpi_lset(&p.Z, 1) == 0 &&
              mbedtls_ecdh_compute_shared(&grp, &z, &p, &d, rngAdapter, rng) == 0 &&
              mbedtls_mpi_write_binary_le(&z, out, 32) == 0;
    mbedtls_ecp_point_free(&p);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    wipe(u, sizeof(u));
    if (ok) {
        // Reject the all-zero result from small-order points.
        uint8_t acc = 0;
        for (int i = 0; i < 32; i++) acc |= out[i];
        ok = acc != 0;
    }
    if (!ok) wipe(out, 32);
    return ok;
}

bool gcm(bool encrypt, const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
         size_t aadLen, const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[16]) {
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = encrypt ? mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, aad,
                                                 aadLen, in, out, 16, tag)
                     : mbedtls_gcm_auth_decrypt(&g, len, nonce, 12, aad, aadLen, tag, 16, in,
                                                out);
    }
    mbedtls_gcm_free(&g);
    if (rc != 0) wipe(out, len);
    return rc == 0;
}

}  // namespace

SecureLink::~SecureLink() {
    reset();
    wipe(psk_, sizeof(psk_));
}

bool SecureLink::begin(Role role, const uint8_t psk[PSK_LEN], const uint8_t macExterior[MAC_LEN],
                       const uint8_t macInterior[MAC_LEN], RngFn rng) {
    reset();
    if (!psk || !macExterior || !macInterior || !rng) return false;
    role_ = role;
    myId_ = role == Role::Exterior ? ID_EXTERIOR : ID_INTERIOR;
    peerId_ = role == Role::Exterior ? ID_INTERIOR : ID_EXTERIOR;
    memcpy(psk_, psk, PSK_LEN);
    memcpy(macE_, macExterior, MAC_LEN);
    memcpy(macI_, macInterior, MAC_LEN);
    rng_ = rng;
    authFails_ = 0;
    hs1Seen_ = false;
    ready_ = true;
    return true;
}

void SecureLink::reset() {
    wipe(&cur_, sizeof(cur_));
    wipe(&prev_, sizeof(prev_));
    clearPending();
}

void SecureLink::clearPending() { wipe(&pend_, sizeof(pend_)); }

uint32_t SecureLink::takeAuthFailures() {
    uint32_t n = authFails_;
    authFails_ = 0;
    return n;
}

bool SecureLink::sessionUsable(const Session& s, uint32_t nowMs) const {
    return s.valid && (uint32_t)(nowMs - s.createdMs) < MAX_SESSION_AGE_MS;
}

bool SecureLink::established(uint32_t nowMs) const { return ready_ && sessionUsable(cur_, nowMs); }

bool SecureLink::rekeyDue(uint32_t nowMs) const {
    if (!established(nowMs)) return true;
    return (uint32_t)(nowMs - cur_.createdMs) >= REKEY_INTERVAL_MS ||
           cur_.txCtr >= MAX_FRAMES_PER_SESSION - 1024;
}

// HMAC(PSK, label | macE | macI | [hs1] | [hs2] | own frame without its tag)
void SecureLink::handshakeTag(uint8_t step, const uint8_t* hs1, const uint8_t* hs2,
                              const uint8_t* own, size_t ownLen, uint8_t out[32]) const {
    char label[] = "ATDS1-HS?";
    label[8] = (char)('0' + step);
    Hmac h(psk_, PSK_LEN);
    h.add(label, sizeof(label) - 1);
    h.add(macE_, MAC_LEN);
    h.add(macI_, MAC_LEN);
    if (hs1) h.add(hs1, HS12_LEN);
    if (hs2) h.add(hs2, HS12_LEN);
    h.add(own, ownLen);
    if (!h.finish(out)) memset(out, 0, 32);
}

// PRK = HKDF-Extract(salt = PSK, ikm = shared | nE | nI)
// OKM = HKDF-Expand(PRK, "ATDS v1 keys" | SHA256(HS1 | HS2), 68)
//     = k_e2i(32) | k_i2e(32) | session_id(4)
bool SecureLink::derive(const uint8_t shared[32], const uint8_t* hs1, const uint8_t* hs2,
                        Session& out) const {
    uint8_t prk[32], th[32], okm[96], t[32];
    bool ok;
    {
        Hmac h(psk_, PSK_LEN);
        h.add(shared, 32);
        h.add(hs1 + OFF_NONCE, NONCE);
        h.add(hs2 + OFF_NONCE, NONCE);
        ok = h.finish(prk);
    }
    if (ok) {
        mbedtls_md_context_t c;
        mbedtls_md_init(&c);
        ok = mbedtls_md_setup(&c, sha256(), 0) == 0 && mbedtls_md_starts(&c) == 0 &&
             mbedtls_md_update(&c, hs1, HS12_LEN) == 0 &&
             mbedtls_md_update(&c, hs2, HS12_LEN) == 0 && mbedtls_md_finish(&c, th) == 0;
        mbedtls_md_free(&c);
    }
    for (uint8_t i = 1; ok && i <= 3; i++) {
        Hmac h(prk, sizeof(prk));
        if (i > 1) h.add(t, sizeof(t));
        h.add(KDF_INFO, sizeof(KDF_INFO) - 1);
        h.add(th, sizeof(th));
        h.add(&i, 1);
        ok = h.finish(t);
        memcpy(okm + 32 * (i - 1), t, 32);
    }
    if (ok) {
        memset(&out, 0, sizeof(out));
        const uint8_t* e2i = okm;
        const uint8_t* i2e = okm + 32;
        memcpy(out.txKey, role_ == Role::Exterior ? e2i : i2e, 32);
        memcpy(out.rxKey, role_ == Role::Exterior ? i2e : e2i, 32);
        memcpy(out.sid, okm + 64, 4);
        out.valid = true;
    }
    wipe(prk, sizeof(prk));
    wipe(okm, sizeof(okm));
    wipe(t, sizeof(t));
    return ok;
}

void SecureLink::install(Session& s, uint32_t nowMs) {
    if (cur_.valid) {
        prev_ = cur_;
        prev_.retiredMs = nowMs;
    }
    cur_ = s;
    cur_.createdMs = nowMs;
    cur_.txCtr = 0;
    cur_.rxLast = 0;
    wipe(&s, sizeof(s));
}

size_t SecureLink::startHandshake(uint8_t* out, size_t cap, uint32_t nowMs) {
    if (!ready_ || role_ != Role::Exterior || !out || cap < HS12_LEN) return 0;
    clearPending();
    uint8_t* f = pend_.hs1;
    f[0] = VERSION;
    f[1] = TYPE_HS1;
    f[2] = myId_;
    f[3] = 0;
    if (!x25519Keygen(&rng_, pend_.priv, f + OFF_PUB)) {
        clearPending();
        return 0;
    }
    rng_(f + OFF_NONCE, NONCE);
    handshakeTag(1, nullptr, nullptr, f, OFF_TAG, f + OFF_TAG);
    pend_.active = true;
    pend_.startedMs = nowMs;
    memcpy(out, f, HS12_LEN);
    return HS12_LEN;
}

void SecureLink::onFrame(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res) {
    res.kind = RxResult::Dropped;
    res.type = 0;
    res.payloadLen = 0;
    res.replyLen = 0;
    if (!ready_ || !in || len < HS_HDR) return;
    if (in[0] != VERSION || in[2] != peerId_ || in[3] != 0) return;

    if (pend_.active && (uint32_t)(nowMs - pend_.startedMs) > HANDSHAKE_TIMEOUT_MS) clearPending();
    if (prev_.valid && (uint32_t)(nowMs - prev_.retiredMs) > PREV_SESSION_GRACE_MS)
        wipe(&prev_, sizeof(prev_));

    const uint8_t type = in[1];
    if (type == TYPE_HS1) {
        if (role_ == Role::Interior) handleHs1(in, len, nowMs, res);
    } else if (type == TYPE_HS2) {
        if (role_ == Role::Exterior) handleHs2(in, len, nowMs, res);
    } else if (type == TYPE_HS3) {
        if (role_ == Role::Interior) handleHs3(in, len, nowMs, res);
    } else if (type >= APP_TYPE_MIN) {
        handleData(in, len, nowMs, res);
    }
}

void SecureLink::handleHs1(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res) {
    if (len != HS12_LEN) return;
    uint8_t tag[32];
    handshakeTag(1, nullptr, nullptr, in, OFF_TAG, tag);
    if (!ctEqual(tag, in + OFF_TAG, MACTAG)) {
        authFails_++;
        res.kind = RxResult::AuthFailed;
        return;
    }
    // A valid HS1 may still be a replay; it cannot finish without the PSK, and
    // the current session stays in place until HS3 arrives.
    if (hs1Seen_ && (uint32_t)(nowMs - lastHs1Ms_) < HS1_MIN_INTERVAL_MS) return;
    hs1Seen_ = true;
    lastHs1Ms_ = nowMs;

    clearPending();
    uint8_t* f = pend_.hs2;
    uint8_t priv[32], shared[32];
    f[0] = VERSION;
    f[1] = TYPE_HS2;
    f[2] = myId_;
    f[3] = 0;
    bool ok = x25519Keygen(&rng_, priv, f + OFF_PUB) &&
              x25519Shared(&rng_, priv, in + OFF_PUB, shared);
    wipe(priv, sizeof(priv));
    if (ok) {
        rng_(f + OFF_NONCE, NONCE);
        memcpy(pend_.hs1, in, HS12_LEN);
        handshakeTag(2, pend_.hs1, nullptr, f, OFF_TAG, f + OFF_TAG);
        ok = derive(shared, pend_.hs1, pend_.hs2, pend_.next);
    }
    wipe(shared, sizeof(shared));
    if (!ok) {
        clearPending();
        return;
    }
    pend_.active = true;
    pend_.startedMs = nowMs;
    memcpy(res.reply, f, HS12_LEN);
    res.replyLen = HS12_LEN;
    res.kind = RxResult::Reply;
}

void SecureLink::handleHs2(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res) {
    if (len != HS12_LEN || !pend_.active) return;
    uint8_t tag[32];
    handshakeTag(2, pend_.hs1, nullptr, in, OFF_TAG, tag);
    if (!ctEqual(tag, in + OFF_TAG, MACTAG)) {
        authFails_++;
        res.kind = RxResult::AuthFailed;
        return;
    }
    uint8_t shared[32];
    Session s;
    bool ok = x25519Shared(&rng_, pend_.priv, in + OFF_PUB, shared);
    wipe(pend_.priv, sizeof(pend_.priv));
    if (ok) {
        memcpy(pend_.hs2, in, HS12_LEN);
        ok = derive(shared, pend_.hs1, pend_.hs2, s);
    }
    wipe(shared, sizeof(shared));
    if (!ok) {
        clearPending();
        return;
    }
    uint8_t* r = res.reply;
    r[0] = VERSION;
    r[1] = TYPE_HS3;
    r[2] = myId_;
    r[3] = 0;
    handshakeTag(3, pend_.hs1, pend_.hs2, r, HS_HDR, r + HS_HDR);
    res.replyLen = HS3_LEN;
    install(s, nowMs);
    clearPending();
    res.kind = RxResult::SessionUp;
}

void SecureLink::handleHs3(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res) {
    if (len != HS3_LEN || !pend_.active) return;
    uint8_t tag[32];
    handshakeTag(3, pend_.hs1, pend_.hs2, in, HS_HDR, tag);
    if (!ctEqual(tag, in + HS_HDR, MACTAG)) {
        authFails_++;
        res.kind = RxResult::AuthFailed;
        return;
    }
    install(pend_.next, nowMs);
    clearPending();
    res.kind = RxResult::SessionUp;
}

void SecureLink::handleData(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res) {
    if (len < HDR_LEN + 1 + TAG_LEN || len > MAX_FRAME) return;
    const uint8_t* sid = in + 4;
    const uint64_t ctr = getU64(in + 8);
    Session* s = nullptr;
    if (sessionUsable(cur_, nowMs) && memcmp(sid, cur_.sid, 4) == 0) {
        s = &cur_;
    } else if (prev_.valid && memcmp(sid, prev_.sid, 4) == 0) {
        s = &prev_;
    }
    if (!s) return;
    if (ctr <= s->rxLast || ctr > MAX_FRAMES_PER_SESSION) return;   // replay or out of range

    const size_t ctLen = len - HDR_LEN - TAG_LEN;
    uint8_t nonce[12];
    memcpy(nonce, sid, 4);
    memcpy(nonce + 4, in + 8, 8);
    uint8_t tag[16];
    memcpy(tag, in + HDR_LEN + ctLen, TAG_LEN);
    if (!gcm(false, s->rxKey, nonce, in, HDR_LEN, in + HDR_LEN, ctLen, res.payload, tag)) {
        authFails_++;
        res.kind = RxResult::AuthFailed;
        return;
    }
    s->rxLast = ctr;
    res.type = in[1];
    res.payloadLen = ctLen;
    res.kind = RxResult::Message;
}

size_t SecureLink::seal(uint8_t type, const void* payload, size_t len, uint8_t* out, size_t cap,
                        uint32_t nowMs) {
    if (!established(nowMs) || type < APP_TYPE_MIN || !payload || len == 0 ||
        len > MAX_PAYLOAD || !out || cap < HDR_LEN + len + TAG_LEN)
        return 0;
    if (cur_.txCtr >= MAX_FRAMES_PER_SESSION) return 0;
    const uint64_t ctr = ++cur_.txCtr;
    out[0] = VERSION;
    out[1] = type;
    out[2] = myId_;
    out[3] = 0;
    memcpy(out + 4, cur_.sid, 4);
    putU64(out + 8, ctr);
    uint8_t nonce[12];
    memcpy(nonce, cur_.sid, 4);
    putU64(nonce + 4, ctr);
    if (!gcm(true, cur_.txKey, nonce, out, HDR_LEN, (const uint8_t*)payload, len, out + HDR_LEN,
             out + HDR_LEN + len))
        return 0;
    return HDR_LEN + len + TAG_LEN;
}

}  // namespace slink
