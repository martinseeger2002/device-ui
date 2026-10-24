#pragma once

#include "arcade/Radio.h"
#include "graphics/common/MeshtasticView.h"

namespace arcade
{
class ArcadeClient;
class ArcadeScreens;
class Storage;
} // namespace arcade

/**
 * @brief The DogecoinArcade T-Deck view (VIEW_ARCADE): the arcade's own
 *        screens instead of the Meshtastic chat UI. Arcade frames travel as
 *        PRIVATE_APP packets; everything else Meshtastic does (routing,
 *        relaying, node info) carries on underneath.
 */
class ArcadeView : public MeshtasticView, public arcade::Radio
{
  public:
    static ArcadeView *instance(void);
    static ArcadeView *instance(const DisplayDriverConfig &cfg);

    void init(IClientBase *client) override;
    void task_handler(void) override;
    bool setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig) override;
    void setMyInfo(uint32_t nodeNum, meshtastic_MyNodeInfo_device_id_t device_id) override;
    void packetReceived(const meshtastic_MeshPacket &p) override;
    void updateMetrics(uint32_t nodeNum, uint32_t bat_level, float voltage, float chUtil, float airUtil) override;
    void updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg) override;
    void updateTime(uint32_t time) override;
    void setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                           bool can_shutdown) override;
    void addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg) override;
    void updateNode(uint32_t nodeNum, uint8_t channel, const meshtastic_User &cfg) override;
    void updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision) override;
    void updatePositionConfig(const meshtastic_Config_PositionConfig &cfg) override;
    void updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id = 0) override;
    void updatePowerConfig(const meshtastic_Config_PowerConfig &cfg) override;
    void updateConnectionStatus(const meshtastic_DeviceConnectionStatus &status) override;

    // arcade::Radio, for the Settings page
    void setGps(bool on) override;
    void setBluetooth(bool on) override;
    void setPowerSaving(bool on) override;
    void setScreenTimeout(uint32_t seconds) override;
    void refresh(void) override;

  private:
    ArcadeView(const DisplayDriverConfig *cfg, DisplayDriver *driver);
    void sendFrame(uint32_t to, const std::vector<uint8_t> &frame);

    arcade::Storage *storage = nullptr;
    arcade::ArcadeClient *arcadeClient = nullptr;
    arcade::ArcadeScreens *screens = nullptr;
    void publishRadio(void);
    void applyPowerSaving(bool on, bool ownersChoice);
    void nameNodeAfterTag(void);

    uint8_t hopLimit = 3;
    uint32_t lastClock = 0, lastStatusAsk = 0;
    arcade::RadioState radio;
    meshtastic_Config_PositionConfig positionCfg{};
    meshtastic_Config_BluetoothConfig bluetoothCfg{};
    meshtastic_Config_PowerConfig powerCfg{};
    meshtastic_DeviceUIConfig uiCfg{};
    meshtastic_User ownUser{};
    bool havePosition = false, haveBluetooth = false, havePower = false, haveUser = false;
    std::string namedFor;
    static ArcadeView *gui;
};
