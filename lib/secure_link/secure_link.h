// Secure link between the exterior and interior modules (docs/DESIGN.md section 4).
//
// Transport agnostic: feed received bytes into onFrame(), send whatever it puts
// in RxResult::reply, and use seal() to build outgoing application frames.
// Single-threaded: call everything from the same task.
//
// Primitives (mbedTLS): X25519, HMAC-SHA256, HKDF-SHA256 (RFC 5869, built on
// HMAC because MBEDTLS_HKDF_C is off in the Arduino SDK), AES-256-GCM.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace slink {

constexpr uint8_t VERSION = 0x01;
constexpr uint8_t ID_EXTERIOR = 0x01;
constexpr uint8_t ID_INTERIOR = 0x02;

// Frame types 0x10-0x1F are reserved for the handshake. Application types
// must be >= APP_TYPE_MIN.
constexpr uint8_t TYPE_HS1 = 0x10;
constexpr uint8_t TYPE_HS2 = 0x11;
constexpr uint8_t TYPE_HS3 = 0x12;
constexpr uint8_t APP_TYPE_MIN = 0x20;

constexpr size_t PSK_LEN = 32;
constexpr size_t MAC_LEN = 6;
constexpr size_t HDR_LEN = 16;
constexpr size_t TAG_LEN = 16;
constexpr size_t MAX_PAYLOAD = 64;
constexpr size_t MAX_FRAME = HDR_LEN + MAX_PAYLOAD + TAG_LEN;   // 96, under ESP-NOW's 250

constexpr uint32_t REKEY_INTERVAL_MS = 10UL * 60 * 1000;      // exterior starts a rekey
constexpr uint32_t MAX_SESSION_AGE_MS = 20UL * 60 * 1000;     // hard limit, both sides
constexpr uint64_t MAX_FRAMES_PER_SESSION = 1ULL << 20;
constexpr uint32_t PREV_SESSION_GRACE_MS = 5000;              // old session still accepted on rx
constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 3000;
constexpr uint32_t HS1_MIN_INTERVAL_MS = 2000;                // interior rate limit

enum class Role : uint8_t { Exterior, Interior };

// Must fill buf with cryptographically secure random bytes.
typedef void (*RngFn)(uint8_t* buf, size_t len);

struct RxResult {
    enum Kind : uint8_t {
        Dropped,      // not for us, malformed, replayed, or unknown session
        AuthFailed,   // failed a MAC / GCM tag check (counted, see takeAuthFailures)
        Reply,        // handshake step: send `reply`
        SessionUp,    // new session installed (send `reply` too if replyLen > 0)
        Message,      // authenticated application message in payload
    };
    Kind kind = Dropped;
    uint8_t type = 0;
    uint8_t payload[MAX_PAYLOAD];
    size_t payloadLen = 0;
    uint8_t reply[MAX_FRAME];
    size_t replyLen = 0;
};

class SecureLink {
public:
    SecureLink() = default;
    ~SecureLink();
    SecureLink(const SecureLink&) = delete;
    SecureLink& operator=(const SecureLink&) = delete;

    bool begin(Role role, const uint8_t psk[PSK_LEN], const uint8_t macExterior[MAC_LEN],
               const uint8_t macInterior[MAC_LEN], RngFn rng);

    // Exterior only. Writes HS1 into out, returns its length (0 on error).
    // The current session (if any) stays usable until the new one is up.
    size_t startHandshake(uint8_t* out, size_t cap, uint32_t nowMs);

    void onFrame(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res);

    // Encrypts one application message. Returns frame length, 0 if there is no
    // usable session or the arguments are invalid.
    size_t seal(uint8_t type, const void* payload, size_t len, uint8_t* out, size_t cap,
                uint32_t nowMs);

    bool established(uint32_t nowMs) const;
    bool rekeyDue(uint32_t nowMs) const;
    void reset();   // drop sessions and any handshake in progress, zeroize keys
    uint32_t takeAuthFailures();

private:
    struct Session {
        bool valid;
        uint8_t sid[4];
        uint8_t txKey[32];
        uint8_t rxKey[32];
        uint64_t txCtr;
        uint64_t rxLast;
        uint32_t createdMs;
        uint32_t retiredMs;
    };
    struct Pending {
        bool active;
        uint32_t startedMs;
        uint8_t priv[32];   // exterior: ephemeral private key until HS2 arrives
        uint8_t hs1[84];
        uint8_t hs2[84];    // interior: kept to verify HS3
        Session next;       // interior: keys ready, installed on HS3
    };

    void handleHs1(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res);
    void handleHs2(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res);
    void handleHs3(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res);
    void handleData(const uint8_t* in, size_t len, uint32_t nowMs, RxResult& res);
    bool derive(const uint8_t shared[32], const uint8_t* hs1, const uint8_t* hs2,
                Session& out) const;
    void install(Session& s, uint32_t nowMs);
    void clearPending();
    bool sessionUsable(const Session& s, uint32_t nowMs) const;
    void handshakeTag(uint8_t step, const uint8_t* hs1, const uint8_t* hs2,
                      const uint8_t* own, size_t ownLen, uint8_t out[32]) const;

    bool ready_ = false;
    Role role_ = Role::Exterior;
    uint8_t myId_ = 0;
    uint8_t peerId_ = 0;
    uint8_t psk_[PSK_LEN] = {};
    uint8_t macE_[MAC_LEN] = {};
    uint8_t macI_[MAC_LEN] = {};
    RngFn rng_ = nullptr;
    Session cur_ = {};
    Session prev_ = {};
    Pending pend_ = {};
    bool hs1Seen_ = false;
    uint32_t lastHs1Ms_ = 0;
    uint32_t authFails_ = 0;
};

}  // namespace slink
