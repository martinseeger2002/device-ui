#include "graphics/view/TFT/ArcadeView.h"
#include "arcade/ArcadeScreens.h"
#include "arcade/Snapshot.h"
#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/driver/DisplayDriverFactory.h"
#include "input/InputDriver.h"
#include "util/ILog.h"
#include <algorithm>
#include <time.h>

#if defined(ARCH_PORTDUINO)
#include <fstream>
#include <sys/stat.h>
#else
#include "arcade/RootCAs.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <freertos/semphr.h>
#endif

using namespace arcade;

namespace
{
// The arcade's few files: in /arcade on the radio's own flash.
class FlashStorage : public Storage
{
  public:
#if defined(ARCH_PORTDUINO)
    FlashStorage()
    {
        const char *home = getenv("HOME");
        dir = std::string(home ? home : ".") + "/.portduino/arcade";
        mkdir(dir.c_str(), 0700);
    }
    bool load(const std::string &n, Bytes &out) override
    {
        std::ifstream f(dir + "/" + n, std::ios::binary);
        if (!f)
            return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    bool save(const std::string &n, const Bytes &d) override
    {
        std::ofstream f(dir + "/" + n, std::ios::binary | std::ios::trunc);
        f.write((const char *)d.data(), d.size());
        return (bool)f;
    }
    void remove(const std::string &n) override { ::remove((dir + "/" + n).c_str()); }
    std::string dir;
#else
    FlashStorage()
    {
        if (!LittleFS.exists("/arcade"))
            LittleFS.mkdir("/arcade");
    }
    bool load(const std::string &n, Bytes &out) override
    {
        File f = LittleFS.open(("/arcade/" + n).c_str(), "r");
        if (!f)
            return false;
        out.resize(f.size());
        size_t got = f.read(out.data(), out.size());
        f.close();
        return got == out.size();
    }
    bool save(const std::string &n, const Bytes &d) override
    {
        // write aside, then rename: a power cut never leaves half a key file
        std::string path = "/arcade/" + n, tmp = path + ".new";
        File f = LittleFS.open(tmp.c_str(), "w");
        if (!f)
            return false;
        size_t put = f.write(d.data(), d.size());
        f.close();
        if (put != d.size())
            return false;
        LittleFS.remove(path.c_str());
        return LittleFS.rename(tmp.c_str(), path.c_str());
    }
    void remove(const std::string &n) override { LittleFS.remove(("/arcade/" + n).c_str()); }
#endif
};
} // namespace

#if !defined(ARCH_PORTDUINO)
// ---- WiFi sync: join, sign in over HTTPS, fetch the mailbox, leave -------------
//
// Runs as its own FreeRTOS task so the screen keeps drawing; it only hands
// results back (progress and, at the end, the sealed mailbox) through a
// mutex-guarded slot that the UI task reads.
namespace
{
struct WifiJob {
    std::string ssid, pass, host;
    ArcadeClient *client;
    std::shared_ptr<const arcade::Account> keys; // held for the whole sync, even if the T-Deck is locked meanwhile
    SemaphoreHandle_t lock;
    volatile bool posted = false;
    volatile bool running = false;
    ArcadeClient::WifiState state = ArcadeClient::WifiState::Idle;
    std::string why, blob, account;
    std::vector<std::pair<std::string, std::string>> coins; // network, JSON
};
WifiJob *wifiJob = nullptr;

// ---- WiFi scan: what networks are around, strongest first ----
struct ScanJob {
    SemaphoreHandle_t lock;
    volatile bool running = false, posted = false;
    std::vector<ArcadeClient::WifiNet> nets;
};
ScanJob *scanJob = nullptr;

void scanTask(void *arg)
{
    ScanJob *s = (ScanJob *)arg;
    std::vector<ArcadeClient::WifiNet> found;
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, false);
    for (int i = 0; i < n; i++) {
        std::string ssid = WiFi.SSID(i).c_str();
        if (ssid.empty())
            continue; // hidden networks have no name to show
        int rssi = WiFi.RSSI(i);
        bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        bool dup = false;
        for (auto &f : found)
            if (f.ssid == ssid) {
                dup = true;
                if (rssi > f.rssi)
                    f.rssi = rssi;
            }
        if (!dup)
            found.push_back({ssid, rssi, secure});
    }
    WiFi.scanDelete();
    if (!(wifiJob && wifiJob->running))
        WiFi.mode(WIFI_OFF);
    std::sort(found.begin(), found.end(), [](const ArcadeClient::WifiNet &a, const ArcadeClient::WifiNet &b) {
        return a.rssi > b.rssi;
    });
    xSemaphoreTake(s->lock, portMAX_DELAY);
    s->nets.swap(found);
    s->posted = true;
    xSemaphoreGive(s->lock);
    s->running = false;
    vTaskDelete(NULL);
}

void post(WifiJob *j, ArcadeClient::WifiState s, const std::string &why = "", std::string *blob = nullptr,
          std::string *account = nullptr, std::vector<std::pair<std::string, std::string>> *coins = nullptr)
{
    xSemaphoreTake(j->lock, portMAX_DELAY);
    j->state = s;
    j->why = why;
    if (blob)
        j->blob.swap(*blob);
    if (account)
        j->account.swap(*account);
    if (coins)
        j->coins.swap(*coins);
    j->posted = true;
    xSemaphoreGive(j->lock);
}

std::string jsonString(const String &body, const char *key)
{
    // the few flat answers read here ({"nonce": "...", ...}) need no parser
    String k = String("\"") + key + "\"";
    int at = body.indexOf(k);
    if (at < 0)
        return "";
    at = body.indexOf('"', body.indexOf(':', at + k.length()) + 1);
    int end = body.indexOf('"', at + 1);
    if (at < 0 || end < 0)
        return "";
    return std::string(body.c_str() + at + 1, end - at - 1);
}

void wifiTask(void *arg)
{
    WifiJob *j = (WifiJob *)arg;
    using S = ArcadeClient::WifiState;
    WiFi.mode(WIFI_STA);
    WiFi.begin(j->ssid.c_str(), j->pass.c_str());
    arcade::wipe(&j->pass[0], j->pass.size());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
        vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() != WL_CONNECTED) {
        post(j, S::Failed, "Could not join " + j->ssid);
    } else {
        post(j, S::SigningIn);
        NetworkClientSecure tls;
        tls.setCACert(ARCADE_ROOT_CAS);
        HTTPClient http;
        std::string base = "https://" + j->host;
        const char *wanted[] = {"Set-Cookie"};
        std::string failure, cookie, blob, account;
        std::vector<std::pair<std::string, std::string>> coins;
        http.setUserAgent("DogecoinArcade-TDeck/1");
        http.setTimeout(20000);
        if (!http.begin(tls, (base + "/auth/challenge").c_str()) || http.GET() != 200) {
            failure = "Could not reach " + j->host;
        } else {
            String said = http.getString();
            http.end();
            std::string nonce = jsonString(said, "nonce"), origin = jsonString(said, "origin");
            uint8_t sig[64];
            j->keys->signLogin(origin, nonce, sig);
            const uint8_t *pk = j->keys->loginPublic();
            std::string body = "{\"pubkey\":\"" + arcade::toHex(pk, 32) + "\",\"nonce\":\"" + nonce +
                               "\",\"signature\":\"" + arcade::toHex(sig, 64) + "\",\"join\":false}";
            http.begin(tls, (base + "/auth/login").c_str());
            http.collectHeaders(wanted, 1);
            http.addHeader("Content-Type", "application/json");
            int code = http.POST(body.c_str());
            String setCookie = http.header("Set-Cookie");
            http.end();
            int at = setCookie.indexOf("arcade_session=");
            if (code != 200 || at < 0) {
                failure = "The arcade did not sign this account in (" + std::to_string(code) + ")";
            } else {
                int end = setCookie.indexOf(';', at);
                cookie = std::string(setCookie.substring(at, end < 0 ? setCookie.length() : end).c_str());
                post(j, S::Downloading);
                http.begin(tls, (base + "/account/mailbox").c_str());
                http.addHeader("Cookie", cookie.c_str());
                code = http.GET();
                if (code != 200) {
                    failure = "The arcade would not hand over the mailbox (" + std::to_string(code) + ")";
                } else {
                    String mbox = http.getString();
                    blob = jsonString(mbox, "blob");
                }
                http.end();
                // the account itself: @tag, chains, balances
                if (failure.empty()) {
                    http.begin(tls, (base + "/account").c_str());
                    http.addHeader("Cookie", cookie.c_str());
                    if (http.GET() == 200) {
                        String a = http.getString();
                        account.assign(a.c_str(), a.length());
                    }
                    http.end();
                }
                // and the coins on each chain, for paying with no gateway in range
                arcade::Snapshot snap;
                if (!account.empty() && arcade::readAccount(account.data(), account.size(), snap)) {
                    for (auto &c : snap.chains) {
                        http.begin(tls, (base + "/account/coins?chain=" + c.network).c_str());
                        http.addHeader("Cookie", cookie.c_str());
                        if (http.GET() == 200 && http.getSize() < 2000000) {
                            String cs = http.getString();
                            coins.emplace_back(c.network, std::string(cs.c_str(), cs.length()));
                        }
                        http.end();
                    }
                }
            }
        }
        if (failure.empty())
            post(j, S::Done, "", &blob, &account, &coins);
        else
            post(j, S::Failed, failure);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    j->keys.reset();
    j->running = false;
    vTaskDelete(NULL);
}
} // namespace
#endif

ArcadeView *ArcadeView::gui = nullptr;

ArcadeView *ArcadeView::instance(void)
{
    if (!gui)
        gui = new ArcadeView(nullptr, DisplayDriverFactory::create(320, 240));
    return gui;
}

ArcadeView *ArcadeView::instance(const DisplayDriverConfig &cfg)
{
    if (!gui)
        gui = new ArcadeView(&cfg, DisplayDriverFactory::create(cfg));
    return gui;
}

ArcadeView::ArcadeView(const DisplayDriverConfig *cfg, DisplayDriver *driver)
    : MeshtasticView(cfg, driver, new ViewController)
{
}

void ArcadeView::init(IClientBase *client)
{
    ILOG_INFO("ArcadeView init");
    MeshtasticView::init(client);
    storage = new FlashStorage();
    arcadeClient = new ArcadeClient(*storage, ownNode);
    screens = new ArcadeScreens(*arcadeClient, *storage);
    arcadeClient->sendFrame = [this](uint32_t to, const Bytes &frame) { sendFrame(to, frame); };
    arcadeClient->changed = [this]() {
        screens->clientChanged();
        nameNodeAfterTag();
    };
    screens->setRadio(this);
#if !defined(ARCH_PORTDUINO)
    arcadeClient->startWifiScan = [this]() {
        if (!scanJob) {
            scanJob = new ScanJob();
            scanJob->lock = xSemaphoreCreateMutex();
        }
        if (scanJob->running || (wifiJob && wifiJob->running))
            return;
        scanJob->running = true;
        scanJob->posted = false;
        xTaskCreatePinnedToCore(scanTask, "arcade-scan", 6144, scanJob, 1, nullptr, 0);
    };
#endif
#if !defined(ARCH_PORTDUINO)
    arcadeClient->startWifiSync = [this](const std::string &ssid, const std::string &pass, const std::string &host) {
        if (wifiJob && wifiJob->running)
            return; // one at a time
        if (!wifiJob) {
            wifiJob = new WifiJob();
            wifiJob->lock = xSemaphoreCreateMutex();
        }
        wifiJob->ssid = ssid;
        wifiJob->pass = pass;
        wifiJob->host = host;
        wifiJob->client = arcadeClient;
        wifiJob->keys = arcadeClient->accountKeys();
        if (!wifiJob->keys)
            return;
        wifiJob->posted = false;
        wifiJob->state = ArcadeClient::WifiState::Joining;
        wifiJob->blob.clear();
        wifiJob->running = true;
        xTaskCreatePinnedToCore(wifiTask, "arcade-wifi", 12288, wifiJob, 1, nullptr, 0);
    };
#endif

    lv_group_t *group = lv_group_get_default();
    if (!group) {
        group = lv_group_create();
        lv_group_set_default(group);
    }
    if (inputdriver && inputdriver->hasKeyboardDevice())
        lv_indev_set_group(inputdriver->getKeyboard(), group);
    if (inputdriver && inputdriver->hasPointerDevice())
        lv_indev_set_group(inputdriver->getPointer(), group);
    screens->begin(group);
    // The controller asks the radio for its config (node number, LoRa
    // settings) only once a view's boot screen is done; the arcade view has
    // none, so it is done at once. Without this the radio never answers and
    // drops every packet the view sends.
    state = MeshtasticView::eBootScreenDone;
}

bool ArcadeView::setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig)
{
    uiCfg = uiconfig;
    radio.screenTimeout = uiconfig.version == 1 && uiconfig.screen_timeout > 1 ? uiconfig.screen_timeout : 30;
    publishRadio();
    // brightness and screen timeout as the Meshtastic UI kept them (30 s by default)
    uint32_t timeout = uiconfig.version == 1 && uiconfig.screen_timeout > 1 ? uiconfig.screen_timeout : 30;
    if (displaydriver->hasLight())
        displaydriver->setBrightness(uiconfig.version == 1 && uiconfig.screen_brightness ? uiconfig.screen_brightness : 153);
    displaydriver->setScreenTimeout(timeout);
    return true;
}

void ArcadeView::task_handler(void)
{
    MeshtasticView::task_handler();
    if (!arcadeClient)
        return;
    uint32_t now = millis();
#if !defined(ARCH_PORTDUINO)
    if (scanJob && scanJob->posted) {
        xSemaphoreTake(scanJob->lock, portMAX_DELAY);
        std::vector<ArcadeClient::WifiNet> nets;
        nets.swap(scanJob->nets);
        scanJob->posted = false;
        xSemaphoreGive(scanJob->lock);
        arcadeClient->wifiScanned(std::move(nets));
    }
    if (wifiJob && wifiJob->posted) {
        xSemaphoreTake(wifiJob->lock, portMAX_DELAY);
        auto st = wifiJob->state;
        std::string why = wifiJob->why, blob, account;
        std::vector<std::pair<std::string, std::string>> coins;
        blob.swap(wifiJob->blob);
        account.swap(wifiJob->account);
        coins.swap(wifiJob->coins);
        wifiJob->posted = false;
        xSemaphoreGive(wifiJob->lock);
        if (st == ArcadeClient::WifiState::Done) {
            Snapshot snap;
            if (!account.empty() && readAccount(account.data(), account.size(), snap))
                arcadeClient->applySnapshot(snap);
            for (auto &c : coins) {
                std::vector<Coin> got;
                if (readCoins(c.second.data(), c.second.size(), got))
                    arcadeClient->saveCoins(c.first, got);
            }
            arcadeClient->importMailbox(blob);
        } else {
            arcadeClient->wifiProgress(st, why);
        }
    }
#endif
    arcadeClient->tick(now);
    screens->tick(now);
    time_t t = time(nullptr);
    if (t > 1600000000 && (uint32_t)t / 30 != lastClock / 30) {
        lastClock = (uint32_t)t;
        screens->setClock(lastClock);
        arcadeClient->setClock(lastClock);
    }
    if (screens->current() == ArcadeScreens::Scr::Settings && now - lastStatusAsk > 5000) {
        lastStatusAsk = now;
        refresh(); // Bluetooth connected or not, while the page is open
    }
}

void ArcadeView::setMyInfo(uint32_t nodeNum, meshtastic_MyNodeInfo_device_id_t device_id)
{
    ownNode = nodeNum;
    if (screens)
        screens->setMyNode(nodeNum);
}

void ArcadeView::packetReceived(const meshtastic_MeshPacket &p)
{
    MeshtasticView::packetReceived(p);
    if (arcadeClient && p.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p.decoded.portnum == meshtastic_PortNum_PRIVATE_APP)
        arcadeClient->receive(p.from, p.decoded.payload.bytes, p.decoded.payload.size, millis());
}

void ArcadeView::updateMetrics(uint32_t nodeNum, uint32_t bat_level, float voltage, float chUtil, float airUtil)
{
    if (nodeNum == ownNode && screens)
        screens->setBattery((int)bat_level);
}

void ArcadeView::updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg)
{
    if (cfg.hop_limit)
        hopLimit = (uint8_t)cfg.hop_limit;
}

void ArcadeView::updateTime(uint32_t t)
{
    if (screens && t > 1600000000)
        screens->setClock(t);
}

void ArcadeView::sendFrame(uint32_t to, const std::vector<uint8_t> &frame)
{
    // direct messages ride Meshtastic's end-to-end (PKC) encryption; a
    // broadcast (looking for a gateway) carries nothing private
    bool broadcast = to == 0xFFFFFFFF;
    controller->sendAppData(to, hopLimit, meshtastic_PortNum_PRIVATE_APP, frame.data(), frame.size(), !broadcast);
}

// ---- the radio's settings, for the Settings page ---------------------------------

void ArcadeView::publishRadio(void)
{
    radio.known = havePosition || haveBluetooth || havePower;
    if (screens)
        screens->setRadioState(radio);
}

void ArcadeView::setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                                   bool can_shutdown)
{
    radio.firmware = version ? version : "";
    publishRadio();
}

void ArcadeView::addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg)
{
    if (nodeNum == ownNode) {
        ownUser = cfg;
        haveUser = true;
        radio.longName = cfg.long_name;
        publishRadio();
        nameNodeAfterTag();
    }
}

void ArcadeView::updateNode(uint32_t nodeNum, uint8_t channel, const meshtastic_User &cfg)
{
    addOrUpdateNode(nodeNum, channel, 0, cfg);
}

void ArcadeView::nameNodeAfterTag(void)
{
    // Other Meshtastic users see this T-Deck as its @tag.
    if (!haveUser || !arcadeClient || arcadeClient->tag.empty())
        return;
    std::string want = "@" + arcadeClient->tag;
    if (want == ownUser.long_name || want == namedFor)
        return;
    namedFor = want;
    meshtastic_User u = ownUser;
    strncpy(u.long_name, want.c_str(), sizeof(u.long_name) - 1);
    u.long_name[sizeof(u.long_name) - 1] = 0;
    std::string shortName;
    for (char c : arcadeClient->tag)
        if (shortName.size() < 4 && isalnum((unsigned char)c))
            shortName += (char)toupper((unsigned char)c);
    strncpy(u.short_name, shortName.c_str(), sizeof(u.short_name) - 1);
    u.short_name[sizeof(u.short_name) - 1] = 0;
    controller->sendConfig(u, ownNode);
    ownUser = u;
    radio.longName = want;
    publishRadio();
}

void ArcadeView::updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision)
{
    if (nodeNum != ownNode)
        return;
    radio.sats = sats;
    radio.fix = lat != 0 || lon != 0;
    radio.lat = lat * 1e-7;
    radio.lon = lon * 1e-7;
    radio.alt = alt;
    publishRadio();
}

void ArcadeView::updatePositionConfig(const meshtastic_Config_PositionConfig &cfg)
{
    positionCfg = cfg;
    havePosition = true;
    radio.gpsMode = (int)cfg.gps_mode;
    publishRadio();
}

void ArcadeView::updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id)
{
    bluetoothCfg = cfg;
    haveBluetooth = true;
    radio.btEnabled = cfg.enabled;
    if (cfg.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN)
        radio.btPin = cfg.fixed_pin;
    publishRadio();
}

void ArcadeView::updatePowerConfig(const meshtastic_Config_PowerConfig &cfg)
{
    powerCfg = cfg;
    havePower = true;
    radio.powerSaving = cfg.is_power_saving;
    // A handheld arcade wants to stay awake (USB, the mesh, the gateway's mail)
    // unless its owner chose otherwise on the Settings page.
    Bytes b;
    bool chosen = storage && storage->load("power", b) && !b.empty();
    if (!chosen && cfg.is_power_saving)
        applyPowerSaving(false, false);
    publishRadio();
}

void ArcadeView::updateConnectionStatus(const meshtastic_DeviceConnectionStatus &status)
{
    if (status.has_bluetooth) {
        radio.btConnected = status.bluetooth.is_connected;
        if (status.bluetooth.pin)
            radio.btPin = status.bluetooth.pin;
    }
    publishRadio();
}

void ArcadeView::setGps(bool on)
{
    if (!havePosition)
        return;
    meshtastic_Config_PositionConfig cfg = positionCfg;
    cfg.gps_mode = on ? meshtastic_Config_PositionConfig_GpsMode_ENABLED : meshtastic_Config_PositionConfig_GpsMode_DISABLED;
    positionCfg = cfg;
    radio.gpsMode = (int)cfg.gps_mode;
    controller->sendConfig(std::move(cfg), ownNode);
    publishRadio();
}

void ArcadeView::setBluetooth(bool on)
{
    if (!haveBluetooth)
        return;
    meshtastic_Config_BluetoothConfig cfg = bluetoothCfg;
    cfg.enabled = on;
    if (on && cfg.mode != meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN) {
        // a PIN the owner can read on the Settings page, not one that flashes past
        uint32_t pin = 0;
        arcade::randomBytes((uint8_t *)&pin, sizeof(pin));
        cfg.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
        cfg.fixed_pin = 100000 + pin % 900000;
    }
    bluetoothCfg = cfg;
    radio.btEnabled = on;
    radio.btPin = cfg.fixed_pin;
    controller->sendConfig(std::move(cfg), ownNode); // the radio restarts to apply this
    publishRadio();
}

void ArcadeView::setPowerSaving(bool on)
{
    applyPowerSaving(on, true);
}

void ArcadeView::applyPowerSaving(bool on, bool ownersChoice)
{
    if (!havePower)
        return;
    meshtastic_Config_PowerConfig cfg = powerCfg;
    cfg.is_power_saving = on;
    powerCfg = cfg;
    radio.powerSaving = on;
    if (ownersChoice && storage)
        storage->save("power", Bytes{(uint8_t)(on ? 1 : 0)}); // from now on the owner's choice stands
    controller->sendConfig(std::move(cfg), ownNode);
    publishRadio();
}

void ArcadeView::setScreenTimeout(uint32_t seconds)
{
    radio.screenTimeout = seconds;
    displaydriver->setScreenTimeout(seconds);
    uiCfg.version = 1;
    uiCfg.screen_timeout = seconds;
    controller->storeUIConfig(uiCfg);
    publishRadio();
}

void ArcadeView::refresh(void)
{
    controller->requestDeviceConnectionStatus();
}
