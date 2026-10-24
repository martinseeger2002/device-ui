#include "arcade/Frames.h"

#include <string.h>

namespace arcade
{

std::vector<Bytes> split(uint8_t type, uint16_t id, const Bytes &body)
{
    std::vector<Bytes> frames;
    size_t count = body.empty() ? 1 : (body.size() + MAX_BODY - 1) / MAX_BODY;
    if (count > MAX_PARTS)
        return frames;
    for (size_t i = 0; i < count; i++) {
        size_t from = i * MAX_BODY, n = body.size() - from < MAX_BODY ? body.size() - from : MAX_BODY;
        if (body.empty())
            n = 0;
        Bytes f = {MAGIC, type, (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)((i << 4) | (count - 1))};
        f.insert(f.end(), body.begin() + from, body.begin() + from + n);
        frames.push_back(std::move(f));
    }
    return frames;
}

bool Joiner::add(uint32_t sender, const uint8_t *frame, size_t len, uint32_t nowMs, Message &out)
{
    if (len < HEADER || len > MAX_FRAME || frame[0] != MAGIC)
        return false;
    uint8_t type = frame[1], index = frame[4] >> 4, count = (frame[4] & 0x0F) + 1;
    uint16_t id = (uint16_t)(frame[2] << 8 | frame[3]);
    if (index >= count)
        return false;
    if (count == 1) {
        out.type = type;
        out.id = id;
        out.body.assign(frame + HEADER, frame + len);
        return true;
    }
    auto key = std::make_tuple(sender, type, id);
    auto it = pending.find(key);
    if (it == pending.end())
        it = pending.emplace(key, Slot{count, nowMs, {}}).first;
    if (it->second.count != count) {
        pending.erase(it);
        return false;
    }
    it->second.parts[index] = Bytes(frame + HEADER, frame + len);
    if (it->second.parts.size() < count)
        return false;
    out.type = type;
    out.id = id;
    out.body.clear();
    for (auto &p : it->second.parts)
        out.body.insert(out.body.end(), p.second.begin(), p.second.end());
    pending.erase(it);
    return true;
}

void Joiner::expire(uint32_t nowMs)
{
    for (auto it = pending.begin(); it != pending.end();)
        it = nowMs - it->second.at > JOIN_TIMEOUT_MS ? pending.erase(it) : std::next(it);
}

Writer &Writer::u8(uint8_t v)
{
    out.push_back(v);
    return *this;
}
Writer &Writer::u16(uint16_t v)
{
    return u8(v >> 8).u8((uint8_t)v);
}
Writer &Writer::u32(uint32_t v)
{
    return u16(v >> 16).u16((uint16_t)v);
}
Writer &Writer::amount(uint64_t v)
{
    return u32((uint32_t)(v >> 32)).u32((uint32_t)v);
}
Writer &Writer::raw(const uint8_t *p, size_t n)
{
    out.insert(out.end(), p, p + n);
    return *this;
}
Writer &Writer::str(const std::string &s)
{
    size_t n = s.size() > 255 ? 255 : s.size();
    return u8((uint8_t)n).raw((const uint8_t *)s.data(), n);
}
Writer &Writer::bytes(const Bytes &b)
{
    return u16((uint16_t)b.size()).raw(b);
}

bool Reader::take(uint8_t *dst, size_t n)
{
    if (!ok || at + n > body.size()) {
        ok = false;
        memset(dst, 0, n);
        return false;
    }
    memcpy(dst, body.data() + at, n);
    at += n;
    return true;
}
Bytes Reader::take(size_t n)
{
    Bytes b(n);
    take(b.data(), n);
    return ok ? b : Bytes();
}
uint8_t Reader::u8(void)
{
    uint8_t v = 0;
    take(&v, 1);
    return v;
}
uint16_t Reader::u16(void)
{
    uint8_t b[2];
    take(b, 2);
    return (uint16_t)(b[0] << 8 | b[1]);
}
uint32_t Reader::u32(void)
{
    uint8_t b[4];
    take(b, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
uint64_t Reader::amount(void)
{
    uint64_t hi = u32();
    return hi << 32 | u32();
}
std::string Reader::str(void)
{
    Bytes b = take(u8());
    return std::string(b.begin(), b.end());
}
Bytes Reader::bytes(void)
{
    return take(u16());
}

} // namespace arcade
