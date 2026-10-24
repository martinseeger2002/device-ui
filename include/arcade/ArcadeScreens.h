#pragma once

// The arcade T-Deck's screens, in plain LVGL: setup, lock, wallet, send,
// contacts and mesh, in the arcade's six skins. They read ArcadeClient and
// call its actions; nothing here knows about Meshtastic, so the same code
// draws on the T-Deck and into a framebuffer on a PC for screenshots.

#include "arcade/ArcadeClient.h"
#include "arcade/Radio.h"
#include "lvgl.h"
#include <functional>
#include <memory>

namespace arcade
{

struct Skin {
    const char *brand;   // "Dogecoin", "Pepecoin", "Litecoin"
    bool dark;
    uint32_t bg, panel, fg, muted, line, accent, accentFg, ok, warn, bad, code, sent;
};
extern const Skin SKINS[6];

class ArcadeScreens
{
  public:
    enum class Scr { Welcome, Words, Pin, PinAgain, Lock, Wallet, Receive, Send, Contacts, Contact, AddContact, Messages, Mesh, Wifi, Settings, Chat };

    ArcadeScreens(ArcadeClient &client, Storage &storage);
    void begin(lv_group_t *group);       // build the first screen on the active display
    void clientChanged(void);            // ArcadeClient said something changed
    void tick(uint32_t nowMs);           // hold-to-sign, lock-out, slow refreshes

    // what the platform knows that the client does not
    void setBattery(int percent) { battery = percent; }
    void setClock(uint32_t unixTime) { wallClock = unixTime; }
    void setMyNode(uint32_t node) { myNode = node; }
    void setRadio(Radio *r) { radio = r; }
    void setRadioState(const RadioState &s);

    // for screenshots and tests
    void show(Scr s);
    void setSkin(size_t index);
    Scr current(void) const { return stack.empty() ? Scr::Welcome : stack.back(); }
    std::string sendTo, sendAmount, addTag, wifiSsid, wifiPass, draft, chatTag, chatError;   // form fields, kept across redraws

  private:
    using Action = std::function<void(void)>;

    void render(void);   // asks for a rebuild on the next tick
    void rebuild(void);
    std::string signature(void) const;
    void go(Scr s);
    void back(void);
    void tab(int delta);

    // building blocks
    lv_obj_t *box(lv_obj_t *parent, bool row = false);
    lv_obj_t *card(lv_obj_t *parent);
    lv_obj_t *text(lv_obj_t *parent, const std::string &s, const lv_font_t *font, uint32_t color);
    lv_obj_t *button(lv_obj_t *parent, const std::string &label, Action a, bool primary = true, bool enabled = true);
    lv_obj_t *pill(lv_obj_t *parent, const std::string &label, bool on, Action a);
    lv_obj_t *chip(lv_obj_t *parent, const std::string &label, uint32_t color);
    lv_obj_t *input(lv_obj_t *parent, std::string &field, const char *placeholder, const char *accepted, size_t max,
                    bool password = false, Action onEnter = nullptr, Action onChange = nullptr);
    void touchOnlyControls(lv_obj_t *root, lv_obj_t *pane);
    lv_obj_t *avatar(lv_obj_t *parent, const std::string &tag, int size);
    lv_obj_t *kv(lv_obj_t *parent, const std::string &k, const std::string &v, bool mono = true);
    void header(lv_obj_t *parent, const std::string &title);
    void focusable(lv_obj_t *o, Action a = nullptr);

    // screens
    void statusBar(lv_obj_t *parent);
    void navBar(lv_obj_t *parent);
    void scrWelcome(lv_obj_t *p);
    void scrWords(lv_obj_t *p);
    void scrPin(lv_obj_t *p, bool again);
    void scrLock(lv_obj_t *p);
    void scrWallet(lv_obj_t *p);
    void scrReceive(lv_obj_t *p);
    void scrSend(lv_obj_t *p);
    void scrContacts(lv_obj_t *p);
    void scrContact(lv_obj_t *p);
    void scrAddContact(lv_obj_t *p);
    void scrMessages(lv_obj_t *p);
    void scrMesh(lv_obj_t *p);
    void scrWifi(lv_obj_t *p);
    void scrSettings(lv_obj_t *p);
    void scrChat(lv_obj_t *p);
    lv_obj_t *section(lv_obj_t *parent, const char *title);
    lv_obj_t *toggle(lv_obj_t *parent, const std::string &label, bool on, std::function<void(bool)> set);

    static void onEvent(lv_event_t *e);
    static void onKey(lv_event_t *e);
    static void onHold(lv_event_t *e);
    static void onGesture(lv_event_t *e);

    ArcadeClient &client;
    Storage &storage;
    lv_group_t *group = nullptr;
    lv_obj_t *screen = nullptr;
    const Skin *skin = &SKINS[1];
    size_t skinIndex = 1;
    std::vector<Scr> stack;
    std::vector<std::unique_ptr<Action>> actions;   // owned for the life of one render
    std::vector<lv_obj_t *> order;                  // focus order, to put focus back after a redraw
    std::string lastSig;
    size_t focusAt = 0;
    bool dirty = false;
    uint32_t rebuiltAt = 0;

    // setup and lock
    std::vector<std::string> words;
    std::string word, pin, pinAgain, lockPin, setupError;
    int lockTries = 0;
    uint32_t lockedUntil = 0;
    bool working = false;           // a slow key operation is about to run
    Action workAfterDraw;

    // send
    size_t sendChain = 0;
    lv_obj_t *holdBar = nullptr;
    lv_obj_t *backButton = nullptr;
    uint32_t holdStart = 0;
    bool holding = false;
    std::string contactTag;
    bool wifiFocusPassword = false;
    bool focusAmount = false;
    std::string walletNote;

    Radio *radio = nullptr;
    RadioState radioState;
    int battery = -1;
    uint32_t wallClock = 0;
    uint32_t myNode = 0;
    uint32_t now = 0;
};

std::string formatAmount(uint64_t base);              // 1284.5 -> "1,284.50"
bool parseAmount(const std::string &text, uint64_t &base);

} // namespace arcade
