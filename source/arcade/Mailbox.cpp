#include "arcade/Mailbox.h"
#include "arcade/JsonLite.h"

#include <map>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "vendor/trezor/blake2b.h"
}

namespace arcade
{

void mailboxKey(const uint8_t mailSecret[32], uint8_t key[32])
{
    static const char CONTEXT[] = "arcade-mailbox/1";
    uint8_t both[32 + sizeof(CONTEXT) - 1];
    memcpy(both, mailSecret, 32);
    memcpy(both + 32, CONTEXT, sizeof(CONTEXT) - 1);
    blake2b(both, sizeof(both), key, 32);
    wipe(both, sizeof(both));
}

bool base64Decode(const std::string &text, Bytes &out)
{
    static int8_t map[256];
    static bool ready = false;
    if (!ready) {
        memset(map, -1, sizeof(map));
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++)
            map[(uint8_t)a[i]] = (int8_t)i;
        ready = true;
    }
    out.clear();
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ')
            continue;
        int8_t v = map[(uint8_t)c];
        if (v < 0)
            return false;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

// ---- a small JSON reader: just enough to walk the mailbox and keep the book ----

namespace
{
bool hexKey(const std::string &h, uint8_t out[32])
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
} // namespace

Book readBook(const char *text, size_t len)
{
    Book book;
    Json j{text, text + len};
    struct Entry {
        Contact c;
        double added = 0;
    };
    std::vector<Entry> entries;
    std::map<std::string, double> gone;
    j.object([&](const std::string &key) {
        if (key == "book") {
            j.array([&]() {
                if (j.p < j.end && (j.ws(), *j.p != '{'))
                    return j.skip();
                Entry e;
                j.object([&](const std::string &f) {
                    std::string v;
                    if (f == "tag" || f == "address" || f == "name" || f == "key") {
                        j.ws();
                        if (j.p < j.end && *j.p == '"') {
                            j.string(&v);
                            if (f == "tag")
                                e.c.tag = v;
                            else if (f == "address")
                                e.c.address = v;
                            else if (f == "name")
                                e.c.name = v;
                            else
                                hexKey(v, e.c.key);
                        } else {
                            j.skip();
                        }
                    } else if (f == "added") {
                        j.ws();
                        if (j.p < j.end && (*j.p == '-' || (*j.p >= '0' && *j.p <= '9')))
                            e.added = j.number();
                        else
                            j.skip();
                    } else {
                        j.skip();
                    }
                });
                if (!e.c.tag.empty() && !e.c.address.empty())
                    entries.push_back(e);
            });
        } else if (key == "bookGone") {
            j.ws();
            if (j.p < j.end && *j.p == '{')
                j.object([&](const std::string &tag) {
                    j.ws();
                    gone[tag] = (j.p < j.end && *j.p != '"' && *j.p != '{' && *j.p != '[') ? j.number() : (j.skip(), 0);
                });
            else
                j.skip();
        } else {
            j.skip();
        }
    });
    if (!j.ok) {
        book.why = "The address book could not be read";
        return book;
    }
    for (auto &e : entries) {
        auto g = gone.find(e.c.tag);
        // the browser's rule (messaging_js syncMailbox): deleted at or after it was added
        if (g != gone.end() && g->second >= e.added) {
            book.removed++;
            continue;
        }
        book.contacts.push_back(e.c);
    }
    book.ok = true;
    return book;
}

Book openMailboxBook(const uint8_t mailSecret[32], const std::string &blob)
{
    Book book;
    Bytes both, plain;
    if (blob.empty()) {
        book.ok = true; // nothing kept yet: an empty book
        return book;
    }
    if (!base64Decode(blob, both) || both.size() < 24 + 16) {
        book.why = "The mailbox from the arcade was garbled";
        return book;
    }
    uint8_t key[32];
    mailboxKey(mailSecret, key);
    bool opened = secretboxOpen(key, both.data(), both.data() + 24, both.size() - 24, plain);
    wipe(key, 32);
    if (!opened) {
        book.why = "This account's key does not open that mailbox";
        return book;
    }
    book = readBook((const char *)plain.data(), plain.size());
    wipe(plain.data(), plain.size());
    return book;
}

} // namespace arcade
