#pragma once

// What a WiFi sync brings besides the address book: the account's @tag, its
// chains and balances (GET /account) and its spendable coins on each chain
// (GET /account/coins). Kept on the T-Deck so the wallet still shows, and
// offline payments can be built, with no gateway in range.

#include "arcade/ArcadeClient.h"

namespace arcade
{

struct Coin {
    uint8_t txid[32]; // display order
    uint32_t vout = 0;
    uint64_t value = 0;
};

struct Snapshot {
    bool ok = false;
    std::string tag;
    std::vector<Chain> chains;
};

bool readAccount(const char *json, size_t len, Snapshot &out);
bool readCoins(const char *json, size_t len, std::vector<Coin> &out);

} // namespace arcade
