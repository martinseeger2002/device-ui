#pragma once

// Arcade messages, sealed and opened on the T-Deck exactly as
// arcade/messaging/envelope.py does it:
//   inner    = crypto_box(my mail key -> theirs, "arcm" 01 01 || body)   nonce24 || mac16+cipher
//   envelope = my mail public key (32) || inner
//   sealed   = crypto_box_seal(their mail key, envelope)                eph_pk32 || mac16+cipher
// The gateway's arcade adds the 8-byte on-chain header; the T-Deck handles
// only the sealed part. Overhead is 126 bytes on top of the body.

#include "arcade/Keys.h"

namespace arcade
{

constexpr size_t SEALED_OVERHEAD = 48 + 32 + 40 + 6;

Bytes sealMessage(const Account &me, const uint8_t theirMailKey[32], const std::string &text);

struct Opened {
    bool ok = false;
    uint8_t sender[32] = {0};   // the sender's mail key: match it against the address book
    std::string text;           // plain text, or what a rich body's "text" said
    bool hadFile = false;       // pictures and files are not shown on the T-Deck
};
Opened openMessage(const Account &me, const Bytes &sealed);

// the libsodium constructions, for tests
bool sealedBox(const uint8_t recipientPk[32], const Bytes &msg, Bytes &out);
bool sealedBoxOpen(const uint8_t recipientSk[32], const uint8_t recipientPk[32], const Bytes &c, Bytes &out);

} // namespace arcade
