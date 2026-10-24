#pragma once

// Arcade LoRa frames (PROTOCOL.md). Must match gateway/arcade_lora/frames.py.

#include "arcade/Crypto.h"
#include <functional>
#include <map>
#include <string>
#include <tuple>

namespace arcade
{

constexpr uint8_t MAGIC = 0xA7;
constexpr size_t HEADER = 5;
constexpr size_t MAX_FRAME = 200; // Meshtastic: 255 - 16 header - 12 PKC = 227 B of encoded Data, less the protobuf wrapper
constexpr size_t MAX_BODY = MAX_FRAME - HEADER;
constexpr size_t MAX_PARTS = 16;
constexpr uint32_t JOIN_TIMEOUT_MS = 120000;
constexpr uint16_t PORTNUM = 256; // Meshtastic PRIVATE_APP

enum Type : uint8_t {
    GW_BEACON = 0x01, HELLO = 0x02, CHALLENGE = 0x03, LOGIN = 0x04, WELCOME = 0x05, ERR = 0x06, RESEND = 0x07,
    PING = 0x08, PONG = 0x09,
    BAL_REQ = 0x10, BAL = 0x11, COINS_REQ = 0x12, COINS = 0x13, SEND_REQ = 0x14, OFFER = 0x15, SIGN = 0x16,
    SENT = 0x17, BROADCAST = 0x18, ADDR = 0x19, ADDR_OK = 0x1A,
    WHO_REQ = 0x20, WHO = 0x21, WATCH = 0x22, WATCH_OK = 0x23,
    MAIL_POST = 0x30, MAIL_IN = 0x31, MAIL_TOO_LONG = 0x32, MAIL_ACK = 0x33, MAIL_POLL = 0x34,
    PRESENCE = 0x40, P2P_MSG = 0x41, P2P_PAY = 0x42, P2P_ACK = 0x43,
};

enum ErrCode : uint8_t { E_NOT_LOGGED_IN = 1, E_BAD_REQUEST, E_ARCADE, E_BUSY, E_TOO_LONG, E_UNKNOWN_TAG, E_EXPIRED };

struct Message {
    uint8_t type = 0;
    uint16_t id = 0;
    Bytes body;
};

// the frames that carry one message, in order (empty if it is too big)
std::vector<Bytes> split(uint8_t type, uint16_t id, const Bytes &body);

class Joiner
{
  public:
    // a whole message once its last part arrives
    bool add(uint32_t sender, const uint8_t *frame, size_t len, uint32_t nowMs, Message &out);
    void expire(uint32_t nowMs);

  private:
    struct Slot {
        uint8_t count;
        uint32_t at;
        std::map<uint8_t, Bytes> parts;
    };
    std::map<std::tuple<uint32_t, uint8_t, uint16_t>, Slot> pending;
};

class Writer
{
  public:
    Writer &u8(uint8_t v);
    Writer &u16(uint16_t v);
    Writer &u32(uint32_t v);
    Writer &amount(uint64_t v);
    Writer &raw(const uint8_t *p, size_t n);
    Writer &raw(const Bytes &b) { return raw(b.data(), b.size()); }
    Writer &str(const std::string &s);   // u8 length; cut at 255 bytes
    Writer &bytes(const Bytes &b);       // u16 length
    const Bytes &done(void) const { return out; }

  private:
    Bytes out;
};

// Reads stop at the end of the body and set `ok` false instead of throwing.
class Reader
{
  public:
    explicit Reader(const Bytes &b) : body(b) {}
    uint8_t u8(void);
    uint16_t u16(void);
    uint32_t u32(void);
    uint64_t amount(void);
    bool take(uint8_t *out, size_t n);
    Bytes take(size_t n);
    std::string str(void);
    Bytes bytes(void);
    bool end(void) const { return at == body.size(); }
    bool ok = true;

  private:
    const Bytes &body;
    size_t at = 0;
};

} // namespace arcade
