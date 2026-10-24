#include "arcade/Keys.h"
#include "arcade/Bip39Words.h"

#include <ctype.h>
#include <string.h>

namespace arcade
{

static const uint32_t H = 0x80000000u;
static const uint32_t LOGIN_BRANCH[3] = {24946 | H, 1 | H, 0 | H};
static const uint32_t MAIL_BRANCH[3] = {24946 | H, 0 | H, 0 | H};

std::string normalisePhrase(const std::string &phrase)
{
    std::string out;
    bool space = false;
    for (char c : phrase) {
        if (isspace((unsigned char)c)) {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += (char)tolower((unsigned char)c);
    }
    return out;
}

static int wordIndex(const std::string &w)
{
    int lo = 0, hi = 2047; // the list is sorted
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(w.c_str(), BIP39_WORDS[mid]);
        if (c == 0)
            return mid;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}

bool phraseValid(const std::string &phrase)
{
    std::string p = normalisePhrase(phrase);
    std::vector<int> idx;
    size_t at = 0;
    while (at <= p.size()) {
        size_t sp = p.find(' ', at);
        if (sp == std::string::npos)
            sp = p.size();
        int i = wordIndex(p.substr(at, sp - at));
        if (i < 0)
            return false;
        idx.push_back(i);
        at = sp + 1;
    }
    size_t n = idx.size();
    if (n < 12 || n > 24 || n % 3)
        return false;
    size_t bits = n * 11, entBits = bits * 32 / 33, csBits = bits - entBits;
    std::vector<uint8_t> all((bits + 7) / 8, 0);
    for (size_t w = 0; w < n; w++)
        for (int b = 0; b < 11; b++)
            if (idx[w] & (1 << (10 - b))) {
                size_t pos = w * 11 + b;
                all[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
            }
    uint8_t h[32];
    sha256(all.data(), entBits / 8, h);
    for (size_t b = 0; b < csBits; b++) {
        size_t pos = entBits + b;
        bool want = h[b / 8] & (0x80 >> (b % 8));
        bool got = all[pos / 8] & (0x80 >> (pos % 8));
        if (want != got)
            return false;
    }
    return true;
}

std::vector<const char *> wordsStartingWith(const std::string &prefix, size_t max)
{
    std::vector<const char *> out;
    for (int i = 0; i < 2048 && out.size() < max; i++)
        if (strncmp(BIP39_WORDS[i], prefix.c_str(), prefix.size()) == 0)
            out.push_back(BIP39_WORDS[i]);
    return out;
}

std::string newPhrase(void)
{
    uint8_t ent[17];
    randomBytes(ent, 16);
    uint8_t h[32];
    sha256(ent, 16, h);
    ent[16] = h[0];
    std::string out;
    for (int w = 0; w < 12; w++) {
        int v = 0;
        for (int b = 0; b < 11; b++) {
            int pos = w * 11 + b;
            v = (v << 1) | ((ent[pos / 8] >> (7 - pos % 8)) & 1);
        }
        if (w)
            out += ' ';
        out += BIP39_WORDS[v];
    }
    wipe(ent, sizeof(ent));
    return out;
}

uint32_t coinTypeFor(const std::string &network)
{
    if (network == "main" || network == "doge" || network == "ltc")
        return 3;
    return 1;
}

void phraseSeed(const std::string &phrase, const std::string &passphrase, uint8_t seed[64])
{
    std::string p = normalisePhrase(phrase), salt = "mnemonic" + passphrase;
    pbkdf2Sha512((const uint8_t *)p.data(), p.size(), (const uint8_t *)salt.data(), salt.size(), 2048, seed, 64);
    wipe(&p[0], p.size());
}

static void master(const uint8_t seed[64], uint8_t key[32], uint8_t chain[32])
{
    uint8_t i[64];
    hmacSha512((const uint8_t *)"Bitcoin seed", 12, seed, 64, i);
    memcpy(key, i, 32);
    memcpy(chain, i + 32, 32);
    wipe(i, sizeof(i));
}

void arcadeBranch(const uint8_t seed[64], const uint32_t *path, size_t n, uint8_t out[32])
{
    uint8_t key[32], chain[32], data[37], i[64];
    master(seed, key, chain);
    for (size_t s = 0; s < n; s++) {
        data[0] = 0;
        memcpy(data + 1, key, 32);
        data[33] = path[s] >> 24;
        data[34] = path[s] >> 16;
        data[35] = path[s] >> 8;
        data[36] = path[s];
        hmacSha512(chain, 32, data, 37, i);
        memcpy(key, i, 32);
        memcpy(chain, i + 32, 32);
    }
    memcpy(out, key, 32);
    wipe(key, 32);
    wipe(chain, 32);
    wipe(data, sizeof(data));
    wipe(i, sizeof(i));
}

bool bip32Private(const uint8_t seed[64], const uint32_t *path, size_t n, uint8_t out[32])
{
    uint8_t key[32], chain[32], data[37], i[64];
    bool ok = true;
    master(seed, key, chain);
    for (size_t s = 0; s < n && ok; s++) {
        if (path[s] & H) {
            data[0] = 0;
            memcpy(data + 1, key, 32);
        } else {
            ok = secpPublicKey(key, data);
        }
        data[33] = path[s] >> 24;
        data[34] = path[s] >> 16;
        data[35] = path[s] >> 8;
        data[36] = path[s];
        hmacSha512(chain, 32, data, 37, i);
        ok = ok && secpAddModN(i, key, key);
        memcpy(chain, i + 32, 32);
    }
    memcpy(out, key, 32);
    wipe(key, 32);
    wipe(chain, 32);
    wipe(i, sizeof(i));
    return ok;
}

Account::Account(const std::string &phrase)
{
    phraseSeed(phrase, "", seed);
    uint8_t login[32];
    arcadeBranch(seed, LOGIN_BRANCH, 3, login);
    edKeypairFromSeed(login, loginPk, loginSk);
    wipe(login, 32);
    arcadeBranch(seed, MAIL_BRANCH, 3, mailSk);
    x25519Public(mailSk, mailPk);
}

Account::~Account()
{
    wipe(seed, sizeof(seed));
    wipe(loginSk, sizeof(loginSk));
    wipe(mailSk, sizeof(mailSk));
}

void Account::signLogin(const std::string &origin, const std::string &nonce, uint8_t sig[64]) const
{
    std::string msg = "DogecoinArcade login\n" + origin + "\n" + nonce;
    edSign(loginSk, (const uint8_t *)msg.data(), msg.size(), sig);
}

void Account::signEd25519(const uint8_t *msg, size_t len, uint8_t sig[64]) const
{
    edSign(loginSk, msg, len, sig);
}

bool Account::coinPrivate(uint32_t coinType, uint8_t priv[32]) const
{
    const uint32_t path[5] = {44 | H, coinType | H, 0 | H, 0, 0};
    return bip32Private(seed, path, 5, priv);
}

bool Account::coinPublic(const std::string &network, uint8_t pub33[33]) const
{
    uint8_t priv[32];
    bool ok = coinPrivate(coinTypeFor(network), priv) && secpPublicKey(priv, pub33);
    wipe(priv, 32);
    return ok;
}

std::string Account::address(const std::string &network, uint8_t version) const
{
    uint8_t pub[33], h[20];
    if (!coinPublic(network, pub))
        return "";
    hash160(pub, 33, h);
    return base58Check(version, h, 20);
}

size_t Account::signDigest(const std::string &network, const uint8_t digest[32], uint8_t out[73]) const
{
    uint8_t priv[32];
    if (!coinPrivate(coinTypeFor(network), priv))
        return 0;
    size_t n = secpSignDer(priv, digest, out);
    wipe(priv, 32);
    if (!n)
        return 0;
    out[n] = 0x01; // SIGHASH_ALL
    return n + 1;
}

} // namespace arcade
