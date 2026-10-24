#pragma once

// The radio's own settings, as the Settings page shows and changes them. The
// screens stay Meshtastic-free: ArcadeView fills RadioState from the radio's
// config and carries the changes back as Meshtastic config messages.

#include <stdint.h>
#include <string>

namespace arcade
{

struct RadioState {
    bool known = false;
    int gpsMode = 0;                 // 0 off, 1 on, 2 no GPS fitted
    uint32_t sats = 0;
    bool fix = false;
    double lat = 0, lon = 0;
    int32_t alt = 0;
    bool btEnabled = false;
    uint32_t btPin = 0;
    bool btConnected = false;
    bool powerSaving = false;
    uint32_t screenTimeout = 30;     // seconds, 0 = never
    std::string longName, firmware;
};

class Radio
{
  public:
    virtual ~Radio() = default;
    virtual void setGps(bool on) = 0;
    virtual void setBluetooth(bool on) = 0;
    virtual void setPowerSaving(bool on) = 0;
    virtual void setScreenTimeout(uint32_t seconds) = 0;
    virtual void refresh(void) = 0;   // ask for Bluetooth status again
};

} // namespace arcade
