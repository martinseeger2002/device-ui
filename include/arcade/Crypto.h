#pragma once

// The handful of primitives an arcade T-Deck needs, behind one small API so
// the vendored libraries (trezor-crypto hashes, micro-ecc, TweetNaCl) stay
// out of the rest of the code. Everything here is plain C++ and builds on the
// host for tests as well as on the ESP32.

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace arcade
{

using Bytes = std::vector<uint8_t>;

void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void dsha256(const uint8_t *data, size_t len, uint8_t out[32]);
void sha512(const uint8_t *data, size_t len, uint8_t out[64]);
void hash160(const uint8_t *data, size_t len, uint8_t out[20]);
void hmacSha512(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[64]);
void pbkdf2Sha512(const uint8_t *pass, size_t passLen, const uint8_t *salt, size_t saltLen, uint32_t rounds,
                  uint8_t *out, size_t outLen);

std::string base58Check(uint8_t version, const uint8_t *payload, size_t len);
// false on a bad character, length or checksum
bool base58CheckDecode(const std::string &text, uint8_t &version, uint8_t payload20[20]);

// secp256k1 (coins)
bool secpPublicKey(const uint8_t priv[32], uint8_t pub33[33]);
// (a + b) mod n; false when the result is zero (BIP32's "invalid child")
bool secpAddModN(const uint8_t a[32], const uint8_t b[32], uint8_t out[32]);
// DER signature with low S, RFC 6979 nonce; returns its length (<= 72), 0 on failure
size_t secpSignDer(const uint8_t priv[32], const uint8_t digest[32], uint8_t der[72]);
bool secpVerifyDer(const uint8_t pub33[33], const uint8_t digest[32], const uint8_t *der, size_t len);

// Ed25519 (login) and X25519 (mail)
void edKeypairFromSeed(const uint8_t seed[32], uint8_t pk[32], uint8_t sk[64]);
void edSign(const uint8_t sk[64], const uint8_t *msg, size_t len, uint8_t sig[64]);
void x25519Public(const uint8_t secret[32], uint8_t pub[32]);

// secretbox (XSalsa20-Poly1305) for the key store: out = mac16 || cipher
bool secretboxSeal(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *msg, size_t len, Bytes &out);
bool secretboxOpen(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *box, size_t len, Bytes &out);

void randomBytes(uint8_t *out, size_t len);
void wipe(void *p, size_t len);

std::string toHex(const uint8_t *data, size_t len);

} // namespace arcade
