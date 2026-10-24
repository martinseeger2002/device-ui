#include "arcade/ArcadeClient.h"
#include "arcade/Mailbox.h"
#include "arcade/Snapshot.h"
#include "arcade/Envelope.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace arcade
{

uint64_t ArcadeClient::FEE_CAP = 5000000; // 0.05 coin

static const uint32_t BROADCAST_NODE = 0xFFFFFFFF;
// The gateway waits up to 25 s for each frame's radio ack and Meshtastic
// retries three times underneath, so an answer can take a while; one that
// does not come is asked for again (same id: the gateway replays its answer,
// it never does the work twice) before it counts as lost.
static const uint32_t REQUEST_TIMEOUT_MS = 45000;
static const uint8_t REQUEST_TRIES = 5;
static const uint32_t PING_EVERY_MS = 20000; // looking for a gateway: often, a lost PONG is common
static const uint32_t BALANCES_EVERY_MS = 10 * 60000;
static const char KEY_FILE[] = "key";
static const char CONTACTS_FILE[] = "contacts";
static const char ACTIVITY_FILE[] = "activity";
static const char GATEWAY_FILE[] = "gateway";
static const char WIFI_FILE[] = "wifi";
static const char ACCOUNT_FILE[] = "account";
static const char PAYQUEUE_FILE[] = "payqueue";
static const char DEFAULT_ARCADE[] = "app.dogecoinarcade.com";

std::string Chain::coinName(void) const
{
    // The arcade's labels name the network ("Testnet", "Mainnet"); the
    // T-Deck names the coin, and says testnet separately.
    if (network == "main" || network == "test" || network == "regtest")
        return "Pepecoin";
    if (network == "doge")
        return "Dogecoin";
    if (network == "ltc" || network == "ltctest")
        return "Litecoin";
    if (network == "lky")
        return "Luckycoin";
    return label;
}

std::string Chain::symbol(void) const
{
    if (network == "doge")
        return "DOGE";
    if (network == "ltc" || network == "ltctest")
        return "LTC";
    if (network == "lky")
        return "LKY";
    return "PEP";
}

// ---- key store --------------------------------------------------------------

bool KeyStore::exists(void)
{
    Bytes b;
    return storage.load(KEY_FILE, b) && b.size() > 43;
}

static void pinKey(const std::string &pin, const uint8_t salt[16], uint8_t key[32])
{
    pbkdf2Sha512((const uint8_t *)pin.data(), pin.size(), salt, 16, KeyStore::ROUNDS, key, 32);
}

bool KeyStore::create(const std::string &phrase, const std::string &pin)
{
    uint8_t salt[16], nonce[24], key[32];
    randomBytes(salt, 16);
    randomBytes(nonce, 24);
    pinKey(pin, salt, key);
    std::string p = normalisePhrase(phrase);
    Bytes box;
    bool ok = secretboxSeal(key, nonce, (const uint8_t *)p.data(), p.size(), box);
    wipe(key, 32);
    wipe(&p[0], p.size());
    if (!ok)
        return false;
    Bytes file = {'a', 'k', '1'};
    file.insert(file.end(), salt, salt + 16);
    file.insert(file.end(), nonce, nonce + 24);
    file.insert(file.end(), box.begin(), box.end());
    return storage.save(KEY_FILE, file);
}

bool KeyStore::unlock(const std::string &pin, std::string &phrase)
{
    Bytes file, out;
    if (!storage.load(KEY_FILE, file) || file.size() < 43 + 16 || memcmp(file.data(), "ak1", 3) != 0)
        return false;
    uint8_t key[32];
    pinKey(pin, file.data() + 3, key);
    bool ok = secretboxOpen(key, file.data() + 19, file.data() + 43, file.size() - 43, out);
    wipe(key, 32);
    if (ok)
        phrase.assign(out.begin(), out.end());
    wipe(out.data(), out.size());
    return ok;
}

void KeyStore::erase(void)
{
    storage.remove(KEY_FILE);
}

// ---- client -----------------------------------------------------------------

ArcadeClient::ArcadeClient(Storage &s, uint32_t node) : storage(s), keys(s), myNode(node)
{
    Bytes w;
    if (storage.load("wifi-on", w) && !w.empty())
        wifiOn = w[0] != 0;
    link = keys.exists() ? Link::Locked : Link::NoAccount;
    status = keys.exists() ? "Locked" : "No account on this T-Deck yet";
}

bool ArcadeClient::createAccount(const std::string &phrase, const std::string &pin)
{
    if (!phraseValid(phrase) || pin.size() < 4)
        return false;
    if (!keys.create(phrase, pin))
        return false;
    return unlock(pin);
}

bool ArcadeClient::unlock(const std::string &pin)
{
    std::string phrase;
    if (!keys.unlock(pin, phrase))
        return false;
    account = std::make_shared<Account>(phrase);
    wipe(&phrase[0], phrase.size());
    loadState();
    link = Link::Searching;
    status = "Looking for a gateway";
    if (gateway) {
        // the last gateway first: one HELLO instead of a broadcast
        link = Link::SigningIn;
        status = "Signing in through " + (gatewayName.empty() ? std::string("the gateway") : gatewayName);
        request(HELLO, Bytes(account->loginPublic(), account->loginPublic() + 32));
    } else {
        findGateway();
    }
    note();
    return true;
}

void ArcadeClient::lock(void)
{
    account.reset();
    link = Link::Locked;
    status = "Locked";
    tag.clear();
    chains.clear();
    pending.clear();
    send.state = SendState::Idle;
    note();
}

void ArcadeClient::loadState(void)
{
    Bytes b;
    contacts.clear();
    if (storage.load(CONTACTS_FILE, b)) {
        Reader r(b);
        for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++) {
            Contact c;
            c.tag = r.str();
            c.address = r.str();
            c.name = r.str();
            r.take(c.key, 32);
            if (r.ok)
                contacts.push_back(c);
        }
    }
    activity.clear();
    if (storage.load(ACTIVITY_FILE, b)) {
        Reader r(b);
        for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++) {
            Activity a;
            a.who = r.str();
            a.symbol = r.str();
            a.amount = (int64_t)r.amount();
            a.when = r.u32();
            a.txid = r.str();
            a.offline = r.u8();
            if (r.ok)
                activity.push_back(a);
        }
    }
    if (storage.load(ACCOUNT_FILE, b)) {
        // the last balances this T-Deck saw, for when no gateway answers
        Reader r(b);
        uint32_t when = r.u32();
        std::string t = r.str();
        std::vector<Chain> got;
        for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++) {
            Chain c;
            c.network = r.str();
            c.label = r.str();
            uint8_t flags = r.u8();
            c.tags = flags & 1;
            c.mainnet = flags & 2;
            c.registered = flags & 4; // registered once is registered: sign-in stays one exchange
            c.p2pkh = r.u8();
            c.p2sh = r.u8();
            c.confirmed = r.amount();
            c.pending = r.amount();
            got.push_back(c);
        }
        if (r.ok && chains.empty()) {
            tag = t;
            chains = got;
            balancesWhen = when;
            balancesFrom.clear();
        }
    }
    payQueue.clear();
    if (storage.load(PAYQUEUE_FILE, b)) {
        Reader r(b);
        for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++) {
            QueuedPay q;
            q.network = r.str();
            q.to = r.str();
            q.amount = r.amount();
            q.when = r.u32();
            if (r.ok)
                payQueue.push_back(q);
        }
    }
    if (storage.load(GATEWAY_FILE, b)) {
        Reader r(b);
        uint32_t node = r.u32();
        std::string name = r.str();
        if (r.ok && node) {
            gateway = node;
            gatewayName = name;
        }
    }
}

void ArcadeClient::saveContacts(void)
{
    Writer w;
    w.u8((uint8_t)(contacts.size() > 255 ? 255 : contacts.size()));
    for (size_t i = 0; i < contacts.size() && i < 255; i++)
        w.str(contacts[i].tag).str(contacts[i].address).str(contacts[i].name).raw(contacts[i].key, 32);
    storage.save(CONTACTS_FILE, w.done());
}

void ArcadeClient::saveActivity(void)
{
    if (activity.size() > 40)
        activity.resize(40);
    Writer w;
    w.u8((uint8_t)activity.size());
    for (auto &a : activity)
        w.str(a.who).str(a.symbol).amount((uint64_t)a.amount).u32(a.when).str(a.txid).u8(a.offline);
    storage.save(ACTIVITY_FILE, w.done());
}

void ArcadeClient::request(uint8_t type, const Bytes &body, uint32_t to, bool expectAnswer)
{
    if (!sendFrame)
        return;
    uint16_t id = nextId++;
    if (!nextId)
        nextId = 1;
    std::vector<Bytes> frames = split(type, id, body);
    uint32_t dest = to ? to : gateway;
    if (expectAnswer) {
        Pending p{type, now, dest, 1, {}};
        if (type != PING)
            p.frames = frames;
        pending[id] = p;
    }
    for (auto &f : frames)
        sendFrame(dest, f);
}

void ArcadeClient::findGateway(void)
{
    lastPing = now;
    request(PING, Bytes(), BROADCAST_NODE);
}

void ArcadeClient::receive(uint32_t from, const uint8_t *data, size_t len, uint32_t nowMs)
{
    now = nowMs;
    Message m;
    if (joiner.add(from, data, len, nowMs, m))
        handle(from, m, nowMs);
}

void ArcadeClient::fail(uint8_t type, const std::string &why)
{
    if (mailInFlight && (type == MAIL_POST || (type == SIGN && signingMail))) {
        mailInFlight = signingMail = false;
        if (why == "No answer from the gateway") {
            // lost on the air, not refused: it stays queued and goes again
            mailRetryAt = now + 60000;
            return;
        }
        // the arcade said no: the letter at the head of the queue did not go; say why on it
        if (!mailQueue.empty()) {
            Outgoing o = mailQueue.front();
            mailQueue.erase(mailQueue.begin());
            auto &t = thread(o.tag);
            if (o.index < t.size()) {
                t[o.index].state = Letter::Failed;
                t[o.index].why = why;
            }
            saveThread(o.tag);
        }
        nextMail();
        return;
    }
    bool lost = why == "No answer from the gateway";
    if (lost && type == SEND_REQ && !fromQueue && send.state == SendState::Asking) {
        // nothing was signed, so it is safe to wait and ask again by itself
        payQueue.insert(payQueue.begin(), {chains.size() > send.chain ? chains[send.chain].network : "", send.to,
                                           send.amount, unixNow});
        savePayQueue();
        send.state = SendState::Queued;
        payRetryAt = now + 60000;
        return;
    }
    if (lost && type == SIGN) {
        // signed and sent off, but no word back: it may well have gone, so
        // nothing is asked for again on its own (that could pay twice)
        send.state = SendState::Failed;
        send.why = "Signed, but the gateway did not answer. It may have gone: check the wallet before you send again.";
        if (fromQueue && !payQueue.empty()) {
            payQueue.erase(payQueue.begin());
            savePayQueue();
            fromQueue = false;
        }
        return;
    }
    switch (type) {
    case SEND_REQ:
    case SIGN:
        send.state = SendState::Failed;
        send.why = why;
        if (fromQueue && !payQueue.empty()) {
            // the arcade said no (funds, a bad @tag): it would say no again
            payQueueError = payQueue.front().to + ": " + why;
            if (why != "No answer from the gateway") {
                payQueue.erase(payQueue.begin());
                savePayQueue();
            } else {
                // lost on the air: still waiting, and it goes again by itself
                payQueueError.clear();
                send.state = SendState::Queued;
                payRetryAt = now + 60000;
            }
            fromQueue = false;
        }
        break;
    case WHO_REQ:
        who.state = WhoState::Failed;
        who.why = why;
        break;
    case HELLO:
    case LOGIN:
        link = Link::Searching;
        status = why;
        break;
    default:
        status = why;
    }
}

void ArcadeClient::handle(uint32_t from, const Message &m, uint32_t nowMs)
{
    auto p = pending.find(m.id);
    uint8_t asked = p != pending.end() ? p->second.type : 0;
    if (p != pending.end())
        pending.erase(p);

    if (m.type == GW_BEACON || m.type == PONG) {
        Reader r(m.body);
        std::string name = r.str(), host = r.str();
        if (!r.ok || !account)
            return;
        if (link == Link::Searching || (gateway == from && link != Link::Online)) {
            gateway = from;
            gatewayName = name;
            gatewayHost = host;
            storage.save(GATEWAY_FILE, Writer().u32(from).str(name).done());
            link = Link::SigningIn;
            status = "Signing in through " + name;
            request(HELLO, Bytes(account->loginPublic(), account->loginPublic() + 32));
            note();
        }
        return;
    }
    if (from != gateway || !account)
        return; // peer-to-peer frames come in a later build

    switch (m.type) {
    case CHALLENGE: {
        Reader r(m.body);
        std::string origin = r.str(), nonce = r.str();
        if (!r.ok)
            return fail(HELLO, "The gateway's challenge was garbled");
        uint8_t sig[64];
        account->signLogin(origin, nonce, sig);
        Bytes body(sig, sig + 64);
        body.push_back(0); // join: no, the account comes from the arcade
        request(LOGIN, body);
        break;
    }
    case WELCOME:
        onWelcome(m);
        break;
    case BAL: {
        Reader r(m.body);
        for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++) {
            uint8_t c = r.u8();
            uint64_t conf = r.amount(), pend = r.amount();
            if (r.ok && c < chains.size()) {
                chains[c].confirmed = conf;
                chains[c].pending = pend;
            }
        }
        balancesAt = nowMs;
        balancesFrom = "gateway";
        balancesWhen = unixNow;
        saveAccountCache();
        break;
    }
    case WATCH_OK:
        break;
    case ADDR_OK: {
        auto a = addrAsked.find(m.id);
        if (a != addrAsked.end() && a->second < chains.size())
            chains[a->second].registered = true;
        if (a != addrAsked.end())
            addrAsked.erase(a);
        saveAccountCache();
        break;
    }
    case OFFER:
        if (mailInFlight && !signingMail)
            onMailOffer(m);
        else
            onOffer(m);
        break;
    case MAIL_IN:
        onMail(m, false);
        break;
    case MAIL_TOO_LONG:
        onMail(m, true);
        break;
    case SENT: {
        if (signingMail) {
            signingMail = false;
            mailInFlight = false;
            if (!mailQueue.empty()) {
                Outgoing o = mailQueue.front();
                mailQueue.erase(mailQueue.begin());
                auto &t = thread(o.tag);
                if (o.index < t.size()) {
                    t[o.index].state = Letter::Sent;
                    t[o.index].txid = m.body.size() == 32 ? toHex(m.body.data(), 32) : "";
                }
                saveThread(o.tag);
            }
            nextMail();
            break;
        }
        if (m.body.size() != 32)
            return fail(SIGN, "The gateway's answer was garbled");
        send.txid = toHex(m.body.data(), 32);
        send.state = SendState::Sent;
        Activity a;
        a.who = send.toTag.empty() ? send.toAddress : "@" + send.toTag;
        a.symbol = chains.size() > send.chain ? chains[send.chain].symbol() : "";
        a.amount = -(int64_t)send.amount;
        a.when = nowMs / 1000;
        a.txid = send.txid;
        activity.insert(activity.begin(), a);
        saveActivity();
        if (fromQueue && !payQueue.empty()) {
            payQueue.erase(payQueue.begin());
            savePayQueue();
            fromQueue = false;
        }
        refreshBalances();
        runPayQueue();
        break;
    }
    case WHO: {
        Reader r(m.body);
        Contact c;
        c.tag = r.str();
        c.address = r.str();
        r.take(c.key, 32);
        c.name = r.str();
        if (!r.ok)
            return fail(WHO_REQ, "The gateway's answer was garbled");
        bool known = false;
        for (auto &k : contacts)
            if (k.tag == c.tag) {
                k = c;
                known = true;
            }
        if (!known)
            contacts.push_back(c);
        saveContacts();
        sendWatch();
        who.state = WhoState::Found;
        who.tag = c.tag;
        break;
    }
    case ERR: {
        Reader r(m.body);
        uint8_t code = r.u8();
        std::string text = r.str();
        if (code == E_NOT_LOGGED_IN && account) {
            link = Link::SigningIn;
            status = "Signing in again";
            request(HELLO, Bytes(account->loginPublic(), account->loginPublic() + 32));
        } else {
            fail(asked, text.empty() ? "The gateway refused" : text);
        }
        break;
    }
    default:
        break;
    }
    note();
}

void ArcadeClient::onWelcome(const Message &m)
{
    Reader r(m.body);
    std::string t = r.str();
    std::vector<Chain> got;
    for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++) {
        Chain c;
        c.network = r.str();
        c.label = r.str();
        uint8_t flags = r.u8();
        c.p2pkh = r.u8();
        c.p2sh = r.u8();
        c.tags = flags & 1;
        c.mainnet = flags & 2;
        for (auto &old : chains)
            if (old.network == c.network) {
                c.confirmed = old.confirmed;
                c.pending = old.pending;
                c.registered = old.registered;
            }
        got.push_back(c);
    }
    if (!r.ok)
        return fail(LOGIN, "The gateway's welcome was garbled");
    tag = t;
    chains = got;
    link = Link::Online;
    status = tag.empty() ? "Signed in. Claim an @tag on the arcade first" : "Online through " + gatewayName;
    registerAddresses();
    sendWatch();
    refreshBalances();
    nextMail();    // letters written while no gateway answered
    runPayQueue(); // and payments
}

void ArcadeClient::saveAccountCache(void)
{
    Writer w;
    w.u32(balancesWhen).str(tag).u8((uint8_t)chains.size());
    for (auto &c : chains)
        w.str(c.network).str(c.label).u8((c.tags ? 1 : 0) | (c.mainnet ? 2 : 0) | (c.registered ? 4 : 0)).u8(c.p2pkh).u8(c.p2sh).amount(c.confirmed).amount(c.pending);
    storage.save(ACCOUNT_FILE, w.done());
}

void ArcadeClient::applySnapshot(const Snapshot &snap)
{
    // A live gateway's list wins; with none, the arcade's own answer from the
    // WiFi sync fills the wallet. The @tag always comes across.
    if (!snap.tag.empty())
        tag = snap.tag;
    if (link != Link::Online || chains.empty()) {
        chains = snap.chains;
    } else {
        for (auto &c : chains)
            for (auto &s : snap.chains)
                if (s.network == c.network) {
                    c.confirmed = s.confirmed;
                    c.pending = s.pending;
                }
    }
    balancesFrom = "WiFi";
    balancesWhen = unixNow;
    saveAccountCache();
    note();
}

void ArcadeClient::saveCoins(const std::string &network, const std::vector<Coin> &coins)
{
    // the biggest 150 are plenty to pay from without a gateway
    Writer w;
    size_t n = coins.size() > 150 ? 150 : coins.size();
    w.u16((uint16_t)n);
    for (size_t i = 0; i < n; i++)
        w.raw(coins[i].txid, 32).u32(coins[i].vout).amount(coins[i].value);
    storage.save("coins-" + network, w.done());
}

size_t ArcadeClient::cachedCoins(const std::string &network)
{
    Bytes b;
    if (!storage.load("coins-" + network, b) || b.size() < 2)
        return 0;
    return (size_t)(b[0] << 8 | b[1]);
}

void ArcadeClient::sendWatch(void)
{
    // whose mail the gateway may carry: the address book's tag-chain addresses
    if (link != Link::Online)
        return;
    Writer w;
    std::vector<std::array<uint8_t, 20>> keys;
    for (auto &c : contacts) {
        uint8_t v, h[20];
        if (base58CheckDecode(c.address, v, h)) {
            std::array<uint8_t, 20> k;
            memcpy(k.data(), h, 20);
            keys.push_back(k);
        }
    }
    if (keys.size() > 150)
        keys.resize(150); // one message of 16 frames
    w.u8((uint8_t)keys.size());
    for (auto &k : keys)
        w.raw(k.data(), 20);
    request(WATCH, w.done());
}

// ---- address book over WiFi ---------------------------------------------------

static void wifiKey(const uint8_t mailSecret[32], uint8_t key[32])
{
    // the WiFi password is sealed with a key only this account's words make
    Bytes both(mailSecret, mailSecret + 32);
    const char ctx[] = "arcade-wifi/1";
    both.insert(both.end(), ctx, ctx + sizeof(ctx) - 1);
    sha256(both.data(), both.size(), key);
    wipe(both.data(), both.size());
}

bool ArcadeClient::saveWifi(const std::string &ssid, const std::string &pass)
{
    if (!account)
        return false;
    uint8_t key[32], nonce[24];
    wifiKey(account->mailSecret(), key);
    randomBytes(nonce, 24);
    Bytes plain = Writer().str(ssid).str(pass).done(), box;
    bool ok = secretboxSeal(key, nonce, plain.data(), plain.size(), box);
    wipe(key, 32);
    wipe(plain.data(), plain.size());
    if (!ok)
        return false;
    Bytes file(nonce, nonce + 24);
    file.insert(file.end(), box.begin(), box.end());
    return storage.save(WIFI_FILE, file);
}

bool ArcadeClient::loadWifi(std::string &ssid, std::string &pass)
{
    Bytes file, plain;
    if (!account || !storage.load(WIFI_FILE, file) || file.size() < 24 + 16)
        return false;
    uint8_t key[32];
    wifiKey(account->mailSecret(), key);
    bool ok = secretboxOpen(key, file.data(), file.data() + 24, file.size() - 24, plain);
    wipe(key, 32);
    if (!ok)
        return false;
    Reader r(plain);
    ssid = r.str();
    pass = r.str();
    wipe(plain.data(), plain.size());
    return r.ok;
}

std::string ArcadeClient::arcadeHost(void) const
{
    // the gateway names its arcade; one that only knows itself as localhost
    // means the public one
    const std::string &h = gatewayHost;
    if (h.empty() || h.compare(0, 9, "127.0.0.1") == 0 || h.compare(0, 9, "localhost") == 0)
        return DEFAULT_ARCADE;
    return h;
}

void ArcadeClient::signLogin(const std::string &origin, const std::string &nonce, uint8_t sig[64]) const
{
    if (account)
        account->signLogin(origin, nonce, sig);
}

void ArcadeClient::syncOverWifi(void)
{
    std::string ssid, pass;
    if (!account) {
        return wifiProgress(WifiState::Failed, "Unlock first");
    }
    if (!wifiOn) {
        return wifiProgress(WifiState::Failed, "WiFi is off. Turn it on in Settings.");
    }
    if (!loadWifi(ssid, pass) || ssid.empty()) {
        return wifiProgress(WifiState::Failed, "Add your WiFi first");
    }
    if (!startWifiSync) {
        return wifiProgress(WifiState::Failed, "This T-Deck build has no WiFi");
    }
    wifi.ssid = ssid;
    wifiProgress(WifiState::Joining);
    startWifiSync(ssid, pass, arcadeHost());
    wipe(&pass[0], pass.size());
}

void ArcadeClient::setWifiOn(bool on)
{
    wifiOn = on;
    storage.save("wifi-on", Bytes{(uint8_t)(on ? 1 : 0)});
    if (!on) {
        wifiNets.clear();
        wifiScanning = false;
    }
    note();
}

void ArcadeClient::scanWifi(void)
{
    if (!wifiOn || !startWifiScan || wifiScanning || (wifi.state != WifiState::Idle && wifi.state != WifiState::Done &&
                                           wifi.state != WifiState::Failed))
        return;
    wifiScanning = true;
    startWifiScan();
    note();
}

void ArcadeClient::wifiScanned(std::vector<WifiNet> nets)
{
    wifiScanning = false;
    wifiNets = std::move(nets);
    note();
}

void ArcadeClient::wifiProgress(WifiState s, const std::string &why)
{
    wifi.state = s;
    wifi.why = why;
    note();
}

bool ArcadeClient::importMailbox(const std::string &blob)
{
    if (!account) {
        wifiProgress(WifiState::Failed, "Locked before the sync finished");
        return false;
    }
    Book book = openMailboxBook(account->mailSecret(), blob);
    if (!book.ok) {
        wifiProgress(WifiState::Failed, book.why);
        return false;
    }
    wifi.removed = book.removed;
    importBook(book.contacts);
    wifi.imported = book.contacts.size();
    wifiProgress(WifiState::Done);
    return true;
}

void ArcadeClient::importBook(const std::vector<Contact> &book)
{
    // The arcade's book is the truth for everyone in it; contacts added only
    // on this T-Deck stay too.
    for (auto &b : book) {
        bool found = false;
        for (auto &c : contacts)
            if (c.tag == b.tag) {
                c = b;
                found = true;
            }
        if (!found)
            contacts.push_back(b);
    }
    saveContacts();
    sendWatch();
    note();
}

void ArcadeClient::registerAddresses(void)
{
    // Tells the arcade which address to watch on each chain. Harmless to
    // repeat; once per sign-in keeps an account made in a browser and one
    // made here the same.
    for (size_t i = 0; i < chains.size(); i++) {
        uint8_t pub[33];
        if (chains[i].registered || !account->coinPublic(chains[i].network, pub))
            continue;
        Bytes body = {(uint8_t)i};
        body.insert(body.end(), pub, pub + 33);
        addrAsked[nextId] = i;
        request(ADDR, body);
    }
}

void ArcadeClient::refreshBalances(void)
{
    if (link != Link::Online)
        return;
    lastBalances = now;
    request(BAL_REQ, Bytes());
}

void ArcadeClient::addContact(const std::string &t)
{
    std::string name = t;
    while (!name.empty() && (name[0] == '@' || name[0] == ' '))
        name.erase(0, 1);
    for (auto &c : name)
        c = (char)tolower((unsigned char)c);
    who.state = WhoState::Asking;
    who.tag = name;
    who.why.clear();
    if (link != Link::Online) {
        who.state = WhoState::Failed;
        who.why = "No gateway in range";
        return note();
    }
    request(WHO_REQ, Writer().str(name).done());
    note();
}

void ArcadeClient::removeContact(const std::string &t)
{
    for (auto it = contacts.begin(); it != contacts.end(); ++it)
        if (it->tag == t) {
            contacts.erase(it);
            break;
        }
    saveContacts();
    note();
}

void ArcadeClient::savePayQueue(void)
{
    Writer w;
    w.u8((uint8_t)(payQueue.size() > 20 ? 20 : payQueue.size()));
    for (size_t i = 0; i < payQueue.size() && i < 20; i++)
        w.str(payQueue[i].network).str(payQueue[i].to).amount(payQueue[i].amount).u32(payQueue[i].when);
    storage.save(PAYQUEUE_FILE, w.done());
}

void ArcadeClient::runPayQueue(void)
{
    // the next waiting payment, once a gateway answers and nothing else is in flight
    if (payQueue.empty() || link != Link::Online || mailInFlight)
        return;
    if (payRetryAt && (int32_t)(now - payRetryAt) < 0)
        return;
    payRetryAt = 0;
    if (send.state == SendState::Asking || send.state == SendState::Review || send.state == SendState::Signing)
        return;
    const QueuedPay &q = payQueue.front();
    size_t chain = chains.size();
    for (size_t i = 0; i < chains.size(); i++)
        if (chains[i].network == q.network)
            chain = i;
    if (chain >= chains.size()) {
        payQueueError = "That wallet is not offered by this gateway";
        payQueue.erase(payQueue.begin());
        savePayQueue();
        return runPayQueue();
    }
    send = {};
    send.chain = chain;
    send.to = q.to;
    send.amount = q.amount;
    send.state = SendState::Asking;
    fromQueue = true;
    request(SEND_REQ, Writer().u8((uint8_t)chain).amount(q.amount).str(q.to).done());
    note();
}

void ArcadeClient::askToSend(size_t chain, const std::string &to, uint64_t amount)
{
    send = {};
    send.chain = chain;
    send.to = to;
    send.amount = amount;
    fromQueue = false;
    if (chain >= chains.size()) {
        send.state = SendState::Failed;
        send.why = "Pick a wallet to send from";
        return note();
    }
    if (link != Link::Online) {
        // no gateway: the payment waits on flash and goes when one answers
        payQueue.push_back({chains[chain].network, to, amount, unixNow});
        savePayQueue();
        send.state = SendState::Queued;
        return note();
    }
    send.state = SendState::Asking;
    request(SEND_REQ, Writer().u8((uint8_t)chain).amount(amount).str(to).done());
    note();
}

void ArcadeClient::onOffer(const Message &m)
{
    if (send.state != SendState::Asking)
        return;
    Reader r(m.body);
    uint8_t chain = r.u8();
    send.toTag = r.str();
    send.toAddress = r.str();
    uint64_t fee = r.amount(), change = r.amount();
    offerRaw = r.bytes();
    offerValues.clear();
    for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++)
        offerValues.push_back(r.amount());
    if (!r.ok || chain != send.chain || chain >= chains.size())
        return fail(SEND_REQ, "The gateway's offer was garbled");
    const Chain &c = chains[chain];
    uint8_t pub[33];
    account->coinPublic(c.network, pub);
    Bytes pay = scriptFor(send.toAddress, c.p2pkh, c.p2sh);
    if (pay.empty())
        return fail(SEND_REQ, "The payee's address is not a " + c.coinName() + (c.mainnet ? "" : " testnet") + " address");
    offerChecked = checkOffer(offerRaw, offerValues, pub, pay, send.amount, FEE_CAP);
    if (!offerChecked.ok)
        return fail(SEND_REQ, "Refused: " + offerChecked.why);
    (void)fee;
    (void)change; // the device's own sums are what it shows
    send.fee = offerChecked.fee;
    send.change = offerChecked.change;
    send.state = SendState::Review;
    if (fromQueue) {
        // Asked for while no gateway answered: the owner already chose who and
        // how much. A payee in the address book whose address the gateway
        // confirms is paid now; anything else waits at Review for the owner.
        std::string tag = send.to;
        while (!tag.empty() && tag[0] == '@')
            tag.erase(0, 1);
        for (auto &c : contacts)
            if (c.tag == tag && c.address == send.toAddress) {
                confirmSend();
                return;
            }
    }
}

void ArcadeClient::confirmSend(void)
{
    if (send.state != SendState::Review || !account)
        return;
    const Chain &c = chains[send.chain];
    Writer w;
    w.u8((uint8_t)offerChecked.digests.size());
    for (auto &d : offerChecked.digests) {
        uint8_t sig[73];
        size_t n = account->signDigest(c.network, d.data(), sig);
        if (!n)
            return fail(SIGN, "This T-Deck could not sign");
        w.u8((uint8_t)n).raw(sig, n);
    }
    uint8_t pub[33];
    account->coinPublic(c.network, pub);
    w.raw(pub, 33);
    send.state = SendState::Signing;
    request(SIGN, w.done());
    note();
}

void ArcadeClient::cancelSend(void)
{
    send = {};
    fromQueue = false;
    nextMail();
    runPayQueue();
    note();
}

void ArcadeClient::tick(uint32_t nowMs)
{
    now = nowMs;
    joiner.expire(nowMs);
    bool dirty = false;
    for (auto it = pending.begin(); it != pending.end();) {
        if (nowMs - it->second.at > REQUEST_TIMEOUT_MS) {
            Pending &p = it->second;
            if (p.type != PING && p.tries < REQUEST_TRIES && !p.frames.empty() && sendFrame) {
                p.tries++;
                p.at = nowMs;
                for (auto &f : p.frames)
                    sendFrame(p.to, f);
                ++it;
                continue;
            }
            uint8_t type = p.type;
            it = pending.erase(it);
            if (type == PING)
                continue;
            fail(type, "No answer from the gateway");
            // only failing to sign in means the gateway is gone; one lost
            // balance answer does not
            if (type == HELLO || type == LOGIN)
                link = account ? Link::Searching : link;
            dirty = true;
        } else {
            ++it;
        }
    }
    if (account && link == Link::Searching && nowMs - lastPing > PING_EVERY_MS) {
        findGateway();
        status = "Looking for a gateway";
        dirty = true;
    }
    if (link == Link::Online && nowMs - lastBalances > BALANCES_EVERY_MS)
        refreshBalances();
    if (link == Link::Online) {
        nextMail();    // anything waiting whose retry time has come
        runPayQueue();
    }
    if (dirty)
        note();
}

} // namespace arcade

// ---- messages -----------------------------------------------------------------

namespace arcade
{

static std::string threadFile(const std::string &tag)
{
    return "mail-" + tag;
}

std::vector<Letter> &ArcadeClient::thread(const std::string &tag)
{
    auto it = threads.find(tag);
    if (it != threads.end())
        return it->second;
    std::vector<Letter> &t = threads[tag];
    Bytes b;
    if (storage.load(threadFile(tag), b)) {
        Reader r(b);
        for (uint16_t n = r.u16(), i = 0; i < n && r.ok; i++) {
            Letter l;
            uint8_t flags = r.u8();
            l.mine = flags & 1;
            l.state = r.u8();
            l.when = r.u32();
            Bytes text = r.bytes();
            l.text.assign(text.begin(), text.end());
            l.txid = r.str();
            l.why = r.str();
            if (r.ok)
                t.push_back(l);
        }
    }
    // a letter that was going out when the T-Deck went off did not go
    for (auto &l : t)
        if (l.state == Letter::Sending) {
            l.state = Letter::Failed;
            l.why = "The T-Deck restarted before it went";
        }
    return t;
}

void ArcadeClient::saveThread(const std::string &tag)
{
    auto &t = thread(tag);
    if (t.size() > 60)
        t.erase(t.begin(), t.end() - 60); // the newest sixty stay
    Writer w;
    w.u16((uint16_t)t.size());
    for (auto &l : t)
        w.u8(l.mine ? 1 : 0).u8(l.state).u32(l.when).bytes(Bytes(l.text.begin(), l.text.end())).str(l.txid).str(l.why);
    storage.save(threadFile(tag), w.done());
    if (!t.empty())
        threadTime[tag] = t.back().when ? t.back().when : 1;
}

std::vector<std::string> ArcadeClient::threadTags(void)
{
    // every contact, the ones with the newest letters first
    std::vector<std::pair<uint32_t, std::string>> order;
    for (auto &c : contacts) {
        auto &t = thread(c.tag);
        uint32_t when = 0;
        for (auto &l : t)
            when = l.when > when ? l.when : when;
        if (!t.empty() && !when)
            when = 1;
        order.push_back({when, c.tag});
    }
    std::stable_sort(order.begin(), order.end(), [](const std::pair<uint32_t, std::string> &a,
                                                    const std::pair<uint32_t, std::string> &b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto &o : order)
        out.push_back(o.second);
    return out;
}

void ArcadeClient::markRead(const std::string &tag)
{
    if (unread.erase(tag))
        note();
}

const Contact *ArcadeClient::contactByKey(const uint8_t key[32]) const
{
    for (auto &c : contacts)
        if (memcmp(c.key, key, 32) == 0)
            return &c;
    return nullptr;
}

const Contact *ArcadeClient::contactByAddress(const std::string &address) const
{
    for (auto &c : contacts)
        if (c.address == address)
            return &c;
    return nullptr;
}

bool ArcadeClient::sendMessage(const std::string &tag, const std::string &text, std::string &why)
{
    const Contact *c = nullptr;
    for (auto &k : contacts)
        if (k.tag == tag)
            c = &k;
    static const uint8_t zero[32] = {0};
    if (!account) {
        why = "Unlock first";
        return false;
    }
    if (!c || !memcmp(c->key, zero, 32)) {
        why = "No mail key for @" + tag + ". Sync the address book, or add them again.";
        return false;
    }
    if (text.empty())
        return false;
    if (text.size() > MESSAGE_MAX) {
        why = "Too long for LoRa: " + std::to_string(MESSAGE_MAX) + " characters at most";
        return false;
    }
    Bytes sealed = sealMessage(*account, c->key, text);
    if (sealed.empty() || sealed.size() > MAIL_MAX) {
        why = "This T-Deck could not seal the message";
        return false;
    }
    auto &t = thread(tag);
    Letter l;
    l.mine = true;
    l.state = Letter::Sending;
    l.when = unixNow;
    l.text = text;
    t.push_back(l);
    saveThread(tag);
    mailQueue.push_back({tag, c->address, sealed, t.size() - 1});
    nextMail();
    note();
    return true;
}

bool ArcadeClient::resendMessage(const std::string &tag, size_t index, std::string &why)
{
    auto &t = thread(tag);
    if (index >= t.size() || !t[index].mine || t[index].state != Letter::Failed) {
        why = "Only a message that was not sent can be sent again";
        return false;
    }
    const Contact *c = nullptr;
    for (auto &k : contacts)
        if (k.tag == tag)
            c = &k;
    static const uint8_t zero[32] = {0};
    if (!account || !c || !memcmp(c->key, zero, 32)) {
        why = account ? "No mail key for @" + tag : "Unlock first";
        return false;
    }
    Bytes sealed = sealMessage(*account, c->key, t[index].text); // a fresh nonce: never the same box twice
    if (sealed.empty() || sealed.size() > MAIL_MAX) {
        why = "This T-Deck could not seal the message";
        return false;
    }
    t[index].state = Letter::Sending;
    t[index].why.clear();
    t[index].when = unixNow ? unixNow : t[index].when;
    saveThread(tag);
    mailQueue.push_back({tag, c->address, sealed, index});
    nextMail();
    note();
    return true;
}

void ArcadeClient::nextMail(void)
{
    // One offer at a time per T-Deck (the gateway keeps one), so letters go in
    // turn, and never while a coin send is being reviewed.
    if (mailInFlight || mailQueue.empty() || link != Link::Online)
        return;
    if (mailRetryAt && (int32_t)(now - mailRetryAt) < 0)
        return;
    mailRetryAt = 0;
    if (send.state == SendState::Asking || send.state == SendState::Review || send.state == SendState::Signing)
        return;
    mailInFlight = true;
    signingMail = false;
    const Outgoing &o = mailQueue.front();
    request(MAIL_POST, Writer().str(o.address).bytes(o.sealed).done());
}

void ArcadeClient::onMailOffer(const Message &m)
{
    Reader r(m.body);
    uint8_t chain = r.u8();
    r.str();
    r.str();
    r.amount();
    r.amount();
    Bytes raw = r.bytes();
    std::vector<uint64_t> values;
    for (uint8_t n = r.u8(), i = 0; i < n && r.ok; i++)
        values.push_back(r.amount());
    if (!r.ok || chain >= chains.size() || mailQueue.empty())
        return fail(MAIL_POST, "The gateway's offer was garbled");
    uint8_t pub[33];
    account->coinPublic(chains[chain].network, pub);
    // A message pays dust only: the marker, 0.01 per 60 bytes of payload in
    // the multisig outputs, and 0.01 to the recipient as the notice.
    size_t packets = (mailQueue.front().sealed.size() + 12 + 59) / 60;
    uint64_t othersCap = (uint64_t)(packets + 3) * 1000000ULL;
    Checked c = checkOffer(raw, values, pub, Bytes(), 0, FEE_CAP, othersCap);
    if (!c.ok)
        return fail(MAIL_POST, "Refused: " + c.why);
    Writer w;
    w.u8((uint8_t)c.digests.size());
    for (auto &d : c.digests) {
        uint8_t sig[73];
        size_t n = account->signDigest(chains[chain].network, d.data(), sig);
        if (!n)
            return fail(MAIL_POST, "This T-Deck could not sign");
        w.u8((uint8_t)n).raw(sig, n);
    }
    w.raw(pub, 33);
    signingMail = true;
    request(SIGN, w.done());
}

void ArcadeClient::onMail(const Message &m, bool tooLong)
{
    Reader r(m.body);
    uint8_t txid[32];
    r.take(txid, 32);
    uint32_t when = r.u32();
    std::string from = r.str();
    Bytes sealed;
    uint16_t size = 0;
    if (tooLong)
        size = r.u16();
    else
        sealed = r.bytes();
    if (!r.ok || !account)
        return;
    std::string fromTag = r.end() ? "" : r.str(); // newer gateways name the sender
    if (!r.ok)
        fromTag.clear();
    std::string tx = toHex(txid, 32);
    Letter l;
    l.when = when;
    l.txid = tx;
    std::string tag;
    if (tooLong) {
        l.state = Letter::TooLong;
        l.text = "Message too long for LoRa (" + std::to_string(size) + " bytes). Read it on the arcade.";
        const Contact *c = contactByAddress(from);
        tag = c ? c->tag : fromTag;
    } else {
        Opened o = openMessage(*account, sealed);
        if (o.ok) {
            const Contact *c = contactByKey(o.sender);
            if (!c)
                c = contactByAddress(from);
            tag = c ? c->tag : fromTag;
            if (tag.empty())
                tag = from; // no @tag known: the thread is named by the address
            l.state = Letter::Received;
            l.text = o.text + (o.hadFile ? " [a picture: open it on the arcade]" : "");
        }
    }
    // file it (once) and send the signed receipt either way, so the gateway
    // stops carrying it; mail this account cannot open is not ours to keep
    if (!tag.empty()) {
        auto &t = thread(tag);
        bool have = false;
        for (auto &x : t)
            have = have || x.txid == tx;
        if (!have) {
            t.push_back(l);
            saveThread(tag);
            unread[tag]++;
        }
        // somebody not in the address book yet: add them, so the thread shows
        // and a reply can be sealed to their key
        bool known = false;
        for (auto &c : contacts)
            known = known || c.tag == tag;
        if (!known && !fromTag.empty() && tag == fromTag) {
            Contact c;
            c.tag = tag;
            c.address = from;
            contacts.push_back(c);
            saveContacts();
            request(WHO_REQ, Writer().str(tag).done()); // fills in their mail key
        }
    }
    std::string receipt = "arcade-lora receipt\n" + tx;
    uint8_t sig[64];
    account->signEd25519((const uint8_t *)receipt.data(), receipt.size(), sig);
    Bytes body(txid, txid + 32);
    body.insert(body.end(), sig, sig + 64);
    request(MAIL_ACK, body, 0, false); // the gateway answers a receipt with nothing
}

} // namespace arcade
