#include "arcade/Crypto.h"

#include <string.h>

extern "C" {
#include "vendor/trezor/hmac.h"
#include "vendor/trezor/memzero.h"
#include "vendor/trezor/pbkdf2.h"
#include "vendor/trezor/ripemd160.h"
#include "vendor/trezor/sha2.h"
#include "vendor/tweetnacl/tweetnacl.h"
#include "vendor/uecc/uECC.h"
int arcade_sign_seed_keypair(unsigned char *pk, unsigned char *sk, const unsigned char *seed);
}

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_random.h>
#else
#include <stdio.h>
#endif

namespace arcade
{

// ---- randomness -------------------------------------------------------------

void randomBytes(uint8_t *out, size_t len)
{
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    esp_fill_random(out, len);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(out, 1, len, f) : 0;
    if (f)
        fclose(f);
    if (got != len)
        memset(out, 0, len); // only on a host without /dev/urandom; tests notice
#endif
}

void wipe(void *p, size_t len)
{
    memzero(p, len);
}

} // namespace arcade

// TweetNaCl (renamed from randombytes: libsodium in the ESP32 core has one) and micro-ecc want a random source.
extern "C" void arcade_randombytes(unsigned char *out, unsigned long long len)
{
    arcade::randomBytes(out, (size_t)len);
}

static int uecc_rng(uint8_t *dest, unsigned size)
{
    arcade::randomBytes(dest, size);
    return 1;
}

namespace arcade
{

// ---- hashes -----------------------------------------------------------------

void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_Raw(data, len, out);
}

void dsha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint8_t once[32];
    sha256_Raw(data, len, once);
    sha256_Raw(once, 32, out);
}

void sha512(const uint8_t *data, size_t len, uint8_t out[64])
{
    sha512_Raw(data, len, out);
}

void hash160(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint8_t h[32];
    sha256_Raw(data, len, h);
    ripemd160(h, 32, out);
}

void hmacSha512(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[64])
{
    hmac_sha512(key, (uint32_t)keyLen, msg, (uint32_t)msgLen, out);
}

void pbkdf2Sha512(const uint8_t *pass, size_t passLen, const uint8_t *salt, size_t saltLen, uint32_t rounds,
                  uint8_t *out, size_t outLen)
{
    // trezor's pbkdf2 takes the salt without the block counter and fills one
    // 64-byte block per call; the arcade only ever asks for 64 bytes or fewer.
    uint8_t block[64];
    pbkdf2_hmac_sha512(pass, (int)passLen, salt, (int)saltLen, rounds, block, sizeof(block));
    memcpy(out, block, outLen < 64 ? outLen : 64);
    memzero(block, sizeof(block));
}

// ---- base58 -----------------------------------------------------------------

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::string base58Check(uint8_t version, const uint8_t *payload, size_t len)
{
    Bytes raw;
    raw.push_back(version);
    raw.insert(raw.end(), payload, payload + len);
    uint8_t check[32];
    dsha256(raw.data(), raw.size(), check);
    raw.insert(raw.end(), check, check + 4);

    size_t zeros = 0;
    while (zeros < raw.size() && raw[zeros] == 0)
        zeros++;
    std::vector<uint8_t> digits; // base 58, little end first
    for (uint8_t byte : raw) {
        int carry = byte;
        for (auto &d : digits) {
            carry += d * 256;
            d = carry % 58;
            carry /= 58;
        }
        while (carry) {
            digits.push_back(carry % 58);
            carry /= 58;
        }
    }
    std::string out(zeros, '1');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        out += B58[*it];
    return out;
}

bool base58CheckDecode(const std::string &text, uint8_t &version, uint8_t payload20[20])
{
    std::vector<uint8_t> bytes; // base 256, little end first
    size_t ones = 0;
    while (ones < text.size() && text[ones] == '1')
        ones++;
    for (char c : text) {
        const char *at = strchr(B58, c);
        if (!at || !c)
            return false;
        int carry = (int)(at - B58);
        for (auto &b : bytes) {
            carry += b * 58;
            b = carry & 0xff;
            carry >>= 8;
        }
        while (carry) {
            bytes.push_back(carry & 0xff);
            carry >>= 8;
        }
    }
    Bytes raw(ones, 0);
    raw.insert(raw.end(), bytes.rbegin(), bytes.rend());
    if (raw.size() != 25)
        return false;
    uint8_t check[32];
    dsha256(raw.data(), 21, check);
    if (memcmp(check, raw.data() + 21, 4) != 0)
        return false;
    version = raw[0];
    memcpy(payload20, raw.data() + 1, 20);
    return true;
}

// ---- secp256k1 --------------------------------------------------------------

static const uint8_t N[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48,
                              0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41};
static const uint8_t HALF_N[32] = {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x5D, 0x57, 0x6E, 0x73, 0x57, 0xA4,
                                   0x50, 0x1D, 0xDF, 0xE9, 0x2F, 0x46, 0x68, 0x1B, 0x20, 0xA0};

static const struct uECC_Curve_t *curve()
{
    static bool rng = false;
    if (!rng) {
        uECC_set_rng(&uecc_rng);
        rng = true;
    }
    return uECC_secp256k1();
}

static int cmp32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32);
}

static void sub32(const uint8_t *a, const uint8_t *b, uint8_t *out) // a - b, a >= b
{
    int borrow = 0;
    for (int i = 31; i >= 0; i--) {
        int d = (int)a[i] - b[i] - borrow;
        borrow = d < 0;
        out[i] = (uint8_t)(d + (borrow ? 256 : 0));
    }
}

bool secpAddModN(const uint8_t a[32], const uint8_t b[32], uint8_t out[32])
{
    uint8_t sum[32];
    int carry = 0;
    for (int i = 31; i >= 0; i--) {
        int s = a[i] + b[i] + carry;
        sum[i] = (uint8_t)s;
        carry = s >> 8;
    }
    if (carry || cmp32(sum, N) >= 0)
        sub32(sum, N, sum); // a, b < n so one subtraction is enough (carry wraps correctly)
    memcpy(out, sum, 32);
    static const uint8_t zero[32] = {0};
    return memcmp(out, zero, 32) != 0;
}

bool secpPublicKey(const uint8_t priv[32], uint8_t pub33[33])
{
    uint8_t pub64[64];
    if (!uECC_compute_public_key(priv, pub64, curve()))
        return false;
    uECC_compress(pub64, pub33, curve());
    return true;
}

struct Sha256HashCtx {
    uECC_HashContext uecc;
    SHA256_CTX ctx;
    uint8_t tmp[32 * 2 + 64];
};

static void hctxInit(const uECC_HashContext *base)
{
    sha256_Init(&((Sha256HashCtx *)base)->ctx);
}
static void hctxUpdate(const uECC_HashContext *base, const uint8_t *message, unsigned len)
{
    sha256_Update(&((Sha256HashCtx *)base)->ctx, message, len);
}
static void hctxFinish(const uECC_HashContext *base, uint8_t *hash)
{
    sha256_Final(&((Sha256HashCtx *)base)->ctx, hash);
}

static size_t derInt(const uint8_t *v, uint8_t *out)
{
    int skip = 0;
    while (skip < 31 && v[skip] == 0)
        skip++;
    bool pad = v[skip] & 0x80;
    size_t len = 32 - skip + (pad ? 1 : 0);
    out[0] = 0x02;
    out[1] = (uint8_t)len;
    size_t at = 2;
    if (pad)
        out[at++] = 0;
    memcpy(out + at, v + skip, 32 - skip);
    return 2 + len;
}

size_t secpSignDer(const uint8_t priv[32], const uint8_t digest[32], uint8_t der[72])
{
    Sha256HashCtx h;
    h.uecc.init_hash = hctxInit;
    h.uecc.update_hash = hctxUpdate;
    h.uecc.finish_hash = hctxFinish;
    h.uecc.block_size = 64;
    h.uecc.result_size = 32;
    h.uecc.tmp = h.tmp;
    uint8_t rs[64];
    if (!uECC_sign_deterministic(priv, digest, 32, &h.uecc, rs, curve()))
        return 0;
    if (cmp32(rs + 32, HALF_N) > 0)
        sub32(N, rs + 32, rs + 32); // low S (BIP 62): nodes refuse the high half
    uint8_t body[70];
    size_t n = derInt(rs, body);
    n += derInt(rs + 32, body + n);
    der[0] = 0x30;
    der[1] = (uint8_t)n;
    memcpy(der + 2, body, n);
    memzero(&h, sizeof(h));
    return n + 2;
}

static bool readDerInt(const uint8_t *&p, const uint8_t *end, uint8_t out[32])
{
    if (end - p < 2 || p[0] != 0x02)
        return false;
    size_t len = p[1];
    p += 2;
    if ((size_t)(end - p) < len || len == 0 || len > 33)
        return false;
    const uint8_t *v = p;
    if (len == 33) {
        v++;
        len--;
    }
    memset(out, 0, 32);
    memcpy(out + 32 - len, v, len);
    p += (v - p) + len;
    return true;
}

bool secpVerifyDer(const uint8_t pub33[33], const uint8_t digest[32], const uint8_t *der, size_t len)
{
    if (len < 8 || der[0] != 0x30 || der[1] + 2u != len)
        return false;
    const uint8_t *p = der + 2, *end = der + len;
    uint8_t rs[64], pub64[64];
    if (!readDerInt(p, end, rs) || !readDerInt(p, end, rs + 32) || p != end)
        return false;
    uECC_decompress(pub33, pub64, curve());
    return uECC_verify(pub64, digest, 32, rs, curve()) == 1;
}

// ---- Ed25519 / X25519 / secretbox ----------------------------------------------

void edKeypairFromSeed(const uint8_t seed[32], uint8_t pk[32], uint8_t sk[64])
{
    arcade_sign_seed_keypair(pk, sk, seed);
}

void edSign(const uint8_t sk[64], const uint8_t *msg, size_t len, uint8_t sig[64])
{
    Bytes sm(len + 64);
    unsigned long long smlen = 0;
    crypto_sign(sm.data(), &smlen, msg, len, sk);
    memcpy(sig, sm.data(), 64);
}

void x25519Public(const uint8_t secret[32], uint8_t pub[32])
{
    crypto_scalarmult_base(pub, secret);
}

bool secretboxSeal(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *msg, size_t len, Bytes &out)
{
    Bytes m(32 + len, 0), c(32 + len);
    memcpy(m.data() + 32, msg, len);
    if (crypto_secretbox(c.data(), m.data(), m.size(), nonce, key) != 0)
        return false;
    out.assign(c.begin() + 16, c.end());
    memzero(m.data(), m.size());
    return true;
}

bool secretboxOpen(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *box, size_t len, Bytes &out)
{
    if (len < 16)
        return false;
    Bytes c(16 + len, 0), m(16 + len);
    memcpy(c.data() + 16, box, len);
    if (crypto_secretbox_open(m.data(), c.data(), c.size(), nonce, key) != 0)
        return false;
    out.assign(m.begin() + 32, m.end());
    memzero(m.data(), m.size());
    return true;
}

std::string toHex(const uint8_t *data, size_t len)
{
    static const char *hex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s += hex[data[i] >> 4];
        s += hex[data[i] & 15];
    }
    return s;
}

} // namespace arcade
