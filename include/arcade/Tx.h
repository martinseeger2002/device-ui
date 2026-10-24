#pragma once

// Checking an OFFER before signing it (the browser's verifyOffer, and
// gateway/arcade_lora/txcheck.py). The device never signs a digest it was
// handed: it parses the transaction, checks every output, and works out each
// SIGHASH_ALL digest itself over its own P2PKH script.

#include "arcade/Crypto.h"
#include <array>
#include <string>

namespace arcade
{

struct TxIn {
    uint8_t txid[32]; // display order
    uint32_t vout;
    Bytes script;
    uint32_t sequence;
};

struct TxOut {
    uint64_t value;
    Bytes script;
};

struct Tx {
    int32_t version = 1;
    std::vector<TxIn> ins;
    std::vector<TxOut> outs;
    uint32_t locktime = 0;

    bool parse(const Bytes &raw);                          // false on anything malformed (or segwit)
    Bytes serialize(const std::vector<Bytes> *scripts = nullptr) const;
    void sighashAll(size_t index, const Bytes &scriptCode, uint8_t out[32]) const;
    void txid(uint8_t out[32]) const;                      // display order
};

Bytes p2pkhScript(const uint8_t h160[20]);
Bytes p2shScript(const uint8_t h160[20]);
// the output script for an address on a chain with these version bytes; empty if it is not one
Bytes scriptFor(const std::string &address, uint8_t p2pkhVersion, uint8_t p2shVersion);

struct Checked {
    bool ok = false;
    std::string why;            // why not, for the screen
    std::vector<std::array<uint8_t, 32>> digests;
    uint64_t paid = 0, change = 0, fee = 0;
};

// A coin send passes payScript and amount; a mail post passes an empty
// payScript and the most it may pay to others (marker, packets, notice).
Checked checkOffer(const Bytes &raw, const std::vector<uint64_t> &inputValues, const uint8_t ownPub33[33],
                   const Bytes &payScript, uint64_t amount, uint64_t feeCap, uint64_t othersCap = 0);

// a signed P2PKH transaction (for offline payments handed over the mesh)
Bytes assemble(const Bytes &raw, const std::vector<Bytes> &sigs, const uint8_t pub33[33]);

} // namespace arcade
