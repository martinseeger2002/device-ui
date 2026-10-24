#pragma once

// The arcade's encrypted mailbox (GET /account/mailbox): the browser's sealed
// copy of letters, groups and the address book, shared across devices.
//   blob = base64(nonce24 || secretbox(JSON))
//   key  = blake2b-256(mail secret || "arcade-mailbox/1")   (messaging_js.js)
// The T-Deck opens it and keeps only the address book.

#include "arcade/ArcadeClient.h"
#include <string>

namespace arcade
{

void mailboxKey(const uint8_t mailSecret[32], uint8_t key[32]);
bool base64Decode(const std::string &text, Bytes &out);

struct Book {
    bool ok = false;
    std::string why;
    std::vector<Contact> contacts;   // the book less what bookGone took away
    size_t removed = 0;              // entries bookGone hid
};

Book openMailboxBook(const uint8_t mailSecret[32], const std::string &blob);
// the JSON half, for tests and for a blob already opened
Book readBook(const char *json, size_t len);

} // namespace arcade
