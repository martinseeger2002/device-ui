#include "arcade/ArcadeScreens.h"

#include <stdio.h>
#include <string.h>

extern "C" {
extern const lv_font_t arcade_roboto_10;
extern const lv_font_t arcade_roboto_12;
extern const lv_font_t arcade_roboto_medium_12;
extern const lv_font_t arcade_roboto_medium_15;
extern const lv_font_t arcade_mono_12;
extern const lv_font_t arcade_mono_22;
}

namespace arcade
{

// The arcade's own faces (arcade/web/brand.py + base_css.css), light and dark.
const Skin SKINS[6] = {
    {"Dogecoin", false, 0xfaf8f4, 0xfffdf9, 0x1b1a17, 0x6b665c, 0xe2ddd3, 0xb8860b, 0xffffff, 0x2d7a4f, 0xa8620a, 0xa83232, 0xf2eee6, 0x2f7cf6},
    {"Dogecoin", true, 0x141310, 0x1a1815, 0xece8e0, 0x9b948a, 0x2c2a25, 0xd9a520, 0x141310, 0x5bbd85, 0xd99a3e, 0xe07070, 0x1d1b17, 0x0b84ff},
    {"Pepecoin", false, 0xf4f8f1, 0xfbfdf9, 0x14200f, 0x5a6a52, 0xd6e3cd, 0x2f8f3a, 0xffffff, 0x1f7a5c, 0xa8620a, 0xa83232, 0xe9f2e3, 0x2f7cf6},
    {"Pepecoin", true, 0x0d140e, 0x131c14, 0xe4efe0, 0x8ea486, 0x223024, 0x5fcb5a, 0x0d140e, 0x4cc29a, 0xd99a3e, 0xe07070, 0x162017, 0x0b84ff},
    {"Litecoin", false, 0xf3f5f9, 0xfbfcfe, 0x141a26, 0x5b6475, 0xd5dbe6, 0x345d9d, 0xffffff, 0x1f7a5c, 0xa8620a, 0xa83232, 0xe8edf5, 0x2f7cf6},
    {"Litecoin", true, 0x0d1118, 0x131924, 0xe3e8f1, 0x8b95a8, 0x222b3a, 0x7fa3dc, 0x0d1118, 0x4cc29a, 0xd99a3e, 0xe07070, 0x161d2a, 0x0b84ff},
};

static const char SETTINGS_FILE[] = "settings";
static const uint32_t HOLD_MS = 1100;

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static uint32_t chainColor(const Chain &c)
{
    if (c.network == "doge")
        return 0xc2a633;
    if (c.network == "ltc" || c.network == "ltctest")
        return 0x345d9d;
    if (c.network == "lky")
        return 0xb04a3a;
    return 0x2f8f3a;
}


std::string formatAmount(uint64_t base)
{
    uint64_t whole = base / 100000000ULL, frac = base % 100000000ULL;
    std::string w = std::to_string(whole), grouped;
    for (size_t i = 0; i < w.size(); i++) {
        if (i && (w.size() - i) % 3 == 0)
            grouped += ',';
        grouped += w[i];
    }
    // digits by hand: the ESP32's printf has no 64-bit %llu (it printed "lu")
    std::string fs(8, '0');
    for (int i = 7; i >= 0; i--) {
        fs[i] = (char)('0' + frac % 10);
        frac /= 10;
    }
    while (fs.size() > 2 && fs.back() == '0')
        fs.pop_back();
    return grouped + "." + fs;
}

bool parseAmount(const std::string &text, uint64_t &base)
{
    uint64_t whole = 0, frac = 0;
    int fracDigits = -1;
    for (char c : text) {
        if (c == ',' || c == ' ')
            continue;
        if (c == '.') {
            if (fracDigits >= 0)
                return false;
            fracDigits = 0;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
        if (fracDigits >= 0) {
            if (++fracDigits > 8)
                return false;
            frac = frac * 10 + (c - '0');
        } else {
            whole = whole * 10 + (c - '0');
            if (whole > 90000000000ULL)
                return false;
        }
    }
    for (int i = fracDigits < 0 ? 0 : fracDigits; i < 8; i++)
        frac *= 10;
    base = whole * 100000000ULL + frac;
    return base > 0;
}

static uint32_t avatarColor(const std::string &tag)
{
    static const uint32_t colors[] = {0xc0567a, 0xd64545, 0x3f9a4a, 0x7a5bc4, 0x2c8ac9, 0xc77a1e, 0x1f8f86, 0x8a5a44};
    uint32_t h = 5381;
    for (char c : tag)
        h = h * 33 + (uint8_t)c;
    return colors[h % 8];
}

static std::string shortAddress(const std::string &a)
{
    return a.size() > 16 ? a.substr(0, 6) + "\xE2\x80\xA6" + a.substr(a.size() - 6) : a;
}

// ---- lifecycle --------------------------------------------------------------

ArcadeScreens::ArcadeScreens(ArcadeClient &c, Storage &s) : client(c), storage(s)
{
    Bytes b;
    if (storage.load(SETTINGS_FILE, b) && !b.empty() && b[0] < 6)
        skinIndex = b[0];
    skin = &SKINS[skinIndex];
}

void ArcadeScreens::begin(lv_group_t *g)
{
    group = g;
    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
    lv_screen_load(screen);
    stack.clear();
    stack.push_back(client.hasAccount() ? Scr::Lock : Scr::Welcome);
    rebuild();
}

void ArcadeScreens::setSkin(size_t index)
{
    skinIndex = index % 6;
    skin = &SKINS[skinIndex];
    storage.save(SETTINGS_FILE, Bytes{(uint8_t)skinIndex});
    render();
}

void ArcadeScreens::show(Scr s)
{
    stack.clear();
    stack.push_back(s);
    focusAt = 0;
    render();
}

void ArcadeScreens::go(Scr s)
{
    stack.push_back(s);
    focusAt = 0;
    render();
}

void ArcadeScreens::back(void)
{
    if (stack.size() > 1)
        stack.pop_back();
    focusAt = 0;
    render();
}

static bool isTab(ArcadeScreens::Scr s)
{
    using S = ArcadeScreens::Scr;
    return s == S::Wallet || s == S::Contacts || s == S::Messages || s == S::Mesh || s == S::Settings;
}

void ArcadeScreens::tab(int delta)
{
    static const Scr tabs[] = {Scr::Wallet, Scr::Contacts, Scr::Messages, Scr::Mesh, Scr::Settings};
    int at = 0;
    for (int i = 0; i < 5; i++)
        if (tabs[i] == current())
            at = i;
    at = (at + delta + 5) % 5;
    show(tabs[at]);
}

std::string ArcadeScreens::signature(void) const
{
    // What, besides navigation, should make the current screen redraw. Forms
    // only redraw when their own answer arrives, so typing is never lost.
    std::string s = std::to_string((int)client.link) + client.status + client.tag + client.gatewayName;
    switch (current()) {
    case Scr::Send:
        return s + std::to_string((int)client.send.state) + client.send.why;
    case Scr::AddContact:
        return s + std::to_string((int)client.who.state) + client.who.why;
    case Scr::Wifi: {
        std::string nets;
        for (auto &n : client.wifiNets)
            nets += n.ssid + ",";
        return s + std::to_string((int)client.wifi.state) + client.wifi.why + std::to_string(client.wifiScanning) + nets;
    }
    case Scr::Chat: {
        std::string t = chatError;
        for (auto &l : client.thread(chatTag))
            t += std::to_string(l.state) + l.txid + l.why + "|";
        return s + t;
    }
    case Scr::Messages: {
        std::string t;
        for (auto &u : client.unread)
            t += u.first + std::to_string(u.second);
        for (auto &tag : client.threadTags())
            t += tag + std::to_string(client.thread(tag).size());
        return s + t;
    }
    case Scr::Settings:
        return s + std::to_string((int)client.wifi.state) + std::to_string(radioState.gpsMode) +
               std::to_string(radioState.sats) + std::to_string(radioState.fix) + std::to_string(radioState.btEnabled) +
               std::to_string(radioState.btConnected) + std::to_string(radioState.btPin) +
               std::to_string(radioState.powerSaving) + std::to_string(radioState.screenTimeout) + radioState.longName;
    case Scr::Words:
    case Scr::Pin:
    case Scr::PinAgain:
    case Scr::Lock:
        return std::to_string((int)client.link);
    default:
        break;
    }
    for (auto &c : client.chains)
        s += c.network + std::to_string(c.confirmed) + "/" + std::to_string(c.pending);
    s += std::to_string(client.payQueue.size()) + std::to_string((int)client.send.state) + client.payQueueError + walletNote;
    s += std::to_string(client.contacts.size()) + std::to_string(client.activity.size());
    return s;
}

void ArcadeScreens::clientChanged(void)
{
    if (!screen)
        return;
    if (client.link == ArcadeClient::Link::Locked && current() != Scr::Lock && current() != Scr::Welcome &&
        client.hasAccount()) {
        show(Scr::Lock);
        return;
    }
    if (signature() != lastSig)
        dirty = true;
}

void ArcadeScreens::tick(uint32_t nowMs)
{
    now = nowMs;
    if (dirty) {
        rebuild();
        return;
    }
    if (working && workAfterDraw && nowMs - rebuiltAt > 200) {
        // the "please wait" screen has been on the display a moment; now do the slow part
        Action a = workAfterDraw;
        workAfterDraw = nullptr;
        a();
        working = false;
    }
    if (holding && holdBar) {
        uint32_t held = nowMs - holdStart;
        lv_obj_set_width(holdBar, LV_PCT(held >= HOLD_MS ? 100 : held * 100 / HOLD_MS));
        if (held >= HOLD_MS) {
            holding = false;
            holdBar = nullptr;
            client.confirmSend();
        }
    }
    if (current() == Scr::Lock && lockedUntil && nowMs >= lockedUntil) {
        lockedUntil = 0;
        dirty = true;
    }
}

static bool isTab(ArcadeScreens::Scr s);

// ---- events -----------------------------------------------------------------

void ArcadeScreens::touchOnlyControls(lv_obj_t *root, lv_obj_t *pane)
{
    // LVGL makes every plain object clickable, so a tap on a card landed on
    // the layout box inside it and did nothing. Only the real controls (and
    // the page, so it can be scrolled by dragging) take touches now; a tap
    // anywhere else falls through to the control beneath it.
    std::vector<lv_obj_t *> stack{root};
    while (!stack.empty()) {
        lv_obj_t *o = stack.back();
        stack.pop_back();
        bool control = o == root || o == pane;
        for (lv_obj_t *f : order)
            control = control || f == o;
        if (!control)
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        uint32_t n = lv_obj_get_child_count(o);
        for (uint32_t i = 0; i < n; i++)
            stack.push_back(lv_obj_get_child(o, i));
    }
}

void ArcadeScreens::onGesture(lv_event_t *e)
{
    // swipe left/right: the next or previous tab; on an inner screen a swipe
    // right goes back
    auto *self = (ArcadeScreens *)lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in)
        return;
    lv_dir_t dir = lv_indev_get_gesture_dir(in);
    Scr s = self->current();
    if (s == Scr::Welcome || s == Scr::Lock || s == Scr::Words || s == Scr::Pin || s == Scr::PinAgain)
        return;
    if (dir == LV_DIR_LEFT && isTab(s))
        self->tab(1);
    else if (dir == LV_DIR_RIGHT)
        isTab(s) ? self->tab(-1) : self->back();
    lv_indev_wait_release(in);
}

void ArcadeScreens::onEvent(lv_event_t *e)
{
    auto *a = (Action *)lv_event_get_user_data(e);
    if (a && *a)
        (*a)();
}

void ArcadeScreens::onKey(lv_event_t *e)
{
    // The trackball's left/right arrive as LV_KEY_DOWN/UP (device-ui's
    // encoder driver). On a tab they flip tabs; elsewhere left goes back.
    auto *self = (ArcadeScreens *)lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        self->back();
    } else if (key == LV_KEY_DOWN || key == LV_KEY_UP) {
        if (isTab(self->current()))
            self->tab(key == LV_KEY_UP ? 1 : -1);
        else if (key == LV_KEY_DOWN)
            self->back();
    }
}

void ArcadeScreens::onHold(lv_event_t *e)
{
    auto *self = (ArcadeScreens *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        self->holding = true;
        self->holdStart = lv_tick_get();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        self->holding = false;
        if (self->holdBar)
            lv_obj_set_width(self->holdBar, 0);
    }
}

// ---- building blocks ----------------------------------------------------------

void ArcadeScreens::focusable(lv_obj_t *o, Action a)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_outline_width(o, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(o, C(skin->accent), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, 1, LV_STATE_FOCUSED);
    if (a) {
        actions.emplace_back(new Action(a));
        lv_obj_add_event_cb(o, onEvent, LV_EVENT_CLICKED, actions.back().get());
    }
    if (!lv_obj_check_type(o, &lv_textarea_class))
        lv_obj_add_event_cb(o, onKey, LV_EVENT_KEY, this);
    if (group)
        lv_group_add_obj(group, o);
    order.push_back(o);
}

lv_obj_t *ArcadeScreens::box(lv_obj_t *parent, bool row)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(o, row ? 5 : 4, 0);
    if (row)
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ArcadeScreens::card(lv_obj_t *parent)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_style_bg_color(o, C(skin->panel), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, C(skin->line), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_hor(o, 8, 0);
    lv_obj_set_style_pad_ver(o, 6, 0);
    return o;
}

lv_obj_t *ArcadeScreens::text(lv_obj_t *parent, const std::string &s, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s.c_str());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    return l;
}

lv_obj_t *ArcadeScreens::button(lv_obj_t *parent, const std::string &label, Action a, bool primary, bool enabled)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_pad_ver(b, 5, 0);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, C(skin->accent), 0);
    lv_obj_set_style_bg_color(b, C(skin->accent), 0);
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_80 : LV_OPA_20, LV_STATE_PRESSED);
    if (!enabled)
        lv_obj_set_style_opa(b, LV_OPA_40, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = text(b, label, &arcade_roboto_medium_12, primary ? skin->accentFg : skin->accent);
    lv_obj_center(l);
    focusable(b, enabled ? a : nullptr);
    return b;
}

lv_obj_t *ArcadeScreens::pill(lv_obj_t *parent, const std::string &label, bool on, Action a)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 7, 0);
    lv_obj_set_style_pad_ver(b, 1, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, C(on ? skin->accent : skin->line), 0);
    lv_obj_set_style_bg_color(b, C(skin->panel), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    text(b, label, &arcade_roboto_10, on ? skin->accent : skin->fg);
    focusable(b, a);
    return b;
}

lv_obj_t *ArcadeScreens::chip(lv_obj_t *parent, const std::string &label, uint32_t color)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 4, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_bg_color(b, C(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    std::string up = label;
    for (auto &ch : up)
        ch = (char)toupper((unsigned char)ch);
    lv_obj_t *l = text(b, up, &arcade_roboto_10, color);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return b;
}

lv_obj_t *ArcadeScreens::input(lv_obj_t *parent, std::string &field, const char *placeholder, const char *accepted,
                               size_t max, bool password, Action onEnter, Action onChange)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_remove_style_all(ta);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_style_bg_color(ta, C(skin->bg), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, C(skin->line), 0);
    lv_obj_set_style_border_color(ta, C(skin->accent), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_hor(ta, 6, 0);
    lv_obj_set_style_pad_ver(ta, 4, 0);
    lv_obj_set_style_text_font(ta, &arcade_roboto_12, 0);
    lv_obj_set_style_text_color(ta, C(skin->fg), 0);
    lv_obj_set_style_text_color(ta, C(skin->muted), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(ta, C(skin->accent), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 1, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 500, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_placeholder_text(ta, placeholder);
    if (accepted)
        lv_textarea_set_accepted_chars(ta, accepted);
    lv_textarea_set_max_length(ta, max);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, field.c_str());
    std::string *f = &field;
    actions.emplace_back(new Action([ta, f, onChange]() {
        *f = lv_textarea_get_text(ta);
        if (onChange)
            onChange();
    }));
    lv_obj_add_event_cb(ta, onEvent, LV_EVENT_VALUE_CHANGED, actions.back().get());
    if (onEnter) {
        actions.emplace_back(new Action([ta, f, onEnter]() {
            *f = lv_textarea_get_text(ta);
            onEnter();
        }));
        lv_obj_add_event_cb(ta, onEvent, LV_EVENT_READY, actions.back().get());
    }
    focusable(ta);
    return ta;
}

lv_obj_t *ArcadeScreens::avatar(lv_obj_t *parent, const std::string &tag, int size)
{
    lv_obj_t *a = lv_obj_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, size, size);
    lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(a, C(avatarColor(tag)), 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    std::string first = tag.empty() ? "?" : std::string(1, (char)toupper((unsigned char)tag[0]));
    lv_obj_t *l = text(a, first, size > 16 ? &arcade_roboto_medium_12 : &arcade_roboto_10, 0xffffff);
    lv_obj_center(l);
    return a;
}

lv_obj_t *ArcadeScreens::kv(lv_obj_t *parent, const std::string &k, const std::string &v, bool mono)
{
    lv_obj_t *r = box(parent, true);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(r, k, &arcade_roboto_10, skin->muted);
    lv_obj_t *val = text(r, v, mono ? &arcade_mono_12 : &arcade_roboto_12, skin->fg);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(val, 200, 0);
    return r;
}

void ArcadeScreens::header(lv_obj_t *parent, const std::string &title)
{
    lv_obj_t *r = box(parent, true);
    lv_obj_t *b = lv_obj_create(r);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 4, 0);
    lv_obj_set_style_radius(b, 4, 0);
    text(b, LV_SYMBOL_LEFT, &arcade_roboto_12, skin->muted);
    focusable(b, [this]() { back(); });
    backButton = b;
    text(r, title, &arcade_roboto_medium_12, skin->fg);
}

// ---- frame --------------------------------------------------------------------

void ArcadeScreens::render(void)
{
    // Never rebuild inside an input event: the event may belong to an object
    // the rebuild deletes (Enter in a text box crashed the T-Deck). Rebuild on
    // the next tick instead, when LVGL has finished with the key.
    dirty = true;
}

void ArcadeScreens::rebuild(void)
{
    if (!screen)
        return;
    dirty = false;
    rebuiltAt = now ? now : 1;
    // a key held across the rebuild (the Enter that got us here) must not
    // also press whatever the new screen puts under focus
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if (lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED)
            lv_indev_wait_release(in);
    if (group)
        lv_group_remove_all_objs(group);
    lv_obj_clean(screen);
    actions.clear();
    order.clear();
    holdBar = nullptr;
    holding = false;
    backButton = nullptr;

    lv_obj_set_style_bg_color(screen, C(skin->bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    static bool gestureHooked = false;
    if (!gestureHooked) {
        lv_obj_add_event_cb(screen, onGesture, LV_EVENT_GESTURE, this);
        gestureHooked = true;
    }
    lv_obj_set_style_text_color(screen, C(skin->fg), 0);

    Scr s = current();
    bool chrome = s != Scr::Welcome && s != Scr::Words && s != Scr::Pin && s != Scr::PinAgain && s != Scr::Lock;
    if (chrome) {
        statusBar(screen);
        navBar(screen);
    }
    lv_obj_t *pane = box(screen);
    lv_obj_set_flex_grow(pane, 1);
    lv_obj_add_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pane, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(pane, 7, 0);
    lv_obj_set_style_pad_ver(pane, 6, 0);
    lv_obj_set_style_pad_gap(pane, 5, 0);
    lv_obj_add_flag(pane, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    switch (s) {
    case Scr::Welcome: scrWelcome(pane); break;
    case Scr::Words: scrWords(pane); break;
    case Scr::Pin: scrPin(pane, false); break;
    case Scr::PinAgain: scrPin(pane, true); break;
    case Scr::Lock: scrLock(pane); break;
    case Scr::Wallet: scrWallet(pane); break;
    case Scr::Receive: scrReceive(pane); break;
    case Scr::Send: scrSend(pane); break;
    case Scr::Contacts: scrContacts(pane); break;
    case Scr::Contact: scrContact(pane); break;
    case Scr::AddContact: scrAddContact(pane); break;
    case Scr::Messages: scrMessages(pane); break;
    case Scr::Mesh: scrMesh(pane); break;
    case Scr::Wifi: scrWifi(pane); break;
    case Scr::Settings: scrSettings(pane); break;
    case Scr::Chat: scrChat(pane); break;
    }
    lastSig = signature();
    touchOnlyControls(screen, pane);

    // Focus the first thing below the tabs, or what had it before a redraw.
    size_t first = chrome ? 5 : 0;
    if (first < order.size() && order[first] == backButton && first + 1 < order.size())
        first++; // start on the screen's first field, not its back arrow
    size_t want = focusAt ? focusAt : first;
    if (group && !order.empty())
        lv_group_focus_obj(order[want < order.size() ? want : order.size() - 1]);
}

void ArcadeScreens::statusBar(lv_obj_t *parent)
{
    lv_obj_t *bar = box(parent, true);
    lv_obj_set_height(bar, 16);
    lv_obj_set_style_bg_color(bar, C(skin->panel), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, C(skin->line), 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_gap(bar, 6, 0);

    text(bar, client.tag.empty() ? std::string("no @tag") : "@" + client.tag, &arcade_roboto_medium_12, skin->fg);
    lv_obj_t *sp = box(bar, true);
    lv_obj_set_flex_grow(sp, 1);

    using L = ArcadeClient::Link;
    uint32_t dot = client.link == L::Online ? skin->ok : skin->warn;
    std::string gw = client.link == L::Online ? client.gatewayName : client.link == L::SigningIn ? "Signing in" : "No gateway";
    lv_obj_t *d = lv_obj_create(bar);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 6, 6);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, C(dot), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_t *g = text(bar, gw, &arcade_roboto_10, skin->muted);
    lv_label_set_long_mode(g, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(g, 110, 0);
    if (battery >= 0) {
        const char *sym = battery > 80 ? LV_SYMBOL_BATTERY_FULL : battery > 55 ? LV_SYMBOL_BATTERY_3
                          : battery > 30 ? LV_SYMBOL_BATTERY_2 : battery > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        char b[24];
        snprintf(b, sizeof(b), "%d%% %s", battery > 100 ? 100 : battery, sym);
        text(bar, b, &arcade_roboto_10, battery > 15 ? skin->muted : skin->bad);
    }
    if (wallClock > 1600000000) {
        time_t t = wallClock;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char c[8];
        snprintf(c, sizeof(c), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        text(bar, c, &arcade_roboto_10, skin->muted);
    }
}

void ArcadeScreens::navBar(lv_obj_t *parent)
{
    lv_obj_t *bar = box(parent, true);
    lv_obj_set_height(bar, 24);
    lv_obj_set_style_bg_color(bar, C(skin->panel), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, C(skin->line), 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_gap(bar, 2, 0);

    lv_obj_t *logo = lv_obj_create(bar);
    lv_obj_remove_style_all(logo);
    lv_obj_set_size(logo, 15, 15);
    lv_obj_set_style_radius(logo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(logo, C(skin->accent), 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(logo, 3, 0);
    lv_obj_center(text(logo, std::string(1, skin->brand[0]), &arcade_roboto_medium_12, skin->accentFg));

    static const char *names[] = {"Wallet", "Contacts", "Messages", "Mesh", LV_SYMBOL_SETTINGS};
    static const Scr tabs[] = {Scr::Wallet, Scr::Contacts, Scr::Messages, Scr::Mesh, Scr::Settings};
    Scr root = stack.empty() ? Scr::Wallet : stack.front();
    for (int i = 0; i < 5; i++) {
        bool on = tabs[i] == root;
        lv_obj_t *t = lv_obj_create(bar);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(t, 7, 0);
        lv_obj_set_style_pad_ver(t, 2, 0);
        lv_obj_set_style_radius(t, 5, 0);
        lv_obj_set_style_bg_color(t, C(skin->accent), 0);
        lv_obj_set_style_bg_opa(t, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        text(t, names[i], &arcade_roboto_12, on ? skin->accentFg : skin->muted);
        if (tabs[i] == Scr::Messages && !client.unread.empty()) {
            lv_obj_t *dot = lv_obj_create(t);
            lv_obj_remove_style_all(dot);
            lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, C(skin->bad), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 5, -2);
        }
        Scr target = tabs[i];
        focusable(t, [this, target]() { show(target); });
    }
}

// ---- setup and lock -------------------------------------------------------------

void ArcadeScreens::scrWelcome(lv_obj_t *p)
{
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *logo = lv_obj_create(p);
    lv_obj_remove_style_all(logo);
    lv_obj_set_size(logo, 36, 36);
    lv_obj_set_style_radius(logo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(logo, C(skin->accent), 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_center(text(logo, std::string(1, skin->brand[0]), &arcade_roboto_medium_15, skin->accentFg));
    text(p, std::string(skin->brand) + "Arcade on LoRa", &arcade_roboto_medium_15, skin->fg);
    lv_obj_t *t = text(p, "Use your arcade account here: your wallet, contacts and sends, over the Meshtastic mesh. "
                          "Your twelve words stay on this T-Deck, locked with a PIN.",
                       &arcade_roboto_10, skin->muted);
    lv_obj_set_width(t, LV_PCT(92));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *r = box(p, true);
    lv_obj_set_width(r, LV_PCT(80));
    button(r, "Type my 12 words", [this]() {
        words.clear();
        word.clear();
        setupError.clear();
        go(Scr::Words);
    });
    lv_obj_t *n = text(p, "No account yet? Make one at app.dogecoinarcade.com first.", &arcade_roboto_10, skin->muted);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
}

void ArcadeScreens::scrWords(lv_obj_t *p)
{
    char head[40];
    snprintf(head, sizeof(head), "Word %u of 12", (unsigned)(words.size() < 12 ? words.size() + 1 : 12));
    header(p, head);
    std::string sofar;
    for (size_t i = 0; i < words.size(); i++)
        sofar += (i ? "  \xC2\xB7  " : "") + std::to_string(i + 1) + "\xC2\xA0" + words[i]; // no break inside "8 abandon"
    if (!sofar.empty()) {
        lv_obj_t *l = text(p, sofar, &arcade_mono_12, skin->muted);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    if (words.size() < 12) {
        auto accept = [this](const std::string &w) {
            words.push_back(w);
            word.clear();
            setupError.clear();
            if (words.size() == 12 && !phraseValid([&] {
                    std::string s;
                    for (auto &x : words)
                        s += x + " ";
                    return s;
                }()))
                setupError = "Those 12 words do not check out. A word is wrong or out of order.";
            render();
        };
        input(p, word, "type the word", "abcdefghijklmnopqrstuvwxyz", 8, false, [this, accept]() {
            auto m = wordsStartingWith(word, 2);
            if (m.size() == 1 || (!m.empty() && word == m[0]))
                accept(m[0]);
        });
        lv_obj_t *sug = box(p, true);
        lv_obj_set_flex_flow(sug, LV_FLEX_FLOW_ROW_WRAP);
        if (!word.empty())
            for (const char *w : wordsStartingWith(word, 4)) {
                std::string ws = w;
                pill(sug, ws, false, [accept, ws]() { accept(ws); });
            }
        lv_obj_t *hint = text(p, "Type the start of a word, then pick it or press Enter. Backspace on an empty box takes back the last word.",
                              &arcade_roboto_10, skin->muted);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        // backspace with nothing typed takes the last word back
        for (lv_obj_t *o : order)
            if (lv_obj_check_type(o, &lv_textarea_class))
                lv_obj_add_event_cb(
                    o,
                    [](lv_event_t *e) {
                        auto *self = (ArcadeScreens *)lv_event_get_user_data(e);
                        if (lv_event_get_key(e) == LV_KEY_BACKSPACE && self->word.empty() && !self->words.empty()) {
                            self->words.pop_back();
                            self->dirty = true;
                        }
                    },
                    LV_EVENT_KEY, this);
        focusAt = 1;
    } else {
        if (!setupError.empty()) {
            lv_obj_t *e = text(p, setupError, &arcade_roboto_12, skin->bad);
            lv_obj_set_width(e, LV_PCT(100));
            lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
            lv_obj_t *r = box(p, true);
            button(r, "Change word 12", [this]() {
                words.pop_back();
                render();
            }, false);
            button(r, "Start over", [this]() {
                words.clear();
                render();
            });
        } else {
            text(p, "All 12 words check out.", &arcade_roboto_12, skin->ok);
            lv_obj_t *r = box(p, true);
            button(r, "Choose a PIN", [this]() {
                pin.clear();
                pinAgain.clear();
                go(Scr::Pin);
            });
        }
    }
}

void ArcadeScreens::scrPin(lv_obj_t *p, bool again)
{
    header(p, again ? "Type the PIN again" : "Choose a PIN");
    lv_obj_t *t = text(p, again ? "The same six digits." : "Six digits. The PIN unlocks this T-Deck and keeps your words sealed on it.",
                       &arcade_roboto_10, skin->muted);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    std::string &field = again ? pinAgain : pin;
    input(p, field, "6 digits", "0123456789", 6, true, [this, again]() {
        std::string &f = again ? pinAgain : pin;
        if (f.size() != 6) {
            setupError = "The PIN is six digits.";
            return render();
        }
        setupError.clear();
        if (!again)
            return go(Scr::PinAgain);
        if (pinAgain != pin) {
            setupError = "Those PINs differ. Choose one again.";
            pin.clear();
            pinAgain.clear();
            stack.pop_back();
            return render();
        }
        working = true;
        setupError = "Sealing your words with the PIN" "\xE2\x80\xA6";
        workAfterDraw = [this]() {
            std::string phrase;
            for (auto &w : words)
                phrase += w + " ";
            bool ok = client.createAccount(phrase, pin);
            wipe(&phrase[0], phrase.size());
            for (auto &w : words)
                wipe(&w[0], w.size());
            words.clear();
            wipe(&pin[0], pin.size());
            wipe(&pinAgain[0], pinAgain.size());
            pin.clear();
            pinAgain.clear();
            setupError = ok ? "" : "This T-Deck could not save the account.";
            if (ok)
                show(Scr::Wallet);
            else
                render();
        };
        render();
    });
    if (!setupError.empty()) {
        lv_obj_t *e = text(p, setupError, &arcade_roboto_10, working ? skin->muted : skin->bad);
        lv_obj_set_width(e, LV_PCT(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
    }
    focusAt = 1;
}

void ArcadeScreens::scrLock(lv_obj_t *p)
{
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *logo = lv_obj_create(p);
    lv_obj_remove_style_all(logo);
    lv_obj_set_size(logo, 34, 34);
    lv_obj_set_style_radius(logo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(logo, C(skin->accent), 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_center(text(logo, std::string(1, skin->brand[0]), &arcade_roboto_medium_15, skin->accentFg));
    text(p, std::string(skin->brand) + "Arcade", &arcade_roboto_medium_15, skin->fg);
    if (lockedUntil && now < lockedUntil) {
        char b[60];
        snprintf(b, sizeof(b), "Too many tries. Wait %u s.", (unsigned)((lockedUntil - now) / 1000 + 1));
        text(p, b, &arcade_roboto_12, skin->bad);
        return;
    }
    text(p, "Enter your PIN", &arcade_roboto_10, skin->muted);
    lv_obj_t *row = box(p, true);
    lv_obj_set_width(row, 120);
    input(row, lockPin, "", "0123456789", 6, true, [this]() {
        if (lockPin.size() != 6)
            return;
        working = true;
        setupError = "Unlocking" "\xE2\x80\xA6";
        workAfterDraw = [this]() {
            bool ok = client.unlock(lockPin);
            wipe(&lockPin[0], lockPin.size());
            lockPin.clear();
            if (ok) {
                lockTries = 0;
                setupError.clear();
                show(Scr::Wallet);
            } else {
                lockTries++;
                setupError = "Wrong PIN.";
                if (lockTries >= 3)
                    lockedUntil = now + (30000u << (lockTries - 3 > 6 ? 6 : lockTries - 3));
                render();
            }
        };
        render();
    });
    if (!setupError.empty())
        text(p, setupError, &arcade_roboto_10, working ? skin->muted : skin->bad);
    text(p, "Your 12 words stay on this T-Deck, sealed with this PIN.", &arcade_roboto_10, skin->muted);
}

// ---- wallet -------------------------------------------------------------------

void ArcadeScreens::scrWallet(lv_obj_t *p)
{
    if (client.chains.empty()) {
        lv_obj_t *c = card(p);
        text(c, client.status, &arcade_roboto_12, skin->fg);
        text(c, "Balances show once a gateway answers, or after a WiFi sync.", &arcade_roboto_10, skin->muted);
    } else if (client.link != ArcadeClient::Link::Online) {
        std::string when = "Saved balances";
        if (client.balancesWhen > 1600000000) {
            time_t t = client.balancesWhen;
            struct tm tmv;
            localtime_r(&t, &tmv);
            char b[40];
            snprintf(b, sizeof(b), " from %02d/%02d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
            when += b;
        }
        text(p, when + (client.balancesFrom.empty() ? "" : " (" + client.balancesFrom + ")") + ". No gateway right now.",
             &arcade_roboto_10, skin->warn);
    }
    for (size_t i = 0; i < client.chains.size(); i++) {
        const Chain &ch = client.chains[i];
        lv_obj_t *c = card(p);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(c, 7, 0);
        lv_obj_t *coin = lv_obj_create(c);
        lv_obj_remove_style_all(coin);
        lv_obj_set_size(coin, 22, 22);
        lv_obj_set_style_radius(coin, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(coin, C(chainColor(ch)), 0);
        lv_obj_set_style_bg_opa(coin, LV_OPA_COVER, 0);
        lv_obj_center(text(coin, ch.symbol().substr(0, 1), &arcade_roboto_medium_12, 0xffffff));
        lv_obj_t *mid = box(c);
        lv_obj_set_flex_grow(mid, 1);
        lv_obj_set_width(mid, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(mid, 1, 0);
        lv_obj_t *top = box(mid, true);
        text(top, ch.coinName(), &arcade_roboto_medium_12, skin->fg);
        if (!ch.mainnet)
            chip(top, "testnet", skin->warn);
        uint64_t waiting = client.waitingOut(ch.network);
        if (waiting)
            text(mid, "\xE2\x88\x92" + formatAmount(waiting) + " waiting for a gateway", &arcade_roboto_10, skin->warn);
        else
            text(mid, ch.pending ? "+" + formatAmount(ch.pending) + " on its way" : std::string("confirmed"), &arcade_roboto_10,
                 skin->muted);
        lv_obj_t *right = box(c);
        lv_obj_set_width(right, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(right, 0, 0);
        text(right, formatAmount(client.available(i)), &arcade_mono_12, skin->fg);
        text(right, ch.symbol(), &arcade_roboto_10, skin->muted);
        size_t idx = i;
        focusable(c, [this, idx]() {
            sendChain = idx;
            client.cancelSend();
            go(Scr::Send);
        });
    }
    if (!client.payQueue.empty()) {
        lv_obj_t *s = text(p, "WAITING FOR A GATEWAY", &arcade_roboto_10, skin->warn);
        lv_obj_set_style_text_letter_space(s, 1, 0);
        lv_obj_t *c = card(p);
        lv_obj_set_style_pad_ver(c, 2, 0);
        for (size_t i = 0; i < client.payQueue.size(); i++) {
            const auto &q = client.payQueue[i];
            std::string sym = "PEP";
            for (auto &ch : client.chains)
                if (ch.network == q.network)
                    sym = ch.symbol();
            lv_obj_t *row = box(c, true);
            lv_obj_set_style_pad_ver(row, 3, 0);
            lv_obj_set_style_radius(row, 5, 0);
            lv_obj_t *who = text(row, "To " + q.to, &arcade_roboto_12, skin->fg);
            lv_obj_set_flex_grow(who, 1);
            lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
            if (q.when > 1600000000) {
                time_t t = q.when;
                struct tm tmv;
                localtime_r(&t, &tmv);
                char b[12];
                snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
                text(row, b, &arcade_roboto_10, skin->muted);
            }
            text(row, "\xE2\x88\x92" + formatAmount(q.amount) + " " + sym, &arcade_mono_12, skin->warn);
            focusable(row, [this]() {
                walletNote = "Hold a waiting payment to cancel it.";
                render();
            });
            actions.emplace_back(new Action([this, i]() {
                walletNote = client.cancelQueued(i) ? "Cancelled." : "That one is with the gateway now and cannot be taken back.";
                render();
            }));
            lv_obj_add_event_cb(row, onEvent, LV_EVENT_LONG_PRESSED, actions.back().get());
        }
        lv_obj_t *hint = text(p, walletNote.empty() ? std::string("They go as soon as a gateway answers. Hold one to cancel it.")
                                                    : walletNote,
                              &arcade_roboto_10, skin->muted);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    }
    if (client.send.state == ArcadeClient::SendState::Review) {
        lv_obj_t *rv = card(p);
        text(rv, "A payment is ready for you to review", &arcade_roboto_12, skin->fg);
        focusable(rv, [this]() { go(Scr::Send); });
    }
    if (!client.payQueueError.empty()) {
        lv_obj_t *e = text(p, "Not sent: " + client.payQueueError, &arcade_roboto_10, skin->bad);
        lv_obj_set_width(e, LV_PCT(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *r = box(p, true);
    button(r, "Send", [this]() {
        client.cancelSend();
        sendTo.clear();
        sendAmount.clear();
        go(Scr::Send);
    }, true, !client.chains.empty());
    button(r, "Receive", [this]() { go(Scr::Receive); }, false, !client.chains.empty());

    if (!client.activity.empty()) {
        lv_obj_t *s = text(p, "RECENT", &arcade_roboto_10, skin->muted);
        lv_obj_set_style_text_letter_space(s, 1, 0);
        lv_obj_t *c = card(p);
        lv_obj_set_style_pad_ver(c, 2, 0);
        size_t n = 0;
        for (auto &a : client.activity) {
            if (++n > 6)
                break;
            lv_obj_t *row = box(c, true);
            lv_obj_set_style_pad_ver(row, 2, 0);
            lv_obj_t *who = text(row, (a.amount > 0 ? "From " : "To ") + a.who, &arcade_roboto_12, skin->fg);
            lv_obj_set_flex_grow(who, 1);
            lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
            if (a.offline)
                chip(row, "offline", skin->warn);
            uint64_t v = a.amount < 0 ? (uint64_t)(-a.amount) : (uint64_t)a.amount;
            text(row, std::string(a.amount < 0 ? "\xE2\x88\x92" : "+") + formatAmount(v) + " " + a.symbol, &arcade_mono_12,
                 a.amount < 0 ? skin->fg : skin->ok);
        }
    }
}

void ArcadeScreens::scrReceive(lv_obj_t *p)
{
    header(p, "Receive");
    for (size_t i = 0; i < client.chains.size(); i++) {
        const Chain &ch = client.chains[i];
        lv_obj_t *c = card(p);
        lv_obj_t *top = box(c, true);
        text(top, ch.coinName(), &arcade_roboto_medium_12, skin->fg);
        if (!ch.mainnet)
            chip(top, "testnet", skin->warn);
        lv_obj_t *a = text(c, client.myAddress(i), &arcade_mono_12, skin->fg);
        lv_obj_set_width(a, LV_PCT(100));
        lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
    }
    if (!client.tag.empty()) {
        lv_obj_t *t = text(p, "Anyone can pay @" + client.tag + " from the arcade or another T-Deck.", &arcade_roboto_10,
                           skin->muted);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    }
}

void ArcadeScreens::scrSend(lv_obj_t *p)
{
    using S = ArcadeClient::SendState;
    auto &st = client.send;
    if (sendChain >= client.chains.size())
        sendChain = 0;
    if (client.chains.empty()) {
        header(p, "Send");
        text(p, "No gateway in range.", &arcade_roboto_12, skin->warn);
        return;
    }
    const Chain &ch = client.chains[st.state == S::Idle ? sendChain : st.chain];

    if (st.state == S::Idle) {
        header(p, "Send");
        text(p, "FROM", &arcade_roboto_10, skin->muted);
        lv_obj_t *wallets = box(p, true);
        lv_obj_set_flex_flow(wallets, LV_FLEX_FLOW_ROW_WRAP);
        for (size_t i = 0; i < client.chains.size(); i++) {
            const Chain &w = client.chains[i];
            std::string label = w.coinName() + (w.mainnet ? "" : " test") + " \xC2\xB7 " + formatAmount(client.available(i));
            pill(wallets, label, i == sendChain, [this, i]() {
                sendChain = i;
                render();
            });
        }
        text(p, "TO", &arcade_roboto_10, skin->muted);
        // suggestions only while typing, and only from the address book
        std::string q = sendTo;
        while (!q.empty() && q[0] == '@')
            q.erase(0, 1);
        for (auto &c : q)
            c = (char)tolower((unsigned char)c);
        std::vector<const Contact *> matches;
        bool exact = false;
        for (auto &k : client.contacts) {
            if (q.empty() || k.tag.compare(0, q.size(), q) != 0)
                continue;
            exact = exact || k.tag == q;
            if (matches.size() < 4)
                matches.push_back(&k);
        }
        auto pick = [this](const std::string &tag) {
            sendTo = "@" + tag;
            focusAmount = true;
            render();
        };
        const Contact *top = matches.empty() ? nullptr : matches[0];
        std::string topTag = top ? top->tag : "";
        lv_obj_t *toField = input(p, sendTo, "@tag or address", nullptr, 40, false,
                                  [this, topTag, exact, pick]() {
                                      if (!topTag.empty() && !exact)
                                          pick(topTag);
                                      else {
                                          focusAmount = true;
                                          render();
                                      }
                                  },
                                  [this]() { render(); });
        if (!exact)
            for (const Contact *k : matches) {
                lv_obj_t *row = box(p, true);
                lv_obj_set_style_pad_all(row, 3, 0);
                lv_obj_set_style_radius(row, 6, 0);
                lv_obj_set_style_bg_color(row, C(skin->panel), 0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                avatar(row, k->tag, 16);
                text(row, "@" + k->tag, &arcade_roboto_medium_12, skin->fg);
                if (!k->name.empty())
                    text(row, k->name, &arcade_roboto_10, skin->muted);
                std::string tg = k->tag;
                focusable(row, [pick, tg]() { pick(tg); });
            }
        char avail[64];
        snprintf(avail, sizeof(avail), "AMOUNT \xC2\xB7 %s %s AVAILABLE", formatAmount(client.available(sendChain)).c_str(),
                 ch.symbol().c_str());
        text(p, avail, &arcade_roboto_10, skin->muted);
        lv_obj_t *amountField = input(p, sendAmount, "0.00", "0123456789.", 18);
        lv_obj_t *r = box(p, true);
        button(r, client.link == ArcadeClient::Link::Online ? "Review" : "Queue for a gateway", [this]() {
            uint64_t a = 0;
            if (sendTo.empty() || !parseAmount(sendAmount, a))
                return;
            client.askToSend(sendChain, sendTo, a);
        });
        // the caret stays where the typing is: in To, or in Amount once a name is picked
        lv_obj_t *want = focusAmount ? amountField : toField;
        for (size_t i = 0; i < order.size(); i++)
            if (order[i] == want)
                focusAt = i;
        focusAmount = false;
        return;
    }
    if (st.state == S::Asking || st.state == S::Signing) {
        header(p, st.state == S::Asking ? "Asking the gateway" : "Sending");
        lv_obj_t *c = card(p);
        text(c, st.state == S::Asking ? "The gateway is building the payment. This T-Deck will check it before you sign."
                                      : "Signed here. The gateway is broadcasting it.",
             &arcade_roboto_12, skin->fg);
        lv_obj_t *s = lv_spinner_create(c);
        lv_obj_set_size(s, 22, 22);
        lv_obj_set_style_arc_color(s, C(skin->accent), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s, C(skin->line), 0);
        lv_obj_set_style_arc_width(s, 3, 0);
        lv_obj_set_style_arc_width(s, 3, LV_PART_INDICATOR);
        lv_obj_t *r = box(p, true);
        button(r, "Cancel", [this]() {
            client.cancelSend();
            back();
        }, false);
        return;
    }
    if (st.state == S::Review) {
        header(p, "Review");
        lv_obj_t *c = card(p);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *who = box(c, true);
        lv_obj_set_width(who, LV_SIZE_CONTENT);
        text(who, "Pay", &arcade_roboto_12, skin->muted);
        if (!st.toTag.empty())
            avatar(who, st.toTag, 15);
        text(who, st.toTag.empty() ? shortAddress(st.toAddress) : "@" + st.toTag, &arcade_roboto_medium_12, skin->fg);
        lv_obj_t *big = box(c, true);
        lv_obj_set_width(big, LV_SIZE_CONTENT);
        text(big, formatAmount(st.amount), &arcade_mono_22, skin->fg);
        text(big, ch.symbol(), &arcade_roboto_12, skin->muted);
        lv_obj_set_style_pad_gap(c, 1, 0);
        lv_obj_t *kvs = box(c);
        lv_obj_set_style_pad_gap(kvs, 0, 0);
        kv(kvs, "Address", shortAddress(st.toAddress));
        kv(kvs, "Fee", formatAmount(st.fee) + " " + ch.symbol());
        kv(kvs, "Change", formatAmount(st.change) + " " + ch.symbol());
        lv_obj_t *okrow = box(p, true);
        text(okrow, LV_SYMBOL_OK " Checked on this T-Deck", &arcade_roboto_10, skin->ok);
        lv_obj_t *r = box(p, true);
        button(r, "Back", [this]() {
            client.cancelSend();
            render();
        }, false);
        lv_obj_t *hold = button(r, "Hold " LV_SYMBOL_OK " to sign", nullptr);
        lv_obj_t *fill = lv_obj_create(hold);
        lv_obj_remove_style_all(fill);
        lv_obj_add_flag(fill, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(fill, 0, LV_PCT(100));
        lv_obj_align(fill, LV_ALIGN_LEFT_MID, -8, 0);
        lv_obj_set_style_bg_color(fill, C(skin->accentFg), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_30, 0);
        lv_obj_move_background(fill);
        holdBar = fill;
        lv_obj_add_event_cb(hold, onHold, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(hold, onHold, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(hold, onHold, LV_EVENT_PRESS_LOST, this);
        focusAt = order.size() - 1;
        return;
    }
    if (st.state == S::Queued) {
        lv_obj_t *c = card(p);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(c, 12, 0);
        chip(c, "waiting for a gateway", skin->warn);
        text(c, formatAmount(st.amount) + " " + ch.symbol() + " to " + st.to, &arcade_roboto_medium_12, skin->fg);
        lv_obj_t *w = text(c, "Saved on this T-Deck. It goes as soon as a gateway answers. A payee in your address book "
                              "is paid then without asking again; anyone else waits for you at Review.",
                           &arcade_roboto_10, skin->muted);
        lv_obj_set_width(w, LV_PCT(100));
        lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *r = box(p, true);
        button(r, "Done", [this]() {
            client.cancelSend();
            sendTo.clear();
            sendAmount.clear();
            show(Scr::Wallet);
        });
        return;
    }
    // Sent or Failed
    lv_obj_t *c = card(p);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(c, 12, 0);
    if (st.state == S::Sent) {
        chip(c, "sent", skin->ok);
        text(c, formatAmount(st.amount) + " " + ch.symbol() + " to " + (st.toTag.empty() ? shortAddress(st.toAddress) : "@" + st.toTag),
             &arcade_roboto_medium_12, skin->fg);
        text(c, "txid " + st.txid.substr(0, 8) + "\xE2\x80\xA6" + st.txid.substr(st.txid.size() > 8 ? st.txid.size() - 8 : 0),
             &arcade_mono_12, skin->muted);
    } else {
        chip(c, "not sent", skin->bad);
        lv_obj_t *w = text(c, st.why, &arcade_roboto_12, skin->fg);
        lv_obj_set_width(w, LV_PCT(100));
        lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *r = box(p, true);
    button(r, "Done", [this]() {
        bool sent = client.send.state == S::Sent;
        client.cancelSend();
        if (sent) {
            sendTo.clear();
            sendAmount.clear();
            show(Scr::Wallet);
        } else {
            render();
        }
    });
}

// ---- contacts -----------------------------------------------------------------

void ArcadeScreens::scrContacts(lv_obj_t *p)
{
    if (client.contacts.empty()) {
        lv_obj_t *t = text(p, "No contacts on this T-Deck yet. Add someone by their @tag.", &arcade_roboto_12, skin->muted);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    }
    for (auto &k : client.contacts) {
        lv_obj_t *row = box(p, true);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_radius(row, 7, 0);
        avatar(row, k.tag, 22);
        lv_obj_t *n = box(row);
        lv_obj_set_flex_grow(n, 1);
        lv_obj_set_width(n, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(n, 0, 0);
        text(n, "@" + k.tag, &arcade_roboto_medium_12, skin->fg);
        text(n, (k.name.empty() ? std::string("") : k.name + " \xC2\xB7 ") + shortAddress(k.address), &arcade_roboto_10,
             skin->muted);
        std::string t = k.tag;
        focusable(row, [this, t]() {
            contactTag = t;
            go(Scr::Contact);
        });
    }
    lv_obj_t *r = box(p, true);
    button(r, LV_SYMBOL_PLUS " Add @tag", [this]() {
        addTag.clear();
        client.who.state = ArcadeClient::WhoState::Idle;
        go(Scr::AddContact);
    }, false);
    button(r, LV_SYMBOL_WIFI " Sync over WiFi", [this]() {
        if (!client.loadWifi(wifiSsid, wifiPass))
            wifiSsid.clear(), wifiPass.clear();
        client.scanWifi();
        go(Scr::Wifi);
    }, false);
}

void ArcadeScreens::scrWifi(lv_obj_t *p)
{
    using W = ArcadeClient::WifiState;
    header(p, "Sync over WiFi");
    lv_obj_t *t = text(p, "Joins your WiFi for a moment, signs in to " + client.arcadeHost() +
                              " and brings your arcade address book onto this T-Deck. Your WiFi password is sealed with your account.",
                       &arcade_roboto_10, skin->muted);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_t *nh = box(p, true);
    lv_obj_t *nt = text(nh, client.wifiScanning ? "LOOKING FOR NETWORKS" "\xE2\x80\xA6" : "NETWORKS NEARBY", &arcade_roboto_10,
                        skin->muted);
    lv_obj_set_flex_grow(nt, 1);
    lv_obj_t *scan = pill(nh, LV_SYMBOL_REFRESH " Scan", false, [this]() { client.scanWifi(); });
    (void)scan;
    if (!client.wifiScanning && client.wifiNets.empty()) {
        text(p, "Press Scan to see the networks around you.", &arcade_roboto_10, skin->muted);
    }
    size_t shown = 0;
    for (auto &n : client.wifiNets) {
        if (++shown > 8)
            break;
        lv_obj_t *row = box(p, true);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_radius(row, 5, 0);
        bool picked = n.ssid == wifiSsid;
        if (picked) {
            lv_obj_set_style_bg_color(row, C(skin->accent), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        }
        int bars = n.rssi > -55 ? 4 : n.rssi > -67 ? 3 : n.rssi > -78 ? 2 : 1;
        lv_obj_t *sig = box(row, true);
        lv_obj_set_width(sig, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(sig, 1, 0);
        lv_obj_set_flex_align(sig, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        for (int b = 0; b < 4; b++) {
            lv_obj_t *bar = lv_obj_create(sig);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 3, 4 + b * 3);
            lv_obj_set_style_bg_color(bar, C(b < bars ? skin->fg : skin->line), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        }
        lv_obj_t *name = text(row, n.ssid, &arcade_roboto_12, skin->fg);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (n.secure)
            text(row, "\xEF\x80\xA3", &arcade_roboto_10, skin->muted); // lock
        std::string ssid = n.ssid;
        bool open = !n.secure;
        focusable(row, [this, ssid, open]() {
            if (wifiSsid != ssid)
                wifiPass.clear();
            wifiSsid = ssid;
            wifiFocusPassword = !open;
            render();
        });
    }
    text(p, "WIFI NETWORK", &arcade_roboto_10, skin->muted);
    input(p, wifiSsid, "network name", nullptr, 32);
    text(p, "PASSWORD", &arcade_roboto_10, skin->muted);
    lv_obj_t *pw = input(p, wifiPass, "password", nullptr, 63, true);
    if (wifiFocusPassword) {
        wifiFocusPassword = false;
        for (size_t i = 0; i < order.size(); i++)
            if (order[i] == pw)
                focusAt = i;
    }
    auto &w = client.wifi;
    bool busy = w.state == W::Joining || w.state == W::SigningIn || w.state == W::Downloading;
    if (w.state != W::Idle) {
        lv_obj_t *c = card(p);
        std::string line;
        uint32_t color = skin->fg;
        switch (w.state) {
        case W::Joining: line = "Joining " + w.ssid + "\xE2\x80\xA6"; break;
        case W::SigningIn: line = "Signing in to the arcade\xE2\x80\xA6"; break;
        case W::Downloading: line = "Bringing the address book\xE2\x80\xA6"; break;
        case W::Done:
            line = std::to_string(w.imported) + " contacts from the arcade" +
                   (w.removed ? " (" + std::to_string(w.removed) + " you deleted stay off)" : "");
            color = skin->ok;
            break;
        case W::Failed: line = w.why; color = skin->bad; break;
        default: break;
        }
        lv_obj_t *l = text(c, line, &arcade_roboto_12, color);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *r = box(p, true);
    button(r, busy ? "Syncing" "\xE2\x80\xA6" : "Save and sync", [this]() {
        if (wifiSsid.empty())
            return;
        client.saveWifi(wifiSsid, wifiPass);
        client.syncOverWifi();
    }, true, !busy);
}

void ArcadeScreens::scrContact(lv_obj_t *p)
{
    const Contact *k = nullptr;
    for (auto &c : client.contacts)
        if (c.tag == contactTag)
            k = &c;
    header(p, "@" + contactTag);
    if (!k) {
        text(p, "Not in your contacts any more.", &arcade_roboto_12, skin->muted);
        return;
    }
    lv_obj_t *c = card(p);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(c, 8, 0);
    avatar(c, k->tag, 26);
    lv_obj_t *n = box(c);
    lv_obj_set_flex_grow(n, 1);
    lv_obj_set_width(n, LV_SIZE_CONTENT);
    text(n, k->name.empty() ? "@" + k->tag : k->name, &arcade_roboto_medium_12, skin->fg);
    text(n, "Found through a gateway", &arcade_roboto_10, skin->muted);
    lv_obj_t *d = card(p);
    kv(d, "Address", shortAddress(k->address));
    kv(d, "Mail key", toHex(k->key, 6));
    lv_obj_t *r = box(p, true);
    std::string t = k->tag;
    button(r, "Pay", [this, t]() {
        client.cancelSend();
        sendTo = "@" + t;
        sendAmount.clear();
        go(Scr::Send);
    });
    button(r, "Remove", [this, t]() {
        client.removeContact(t);
        back();
    }, false);
}

void ArcadeScreens::scrAddContact(lv_obj_t *p)
{
    using W = ArcadeClient::WhoState;
    header(p, "Add contact");
    text(p, "@TAG", &arcade_roboto_10, skin->muted);
    input(p, addTag, "@robin", "@abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 25, false, [this]() {
        if (!addTag.empty())
            client.addContact(addTag);
    });
    auto &w = client.who;
    if (w.state == W::Asking) {
        text(p, "Asking the gateway for @" + w.tag + "\xE2\x80\xA6", &arcade_roboto_12, skin->muted);
    } else if (w.state == W::Found) {
        lv_obj_t *c = card(p);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        avatar(c, w.tag, 22);
        text(c, "@" + w.tag + " added", &arcade_roboto_medium_12, skin->ok);
    } else if (w.state == W::Failed) {
        lv_obj_t *e = text(p, w.why, &arcade_roboto_12, skin->bad);
        lv_obj_set_width(e, LV_PCT(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *r = box(p, true);
    button(r, w.state == W::Found ? "Done" : "Look up", [this]() {
        if (client.who.state == ArcadeClient::WhoState::Found) {
            client.who.state = ArcadeClient::WhoState::Idle;
            back();
        } else if (!addTag.empty()) {
            client.addContact(addTag);
        }
    }, true, w.state != W::Asking);
    focusAt = w.state == W::Found ? order.size() - 1 : 1;
}

// ---- messages and mesh --------------------------------------------------------------

void ArcadeScreens::scrMessages(lv_obj_t *p)
{
    // every contact can be written to; the newest conversations come first
    std::vector<std::string> tags = client.threadTags();
    if (tags.empty()) {
        lv_obj_t *t = text(p, "Add contacts (or sync your address book over WiFi) to write to them.", &arcade_roboto_12,
                           skin->muted);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    }
    for (auto &tag : tags) {
        auto &t = client.thread(tag);
        lv_obj_t *row = box(p, true);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_radius(row, 7, 0);
        avatar(row, tag, 22);
        lv_obj_t *mid = box(row);
        lv_obj_set_flex_grow(mid, 1);
        lv_obj_set_width(mid, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(mid, 0, 0);
        text(mid, "@" + tag, &arcade_roboto_medium_12, skin->fg);
        std::string last = t.empty() ? std::string("No messages yet") : (t.back().mine ? "You: " : "") + t.back().text;
        lv_obj_t *l = text(mid, last, &arcade_roboto_10, skin->muted);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        auto u = client.unread.find(tag);
        if (u != client.unread.end()) {
            lv_obj_t *b = lv_obj_create(row);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, LV_SIZE_CONTENT, 14);
            lv_obj_set_style_min_width(b, 14, 0);
            lv_obj_set_style_pad_hor(b, 4, 0);
            lv_obj_set_style_radius(b, 7, 0);
            lv_obj_set_style_bg_color(b, C(skin->accent), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_center(text(b, std::to_string(u->second), &arcade_roboto_10, skin->accentFg));
        }
        std::string tg = tag;
        focusable(row, [this, tg]() {
            chatTag = tg;
            chatError.clear();
            draft.clear();
            client.markRead(tg);
            go(Scr::Chat);
        });
    }
}

void ArcadeScreens::scrChat(lv_obj_t *p)
{
    header(p, "@" + chatTag);
    client.markRead(chatTag);
    auto &t = client.thread(chatTag);
    size_t from = t.size() > 20 ? t.size() - 20 : 0;
    if (t.empty()) {
        lv_obj_t *e = text(p, "Say hello. Messages go through the gateway on the PEP testnet chain and wait there for @" +
                                  chatTag + " if their T-Deck is off.",
                           &arcade_roboto_10, skin->muted);
        lv_obj_set_width(e, LV_PCT(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
    }
    for (size_t i = from; i < t.size(); i++) {
        const Letter &l = t[i];
        lv_obj_t *row = box(p);
        lv_obj_set_style_pad_gap(row, 1, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, l.mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              l.mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
        lv_obj_t *b = lv_obj_create(row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(b, LV_PCT(80), 0);
        lv_obj_set_style_pad_hor(b, 7, 0);
        lv_obj_set_style_pad_ver(b, 4, 0);
        lv_obj_set_style_radius(b, 9, 0);
        bool tooLong = l.state == Letter::TooLong;
        if (l.mine) {
            lv_obj_set_style_bg_color(b, C(skin->sent), 0);
            lv_obj_set_style_bg_opa(b, l.state == Letter::Failed ? LV_OPA_40 : LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(b, C(skin->panel), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, C(tooLong ? skin->warn : skin->line), 0);
        }
        lv_obj_t *msg = text(b, l.text, &arcade_roboto_12, l.mine ? 0xffffff : skin->fg);
        lv_obj_set_style_max_width(msg, 220, 0);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        std::string meta;
        if (l.when > 1600000000) {
            time_t tt = l.when;
            struct tm tmv;
            localtime_r(&tt, &tmv);
            char c[16];
            snprintf(c, sizeof(c), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            meta = c;
        }
        if (l.mine)
            meta += std::string(meta.empty() ? "" : " \xC2\xB7 ") +
                    (l.state == Letter::Sending
                         ? (client.link == ArcadeClient::Link::Online ? "sending\xE2\x80\xA6" : "waiting for a gateway")
                         : l.state == Letter::Sent ? "sent" : "not sent: " + l.why);
        if (l.mine && l.state == Letter::Failed)
            meta += " \xC2\xB7 hold to send again";
        if (!meta.empty()) {
            lv_obj_t *m = text(row, meta, &arcade_roboto_10, l.state == Letter::Failed ? skin->bad : skin->muted);
            lv_obj_set_style_max_width(m, 280, 0);
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        }
        if (l.mine && l.state == Letter::Failed) {
            // long-press (trackball held, or Enter held) sends it again
            size_t index = i;
            focusable(b, [this]() {
                chatError = "Hold the trackball on a message that was not sent to send it again.";
                render();
            });
            actions.emplace_back(new Action([this, index]() {
                std::string why;
                if (client.resendMessage(chatTag, index, why))
                    chatError.clear();
                else
                    chatError = why;
                render();
            }));
            lv_obj_add_event_cb(b, onEvent, LV_EVENT_LONG_PRESSED, actions.back().get());
        }
    }
    if (!chatError.empty()) {
        lv_obj_t *e = text(p, chatError, &arcade_roboto_10, skin->bad);
        lv_obj_set_width(e, LV_PCT(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
    }
    auto sendIt = [this]() {
        std::string why;
        if (client.sendMessage(chatTag, draft, why)) {
            draft.clear();
            chatError.clear();
        } else {
            chatError = why;
        }
        render();
    };
    lv_obj_t *ta = input(p, draft, ("Message @" + chatTag).c_str(), nullptr, ArcadeClient::MESSAGE_MAX, false, sendIt);
    size_t left = ArcadeClient::MESSAGE_MAX - (draft.size() > ArcadeClient::MESSAGE_MAX ? ArcadeClient::MESSAGE_MAX : draft.size());
    std::string how = client.link == ArcadeClient::Link::Online ? "on chain via " + client.gatewayName
                                                                 : "waits for a gateway";
    text(p, std::to_string(left) + " characters left \xC2\xB7 " + how, &arcade_roboto_10, skin->muted);
    lv_obj_t *r = box(p, true);
    button(r, "Send", sendIt);
    for (size_t i = 0; i < order.size(); i++)
        if (order[i] == ta)
            focusAt = i;
}

void ArcadeScreens::scrMesh(lv_obj_t *p)
{
    using L = ArcadeClient::Link;
    lv_obj_t *c = card(p);
    lv_obj_t *top = box(c, true);
    if (client.link == L::Online) {
        text(top, client.gatewayName, &arcade_roboto_medium_12, skin->fg);
        chip(top, client.tag.empty() ? "signed in" : "online", skin->ok);
    } else {
        text(top, client.link == L::SigningIn ? "Signing in" : "No gateway in range", &arcade_roboto_medium_12, skin->fg);
        chip(top, "mesh only", skin->warn);
    }
    char node[16];
    snprintf(node, sizeof(node), "!%08x", (unsigned)client.gateway);
    if (client.gateway)
        kv(c, "Gateway", node);
    if (!client.gatewayHost.empty())
        kv(c, "Arcade", client.gatewayHost, false);
    lv_obj_t *st = text(c, client.status, &arcade_roboto_10, skin->muted);
    lv_obj_set_width(st, LV_PCT(100));
    lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);

    lv_obj_t *me = card(p);
    snprintf(node, sizeof(node), "!%08x", (unsigned)myNode);
    kv(me, "This T-Deck", node);
    kv(me, "Contacts", std::to_string(client.contacts.size()));

    lv_obj_t *r = box(p, true);
    button(r, LV_SYMBOL_REFRESH " Find gateway", [this]() {
        client.link = ArcadeClient::Link::Searching;
        client.findGateway();
        render();
    }, false);
}

} // namespace arcade

namespace arcade
{

void ArcadeScreens::setRadioState(const RadioState &s)
{
    radioState = s;
    if (current() == Scr::Settings && signature() != lastSig)
        dirty = true;
}

lv_obj_t *ArcadeScreens::section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *s = text(parent, title, &arcade_roboto_10, skin->muted);
    lv_obj_set_style_text_letter_space(s, 1, 0);
    lv_obj_set_style_margin_top(s, 3, 0);
    return card(parent);
}

lv_obj_t *ArcadeScreens::toggle(lv_obj_t *parent, const std::string &label, bool on, std::function<void(bool)> set)
{
    lv_obj_t *row = box(parent, true);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_radius(row, 5, 0);
    lv_obj_t *l = text(row, label, &arcade_roboto_12, skin->fg);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *sw = lv_obj_create(row);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 30, 16);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sw, C(on ? skin->accent : skin->line), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_t *knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, 12, 12);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, C(on ? skin->accentFg : skin->muted), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
    focusable(row, [set, on]() { set(!on); });
    return row;
}

void ArcadeScreens::scrSettings(lv_obj_t *p)
{
    const RadioState &r = radioState;

    lv_obj_t *acct = section(p, "ACCOUNT");
    kv(acct, "Signed in as", client.tag.empty() ? std::string("no @tag yet") : "@" + client.tag, false);
    if (!r.longName.empty())
        kv(acct, "Name on the mesh", r.longName, false);
    lv_obj_t *ar = box(acct, true);
    button(ar, "\xEF\x80\xA3 Lock now", [this]() {
        client.lock();
        show(Scr::Lock);
    }, false);

    lv_obj_t *wifi = section(p, "WIFI");
    toggle(wifi, "WiFi", client.wifiOn, [this](bool on) {
        client.setWifiOn(on);
        render();
    });
    if (!client.wifiOn) {
        text(wifi, "Off: this T-Deck never turns its WiFi on.", &arcade_roboto_10, skin->muted);
    } else {
    std::string ssid, pass;
    bool saved = client.loadWifi(ssid, pass);
    wipe(&pass[0], pass.size());
    kv(wifi, "Network", saved ? ssid : std::string("none saved"), false);
    std::string last;
    using W = ArcadeClient::WifiState;
    if (client.wifi.state == W::Done)
        last = std::to_string(client.wifi.imported) + " contacts, balances and coins";
    else if (client.wifi.state == W::Failed)
        last = client.wifi.why;
    if (!last.empty()) {
        lv_obj_t *l = text(wifi, "Last sync: " + last, &arcade_roboto_10, client.wifi.state == W::Done ? skin->ok : skin->bad);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *wr = box(wifi, true);
    button(wr, saved ? LV_SYMBOL_REFRESH " Sync now" : LV_SYMBOL_WIFI " Set up WiFi", [this, saved]() {
        if (saved) {
            client.syncOverWifi();
            render();
        } else {
            wifiSsid.clear();
            wifiPass.clear();
            client.scanWifi();
            go(Scr::Wifi);
        }
    });
    if (saved)
        button(wr, "Change", [this]() {
            if (!client.loadWifi(wifiSsid, wifiPass))
                wifiSsid.clear(), wifiPass.clear();
            client.scanWifi();
            go(Scr::Wifi);
        }, false);

    }

    lv_obj_t *bt = section(p, "BLUETOOTH");
    toggle(bt, "Bluetooth", r.btEnabled, [this](bool on) {
        if (radio)
            radio->setBluetooth(on);
    });
    if (r.btEnabled) {
        char pin[16];
        snprintf(pin, sizeof(pin), "%03u %03u", (unsigned)(r.btPin / 1000), (unsigned)(r.btPin % 1000));
        kv(bt, "Pairing PIN", r.btPin ? pin : "shown when a phone pairs");
        lv_obj_t *st = box(bt, true);
        text(st, r.btConnected ? LV_SYMBOL_BLUETOOTH " A phone is connected" : LV_SYMBOL_BLUETOOTH " No phone connected",
             &arcade_roboto_10, r.btConnected ? skin->ok : skin->muted);
        lv_obj_t *n = text(bt, "Pair the DogecoinArcade app on an Android phone (Chrome) to use it as this T-Deck's gateway, or the Meshtastic app for radio settings.",
                           &arcade_roboto_10, skin->muted);
        lv_obj_set_width(n, LV_PCT(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    }

    lv_obj_t *gps = section(p, "GPS");
    if (r.gpsMode == 2) {
        text(gps, "This T-Deck has no GPS.", &arcade_roboto_12, skin->muted);
    } else {
        toggle(gps, "GPS", r.gpsMode == 1, [this](bool on) {
            if (radio)
                radio->setGps(on);
        });
        if (r.gpsMode == 1) {
            char b[64];
            if (r.fix)
                snprintf(b, sizeof(b), "Fixed Â· %u satellites", (unsigned)r.sats);
            else
                snprintf(b, sizeof(b), "Looking for satellites (%u so far)", (unsigned)r.sats);
            text(gps, b, &arcade_roboto_10, r.fix ? skin->ok : skin->muted);
            if (r.fix) {
                snprintf(b, sizeof(b), "%.5f, %.5f Â· %d m", r.lat, r.lon, (int)r.alt);
                kv(gps, "Position", b);
            }
        }
    }

    lv_obj_t *pw = section(p, "POWER");
    toggle(pw, "Power saving", r.powerSaving, [this](bool on) {
        if (radio)
            radio->setPowerSaving(on);
    });
    lv_obj_t *pn = text(pw, r.powerSaving ? "Saves battery: the T-Deck sleeps when the screen is off and leaves USB."
                                          : "Stays awake: USB and the mesh keep working with the screen off.",
                        &arcade_roboto_10, skin->muted);
    lv_obj_set_width(pn, LV_PCT(100));
    lv_label_set_long_mode(pn, LV_LABEL_LONG_WRAP);
    text(pw, "Screen off after", &arcade_roboto_12, skin->fg);
    lv_obj_t *to = box(pw, true);
    static const uint32_t secs[] = {30, 60, 300, 0};
    static const char *names[] = {"30 s", "1 min", "5 min", "never"};
    for (int i = 0; i < 4; i++) {
        uint32_t v = secs[i];
        pill(to, names[i], r.screenTimeout == v, [this, v]() {
            if (radio)
                radio->setScreenTimeout(v);
        });
    }

    lv_obj_t *sk = section(p, "SKIN");
    lv_obj_set_flex_flow(sk, LV_FLEX_FLOW_ROW_WRAP);
    for (size_t i = 0; i < 6; i++) {
        std::string name = std::string(SKINS[i].brand) + (SKINS[i].dark ? " dark" : " light");
        pill(sk, name, i == skinIndex, [this, i]() { setSkin(i); });
    }

    lv_obj_t *about = section(p, "ABOUT");
    char node[16];
    snprintf(node, sizeof(node), "!%08x", (unsigned)myNode);
    kv(about, "This T-Deck", node);
    if (!r.firmware.empty())
        kv(about, "Meshtastic", r.firmware);
    kv(about, "Contacts", std::to_string(client.contacts.size()));
}

} // namespace arcade
