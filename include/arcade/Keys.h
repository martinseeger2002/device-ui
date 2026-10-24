#pragma once

// The keys an arcade account's twelve words make, exactly as the arcade's
// browser makes them (signin.js, coins.js, messaging_js.js). The Python
// reference is gateway/arcade_lora/device_keys.py; tests hold both to the
// same vectors.

#include "arcade/Crypto.h"
#include <string>

namespace arcade
{

// lowercase, single spaces, no ends; the wordlist is ASCII so NFKD is a no-op
std::string normalisePhrase(const std::string &phrase);
// every word on the list and the checksum right
bool phraseValid(const std::string &phrase);
// words starting with `prefix`, at most `max` of them (for typing help)
std::vector<const char *> wordsStartingWith(const std::string &prefix, size_t max);
// a fresh 12-word phrase
std::string newPhrase(void);

// coins.js COIN_TYPE: main/doge/ltc 3, everything else 1
uint32_t coinTypeFor(const std::string &network);

class Account
{
  public:
    explicit Account(const std::string &phrase);
    ~Account();

    const uint8_t *loginPublic(void) const { return loginPk; }
    const uint8_t *mailPublic(void) const { return mailPk; }
    const uint8_t *mailSecret(void) const { return mailSk; }

    void signLogin(const std::string &origin, const std::string &nonce, uint8_t sig[64]) const;
    void signEd25519(const uint8_t *msg, size_t len, uint8_t sig[64]) const;

    // the m/44'/<coin>'/0'/0/0 key for this network
    bool coinPublic(const std::string &network, uint8_t pub33[33]) const;
    std::string address(const std::string &network, uint8_t version) const;
    // DER + SIGHASH_ALL byte; 0 on failure
    size_t signDigest(const std::string &network, const uint8_t digest[32], uint8_t out[73]) const;

  private:
    bool coinPrivate(uint32_t coinType, uint8_t priv[32]) const;

    uint8_t seed[64];
    uint8_t loginPk[32];
    uint8_t loginSk[64];
    uint8_t mailSk[32];
    uint8_t mailPk[32];
};

// the arcade branch: hardened steps keeping IL as the key (not real BIP32)
void arcadeBranch(const uint8_t seed[64], const uint32_t *path, size_t n, uint8_t out[32]);
// real BIP32 private derivation
bool bip32Private(const uint8_t seed[64], const uint32_t *path, size_t n, uint8_t out[32]);
void phraseSeed(const std::string &phrase, const std::string &passphrase, uint8_t seed[64]);

} // namespace arcade
