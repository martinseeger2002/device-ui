#pragma once

// The T-Deck side of PROTOCOL.md: finding a gateway, signing in, balances,
// contacts and sending coins. Transport-free (frames in through receive(),
// out through sendFrame) and screen-free (the view reads the public state
// and is told when it changed), so it runs in host tests too.

#include "arcade/Frames.h"
#include "arcade/Keys.h"
#include "arcade/Tx.h"
#include <functional>
#include <memory>

namespace arcade
{

struct Snapshot;
struct Coin;

// Where the T-Deck keeps its few files (flash on the device, a folder in tests).
class Storage
{
  public:
    virtual ~Storage() = default;
    virtual bool load(const std::string &name, Bytes &out) = 0;
    virtual bool save(const std::string &name, const Bytes &data) = 0;
    virtual void remove(const std::string &name) = 0;
};

// The twelve words, sealed with a key made from the PIN.
class KeyStore
{
  public:
    explicit KeyStore(Storage &s) : storage(s) {}
    bool exists(void);
    bool create(const std::string &phrase, const std::string &pin);
    bool unlock(const std::string &pin, std::string &phrase);
    void erase(void);
    static constexpr uint32_t ROUNDS = 20000;

  private:
    Storage &storage;
};

struct Chain {
    std::string network, label;
    bool tags = false, mainnet = false;
    uint8_t p2pkh = 0, p2sh = 0;
    uint64_t confirmed = 0, pending = 0;
    bool registered = false;
    std::string symbol(void) const;
    std::string coinName(void) const;   // "Pepecoin", "Dogecoin": what people call it, test chain or not
};

struct Contact {
    std::string tag, address, name;
    uint8_t key[32] = {0};
};

struct Letter {
    enum State : uint8_t { Sending = 0, Sent = 1, Failed = 2, Received = 3, TooLong = 4 };
    bool mine = false;
    uint8_t state = Sending;
    uint32_t when = 0;   // unix time when known
    std::string text, txid, why;
};

struct Activity {
    std::string who;     // "@robin" or an address
    std::string symbol;
    int64_t amount = 0;  // base units, + in / - out
    uint32_t when = 0;   // device clock seconds
    std::string txid;
    bool offline = false;
};

class ArcadeClient
{
  public:
    enum class Link { Locked, Searching, SigningIn, Online, NoAccount };
    enum class SendState { Idle, Asking, Review, Signing, Sent, Failed, Queued };
    enum class WhoState { Idle, Asking, Found, Failed };
    enum class WifiState { Idle, Joining, SigningIn, Downloading, Done, Failed };

    ArcadeClient(Storage &storage, uint32_t myNode);

    std::function<void(uint32_t to, const Bytes &frame)> sendFrame;  // to = 0xFFFFFFFF broadcasts
    std::function<void(void)> changed;                                 // the screen should redraw

    // ---- account ----
    bool hasAccount(void) { return keys.exists(); }
    bool createAccount(const std::string &phrase, const std::string &pin);
    bool unlock(const std::string &pin);
    void lock(void);
    bool unlocked(void) const { return account != nullptr; }
    std::string myAddress(size_t chain) const
    {
        return account && chain < chains.size() ? account->address(chains[chain].network, chains[chain].p2pkh) : "";
    }

    // ---- radio ----
    void receive(uint32_t from, const uint8_t *data, size_t len, uint32_t nowMs);
    void tick(uint32_t nowMs);
    void findGateway(void);

    // ---- actions ----
    void refreshBalances(void);
    void addContact(const std::string &tag);
    void removeContact(const std::string &tag);
    void askToSend(size_t chain, const std::string &to, uint64_t amount);
    void confirmSend(void);
    void cancelSend(void);

    // payments asked for with no gateway in range wait here (kept on flash)
    struct QueuedPay {
        std::string network, to;
        uint64_t amount = 0;
        uint32_t when = 0;
    };
    std::vector<QueuedPay> payQueue;
    std::string payQueueError;       // why the last waiting payment did not go
    uint64_t waitingOut(const std::string &network) const;   // what queued payments will take from a wallet
    uint64_t available(size_t chain) const;                  // the balance less what is waiting to go
    bool cancelQueued(size_t index);                         // take a waiting payment back

    // ---- messages ----
    static constexpr size_t MAIL_MAX = 645;                          // sealed bytes the gateway carries (3 frames)
    static constexpr size_t MESSAGE_MAX = MAIL_MAX - 126;            // characters (bytes) a message may hold
    bool sendMessage(const std::string &tag, const std::string &text, std::string &why);
    bool resendMessage(const std::string &tag, size_t index, std::string &why);   // a "not sent" letter, sealed afresh
    std::vector<Letter> &thread(const std::string &tag);             // loaded from flash on first use
    std::vector<std::string> threadTags(void);                       // contacts with letters, newest first
    std::map<std::string, int> unread;
    void markRead(const std::string &tag);

    // ---- address book over WiFi ----
    // the platform joins WiFi, signs in over HTTPS and fetches the mailbox
    std::function<void(const std::string &ssid, const std::string &pass, const std::string &host)> startWifiSync;
    struct WifiNet {
        std::string ssid;
        int rssi = 0;
        bool secure = true;
    };
    std::function<void(void)> startWifiScan;                         // the platform looks for networks
    std::vector<WifiNet> wifiNets;
    bool wifiScanning = false;
    void scanWifi(void);
    bool wifiOn = true;                                              // the owner's switch on the Settings page
    void setWifiOn(bool on);
    void wifiScanned(std::vector<WifiNet> nets);                     // from the platform, on the UI thread
    bool saveWifi(const std::string &ssid, const std::string &pass);
    bool loadWifi(std::string &ssid, std::string &pass);
    void syncOverWifi(void);
    void wifiProgress(WifiState s, const std::string &why = "");     // from the platform, on the UI thread
    bool importMailbox(const std::string &blob);                     // opens it, keeps the book
    void importBook(const std::vector<Contact> &book);
    std::string arcadeHost(void) const;                              // where to sync from
    void applySnapshot(const Snapshot &snap);                        // @tag, chains, balances from the arcade
    void saveCoins(const std::string &network, const std::vector<Coin> &coins);
    size_t cachedCoins(const std::string &network);
    void setClock(uint32_t unix) { unixNow = unix; }
    void signLogin(const std::string &origin, const std::string &nonce, uint8_t sig[64]) const;
    const uint8_t *loginPublic(void) const { return account ? account->loginPublic() : nullptr; }
    // the keys for work on another task: locking drops the client's hold, not this one
    std::shared_ptr<const Account> accountKeys(void) const { return account; }

    // ---- what the screen shows ----
    Link link = Link::Locked;
    std::string status;               // one line, plain English
    uint32_t gateway = 0;
    std::string gatewayName, gatewayHost;
    int8_t gatewaySnr = 0;
    std::string tag;
    std::vector<Chain> chains;
    std::vector<Contact> contacts;
    std::vector<Activity> activity;
    uint32_t balancesAt = 0;          // ms
    std::string balancesFrom;         // "gateway", "WiFi", or "" when only cached
    uint32_t balancesWhen = 0;        // unix time of those balances, 0 = unknown

    struct {
        SendState state = SendState::Idle;
        size_t chain = 0;
        std::string to, toTag, toAddress, why, txid;
        uint64_t amount = 0, fee = 0, change = 0;
    } send;
    struct {
        WhoState state = WhoState::Idle;
        std::string tag, why;
    } who;
    struct {
        WifiState state = WifiState::Idle;
        std::string why, ssid;
        size_t imported = 0, removed = 0;
    } wifi;

    static uint64_t FEE_CAP;          // base units: refuse any offer paying more in fees

  private:
    void request(uint8_t type, const Bytes &body, uint32_t to = 0, bool expectAnswer = true);
    void handle(uint32_t from, const Message &m, uint32_t nowMs);
    void onWelcome(const Message &m);
    void onOffer(const Message &m);
    void fail(uint8_t type, const std::string &why);
    void registerAddresses(void);
    void sendWatch(void);
    void onMailOffer(const Message &m);
    void runPayQueue(void);
    void savePayQueue(void);
    bool fromQueue = false;          // the send in progress came out of payQueue
    void onMail(const Message &m, bool tooLong);
    void nextMail(void);
    void saveThread(const std::string &tag);
    const Contact *contactByKey(const uint8_t key[32]) const;
    const Contact *contactByAddress(const std::string &address) const;
    std::map<std::string, std::vector<Letter>> threads;
    std::map<std::string, uint32_t> threadTime;
    struct Outgoing {
        std::string tag, address;
        Bytes sealed;
        size_t index;
    };
    std::vector<Outgoing> mailQueue;
    bool mailInFlight = false, signingMail = false;
    uint32_t mailRetryAt = 0, payRetryAt = 0;   // after a lost answer: try again then, by itself
    void loadState(void);
    void saveContacts(void);
    void saveActivity(void);
    void note(void)
    {
        if (changed)
            changed();
    }

    Storage &storage;
    KeyStore keys;
    std::shared_ptr<Account> account;   // shared: a WiFi sync in flight keeps its own hold
    uint32_t myNode;
    Joiner joiner;
    uint16_t nextId = 1;
    uint32_t now = 0, lastPing = 0, lastBalances = 0, unixNow = 0;
    void saveAccountCache(void);
    struct Pending {
        uint8_t type;
        uint32_t at;
        uint32_t to = 0;
        uint8_t tries = 1;
        std::vector<Bytes> frames;   // kept, so a lost answer can be asked for again with the same id
    };
    std::map<uint16_t, Pending> pending;
    std::map<uint16_t, size_t> addrAsked;  // ADDR request id -> chain
    Bytes offerRaw;
    std::vector<uint64_t> offerValues;
    Checked offerChecked;
};

} // namespace arcade
