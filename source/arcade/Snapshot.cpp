#include "arcade/Snapshot.h"
#include "arcade/JsonLite.h"

#include <algorithm>

namespace arcade
{

bool readAccount(const char *json, size_t len, Snapshot &out)
{
    Json j{json, json + len};
    out = Snapshot();
    j.object([&](const std::string &key) {
        if (key == "tag") {
            out.tag = j.str();
        } else if (key == "chains") {
            j.ws();
            if (j.p < j.end && *j.p != '[')
                return j.skip();
            j.array([&]() {
                Chain c;
                double balance = 0, leaving = 0, held = 0, inscribed = 0, incoming = 0, spendable = -1, arriving = 0;
                j.object([&](const std::string &f) {
                    if (f == "network")
                        c.network = j.str();
                    else if (f == "label")
                        c.label = j.str();
                    else if (f == "mainnet")
                        c.mainnet = j.boolean();
                    else if (f == "tags")
                        c.tags = j.boolean();
                    else if (f == "version")
                        c.p2pkh = (uint8_t)j.num();
                    else if (f == "p2sh")
                        c.p2sh = (uint8_t)j.num();
                    else if (f == "balance")
                        balance = j.num();
                    else if (f == "leaving")
                        leaving = j.num();
                    else if (f == "held")
                        held = j.num();
                    else if (f == "inscribed")
                        inscribed = j.num();
                    else if (f == "incoming")
                        incoming = j.num();
                    else if (f == "spendable")
                        spendable = j.num();
                    else if (f == "arriving")
                        arriving = j.num();
                    else if (f == "address")
                        c.registered = !j.str().empty();
                    else
                        j.skip();
                });
                // the web wallet's big number (app.py _spendable), and what is still arriving
                if (spendable < 0) {
                    spendable = balance - leaving - held - inscribed + incoming;
                    if (spendable < 0)
                        spendable = 0;
                }
                c.confirmed = (uint64_t)spendable;
                c.pending = (uint64_t)arriving;
                if (!c.network.empty())
                    out.chains.push_back(c);
            });
        } else {
            j.skip();
        }
    });
    // the tag chain first, as the gateway lists them
    std::stable_sort(out.chains.begin(), out.chains.end(), [](const Chain &a, const Chain &b) { return a.tags > b.tags; });
    out.ok = j.ok && !out.chains.empty();
    return out.ok;
}

static bool hex32(const std::string &h, uint8_t out[32])
{
    if (h.size() != 64)
        return false;
    for (int i = 0; i < 32; i++) {
        char b[3] = {h[2 * i], h[2 * i + 1], 0};
        char *stop;
        out[i] = (uint8_t)strtoul(b, &stop, 16);
        if (*stop)
            return false;
    }
    return true;
}

bool readCoins(const char *json, size_t len, std::vector<Coin> &out)
{
    Json j{json, json + len};
    out.clear();
    j.object([&](const std::string &key) {
        if (key != "coins")
            return j.skip();
        j.array([&]() {
            Coin c;
            bool good = false;
            j.object([&](const std::string &f) {
                if (f == "txid")
                    good = hex32(j.str(), c.txid);
                else if (f == "vout")
                    c.vout = (uint32_t)j.num();
                else if (f == "value")
                    c.value = (uint64_t)j.num();
                else
                    j.skip();
            });
            if (good)
                out.push_back(c);
        });
    });
    // the biggest first: an offline payment wants the fewest inputs
    std::sort(out.begin(), out.end(), [](const Coin &a, const Coin &b) { return a.value > b.value; });
    return j.ok;
}

} // namespace arcade
