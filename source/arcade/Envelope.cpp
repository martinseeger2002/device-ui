#include "arcade/Envelope.h"
#include "arcade/JsonLite.h"

#include <string.h>

extern "C" {
#include "vendor/trezor/blake2b.h"
#include "vendor/tweetnacl/tweetnacl.h"
}

namespace arcade
{

static const uint8_t BOUND[6] = {'a', 'r', 'c', 'm', 0x01, 0x01}; // magic, version, TYPE_SINGLE

// NaCl's crypto_box wants 32 zero bytes before the message and gives back
// 16 zero bytes before the MAC; these hide that.
static bool box(const uint8_t *msg, size_t len, const uint8_t nonce[24], const uint8_t pk[32], const uint8_t sk[32],
                Bytes &out)
{
    Bytes m(32 + len, 0), c(32 + len);
    memcpy(m.data() + 32, msg, len);
    if (crypto_box(c.data(), m.data(), m.size(), nonce, pk, sk) != 0)
        return false;
    out.assign(c.begin() + 16, c.end());
    wipe(m.data(), m.size());
    return true;
}

static bool unbox(const uint8_t *c, size_t len, const uint8_t nonce[24], const uint8_t pk[32], const uint8_t sk[32],
                  Bytes &out)
{
    if (len < 16)
        return false;
    Bytes cc(16 + len, 0), m(16 + len);
    memcpy(cc.data() + 16, c, len);
    if (crypto_box_open(m.data(), cc.data(), cc.size(), nonce, pk, sk) != 0)
        return false;
    out.assign(m.begin() + 32, m.end());
    wipe(m.data(), m.size());
    return true;
}

static void sealNonce(const uint8_t eph[32], const uint8_t recipient[32], uint8_t nonce[24])
{
    uint8_t both[64];
    memcpy(both, eph, 32);
    memcpy(both + 32, recipient, 32);
    blake2b(both, 64, nonce, 24); // libsodium crypto_box_seal: blake2b-192(eph_pk || recipient_pk)
}

bool sealedBox(const uint8_t recipientPk[32], const Bytes &msg, Bytes &out)
{
    uint8_t esk[32], epk[32], nonce[24];
    randomBytes(esk, 32);
    crypto_scalarmult_base(epk, esk);
    sealNonce(epk, recipientPk, nonce);
    Bytes c;
    bool ok = box(msg.data(), msg.size(), nonce, recipientPk, esk, c);
    wipe(esk, 32);
    if (!ok)
        return false;
    out.assign(epk, epk + 32);
    out.insert(out.end(), c.begin(), c.end());
    return true;
}

bool sealedBoxOpen(const uint8_t recipientSk[32], const uint8_t recipientPk[32], const Bytes &c, Bytes &out)
{
    if (c.size() < 32 + 16)
        return false;
    uint8_t nonce[24];
    sealNonce(c.data(), recipientPk, nonce);
    return unbox(c.data() + 32, c.size() - 32, nonce, c.data(), recipientSk, out);
}

Bytes sealMessage(const Account &me, const uint8_t theirMailKey[32], const std::string &text)
{
    Bytes plain(BOUND, BOUND + 6);
    plain.insert(plain.end(), text.begin(), text.end());
    uint8_t nonce[24];
    randomBytes(nonce, 24);
    Bytes inner;
    if (!box(plain.data(), plain.size(), nonce, theirMailKey, me.mailSecret(), inner))
        return Bytes();
    Bytes envelope(me.mailPublic(), me.mailPublic() + 32);
    envelope.insert(envelope.end(), nonce, nonce + 24);
    envelope.insert(envelope.end(), inner.begin(), inner.end());
    Bytes sealed;
    if (!sealedBox(theirMailKey, envelope, sealed))
        return Bytes();
    return sealed;
}

Opened openMessage(const Account &me, const Bytes &sealed)
{
    Opened o;
    Bytes envelope, plain;
    if (!sealedBoxOpen(me.mailSecret(), me.mailPublic(), sealed, envelope) || envelope.size() < 32 + 24 + 16)
        return o; // not for us, or damaged
    memcpy(o.sender, envelope.data(), 32);
    if (!unbox(envelope.data() + 56, envelope.size() - 56, envelope.data() + 32, o.sender, me.mailSecret(), plain))
        return o; // the sender key inside does not match who sealed it
    if (plain.size() < 6 || memcmp(plain.data(), BOUND, 6) != 0)
        return o;
    const char *body = (const char *)plain.data() + 6;
    size_t len = plain.size() - 6;
    // rich bodies (messaging_js.js): "\x01ARCB" version(1) jsonlen(u16 BE) JSON {text, file?, profile?} file bytes
    if (len >= 8 && memcmp(body, "\x01" "ARCB", 5) == 0) {
        size_t jlen = (size_t)((uint8_t)body[6] << 8 | (uint8_t)body[7]);
        if (8 + jlen > len)
            return o;
        Json j{body + 8, body + 8 + jlen};
        j.object([&](const std::string &k) {
            if (k == "text")
                o.text = j.str();
            else if (k == "file") {
                j.ws();
                o.hadFile = j.p < j.end && *j.p != 'n'; // "file": null is no file
                j.skip();
            } else
                j.skip();
        });
        if (!j.ok)
            return o;
    } else {
        o.text.assign(body, len);
    }
    o.ok = true;
    return o;
}

} // namespace arcade
