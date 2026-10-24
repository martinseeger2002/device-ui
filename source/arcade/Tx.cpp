#include "arcade/Tx.h"

#include <array>
#include <string.h>

namespace arcade
{

static bool varint(const Bytes &b, size_t &at, uint64_t &v)
{
    if (at >= b.size())
        return false;
    uint8_t n = b[at++];
    size_t size = n < 0xFD ? 0 : n == 0xFD ? 2 : n == 0xFE ? 4 : 8;
    if (!size) {
        v = n;
        return true;
    }
    if (at + size > b.size())
        return false;
    v = 0;
    for (size_t i = 0; i < size; i++)
        v |= (uint64_t)b[at + i] << (8 * i);
    at += size;
    return true;
}

static void putVarint(Bytes &out, uint64_t v)
{
    if (v < 0xFD) {
        out.push_back((uint8_t)v);
    } else if (v <= 0xFFFF) {
        out.push_back(0xFD);
        for (int i = 0; i < 2; i++)
            out.push_back((uint8_t)(v >> (8 * i)));
    } else {
        out.push_back(0xFE);
        for (int i = 0; i < 4; i++)
            out.push_back((uint8_t)(v >> (8 * i)));
    }
}

static bool le(const Bytes &b, size_t &at, size_t n, uint64_t &v)
{
    if (at + n > b.size())
        return false;
    v = 0;
    for (size_t i = 0; i < n; i++)
        v |= (uint64_t)b[at + i] << (8 * i);
    at += n;
    return true;
}

static void putLe(Bytes &out, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out.push_back((uint8_t)(v >> (8 * i)));
}

bool Tx::parse(const Bytes &raw)
{
    size_t at = 0;
    uint64_t v, n;
    ins.clear();
    outs.clear();
    if (!le(raw, at, 4, v))
        return false;
    version = (int32_t)v;
    if (at + 2 <= raw.size() && raw[at] == 0 && raw[at + 1] == 1)
        return false; // segwit: not on the T-Deck yet
    if (!varint(raw, at, n) || n > 64)
        return false;
    for (uint64_t i = 0; i < n; i++) {
        TxIn in;
        if (at + 36 > raw.size())
            return false;
        for (int k = 0; k < 32; k++)
            in.txid[k] = raw[at + 31 - k];
        at += 32;
        if (!le(raw, at, 4, v))
            return false;
        in.vout = (uint32_t)v;
        uint64_t slen;
        if (!varint(raw, at, slen) || at + slen > raw.size())
            return false;
        in.script.assign(raw.begin() + at, raw.begin() + at + slen);
        at += slen;
        if (!le(raw, at, 4, v))
            return false;
        in.sequence = (uint32_t)v;
        ins.push_back(std::move(in));
    }
    if (!varint(raw, at, n) || n > 64)
        return false;
    for (uint64_t i = 0; i < n; i++) {
        TxOut o;
        uint64_t slen;
        if (!le(raw, at, 8, o.value) || !varint(raw, at, slen) || at + slen > raw.size())
            return false;
        o.script.assign(raw.begin() + at, raw.begin() + at + slen);
        at += slen;
        outs.push_back(std::move(o));
    }
    if (!le(raw, at, 4, v))
        return false;
    locktime = (uint32_t)v;
    return at == raw.size();
}

Bytes Tx::serialize(const std::vector<Bytes> *scripts) const
{
    Bytes out;
    putLe(out, (uint32_t)version, 4);
    putVarint(out, ins.size());
    for (size_t i = 0; i < ins.size(); i++) {
        for (int k = 31; k >= 0; k--)
            out.push_back(ins[i].txid[k]);
        putLe(out, ins[i].vout, 4);
        const Bytes &s = scripts ? (*scripts)[i] : ins[i].script;
        putVarint(out, s.size());
        out.insert(out.end(), s.begin(), s.end());
        putLe(out, ins[i].sequence, 4);
    }
    putVarint(out, outs.size());
    for (auto &o : outs) {
        putLe(out, o.value, 8);
        putVarint(out, o.script.size());
        out.insert(out.end(), o.script.begin(), o.script.end());
    }
    putLe(out, locktime, 4);
    return out;
}

void Tx::sighashAll(size_t index, const Bytes &scriptCode, uint8_t out[32]) const
{
    std::vector<Bytes> scripts(ins.size());
    scripts[index] = scriptCode;
    Bytes pre = serialize(&scripts);
    putLe(pre, 1, 4); // SIGHASH_ALL
    dsha256(pre.data(), pre.size(), out);
}

void Tx::txid(uint8_t out[32]) const
{
    Bytes raw = serialize();
    uint8_t h[32];
    dsha256(raw.data(), raw.size(), h);
    for (int k = 0; k < 32; k++)
        out[k] = h[31 - k];
}

Bytes p2pkhScript(const uint8_t h160[20])
{
    Bytes s = {0x76, 0xa9, 0x14};
    s.insert(s.end(), h160, h160 + 20);
    s.push_back(0x88);
    s.push_back(0xac);
    return s;
}

Bytes p2shScript(const uint8_t h160[20])
{
    Bytes s = {0xa9, 0x14};
    s.insert(s.end(), h160, h160 + 20);
    s.push_back(0x87);
    return s;
}

Bytes scriptFor(const std::string &address, uint8_t p2pkhVersion, uint8_t p2shVersion)
{
    uint8_t version, h[20];
    if (!base58CheckDecode(address, version, h))
        return Bytes();
    if (version == p2pkhVersion)
        return p2pkhScript(h);
    if (version == p2shVersion)
        return p2shScript(h);
    return Bytes();
}

Checked checkOffer(const Bytes &raw, const std::vector<uint64_t> &inputValues, const uint8_t ownPub33[33],
                   const Bytes &payScript, uint64_t amount, uint64_t feeCap, uint64_t othersCap)
{
    Checked c;
    Tx tx;
    if (!tx.parse(raw)) {
        c.why = "the offer is not a transaction this T-Deck can read";
        return c;
    }
    if (inputValues.size() != tx.ins.size()) {
        c.why = "input values do not match the inputs";
        return c;
    }
    for (auto &in : tx.ins)
        if (!in.script.empty()) {
            c.why = "an input already carries a script";
            return c;
        }
    uint8_t h[20];
    hash160(ownPub33, 33, h);
    Bytes own = p2pkhScript(h);
    for (auto &o : tx.outs) {
        if (o.script == own)
            c.change += o.value;
        else if (!payScript.empty() && o.script == payScript)
            c.paid += o.value;
        else if (payScript.empty())
            c.paid += o.value;
        else {
            c.why = "an output pays somebody this T-Deck did not name";
            return c;
        }
    }
    uint64_t in = 0;
    for (auto v : inputValues)
        in += v;
    if (in < c.paid + c.change) {
        c.why = "the outputs spend more than the inputs hold";
        return c;
    }
    c.fee = in - c.paid - c.change;
    if (c.fee > feeCap) {
        c.why = "the fee is over this T-Deck's cap";
        return c;
    }
    if (!payScript.empty() && c.paid != amount) {
        c.why = "the offer pays a different amount";
        return c;
    }
    if (payScript.empty() && c.paid > othersCap) {
        c.why = "the offer pays others more than a message needs";
        return c;
    }
    for (size_t i = 0; i < tx.ins.size(); i++) {
        std::array<uint8_t, 32> d;
        tx.sighashAll(i, own, d.data());
        c.digests.push_back(d);
    }
    c.ok = true;
    return c;
}

Bytes assemble(const Bytes &raw, const std::vector<Bytes> &sigs, const uint8_t pub33[33])
{
    Tx tx;
    if (!tx.parse(raw) || sigs.size() != tx.ins.size())
        return Bytes();
    std::vector<Bytes> scripts;
    for (auto &s : sigs) {
        Bytes sc;
        sc.push_back((uint8_t)s.size());
        sc.insert(sc.end(), s.begin(), s.end());
        sc.push_back(33);
        sc.insert(sc.end(), pub33, pub33 + 33);
        scripts.push_back(std::move(sc));
    }
    return tx.serialize(&scripts);
}

} // namespace arcade
