#pragma once

// The charge temperature guard (MESHSAT-1774, owner's rule of 2 Nov 2026), with no Arduino in it so a host test can
// drive it.
//
// The T-Beam S3 Core has no thermistor on its cell: LILYGO left the AXP2101's battery temperature input unconnected
// and the firmware switches it off, so the charger charges whatever the cell's temperature. This guard decides from
// what the node can read on its boards instead, with wide margins against the cell's own 0 to 45 C charging range:
//
//   - charging is allowed only while every fresh reading is inside 10 to 35 C;
//   - the cell lags the boards, so after any reading under 10 C charging waits 60 minutes, after any over 35 C
//     30 minutes, counted from the last such reading;
//   - the charger's own reading is required: without a fresh one there is no charging.
//
// Readings: the charger chip (AXP2101 die, taken while it does not charge, so its own charging heat does not count)
// and, while the modem runs, the RockBLOCK 9704's board temperature. Not validated against the cell on a bench
// (owner's decision of 2 Nov 2026); the margins stand in for that.
//
// The charger chip also feeds the ESP32-S3 and the radios, so even at rest it sits above the room: 35,7 to 37,3 C
// at 21 C on unit 0001 (2 Nov 2026). Its reading minus CHARGER_RISE_C stands for the room. Right after a cold
// power-on that rise has not built up yet, so for the first minutes (warm-up) nothing charges and only a cold raw
// reading counts. The 9704's own rise is not known: its raw reading counts for the cold limit, and for heat only above
// modemHotC.

#include <cstdint>

namespace meshsat
{
enum class ChargeVerdict : uint8_t {
    Allowed = 0,
    // No fresh reading from the charger chip.
    NoReading,
    // The first minutes after the guard started: the charger chip's own rise is still building.
    WarmingUp,
    TooCold,
    TooHot,
    // Inside the window, waiting for the cell to follow after a cold reading.
    ColdHold,
    // Inside the window, waiting for the cell to follow after a hot reading.
    HotHold,
};

struct ChargeGuardLimits {
    float lowC = 10.0f;
    float highC = 35.0f;
    uint32_t coldHoldMs = 60 * 60 * 1000UL;
    uint32_t hotHoldMs = 30 * 60 * 1000UL;
    // A reading older than this no longer counts.
    uint32_t staleMs = 15 * 60 * 1000UL;
    // How far the charger chip sits above the room at rest (unit 0001: about 15,5 C at 21 C, 2 Nov 2026).
    float chargerRiseC = 15.0f;
    // After the guard's first reading: no charging, cold raw readings only, while that rise builds.
    uint32_t warmUpMs = 10 * 60 * 1000UL;
    // The 9704 board's raw reading stops charging for heat only above this.
    float modemHotC = 50.0f;
};

class ChargeGuard
{
  public:
    explicit ChargeGuard(ChargeGuardLimits limits = ChargeGuardLimits()) : l(limits) {}

    // The charger chip's reading (required) and the modem's board reading (when the modem runs).
    void noteCharger(uint32_t now, float c)
    {
        note(charger, now, c);
        if (charger.valid && !chargerEverValid) {
            chargerEverValid = true;
            firstChargerMs = now;
        }
    }
    void noteModem(uint32_t now, float c) { note(modem, now, c); }

    ChargeVerdict evaluate(uint32_t now)
    {
        if (!fresh(charger, now))
            return ChargeVerdict::NoReading;
        const bool modemOk = fresh(modem, now);
        const bool warming = now - firstChargerMs < l.warmUpMs;
        // Cold: the charger chip raw while it warms up, its estimate of the room after; the modem raw.
        const float chargerRoom = warming ? charger.c : charger.c - l.chargerRiseC;
        float lowest = chargerRoom;
        if (modemOk && modem.c < lowest)
            lowest = modem.c;
        if (lowest < l.lowC) {
            coldSeen = true;
            lastColdMs = now;
            return ChargeVerdict::TooCold;
        }
        if (warming)
            return ChargeVerdict::WarmingUp;
        if (chargerRoom > l.highC || (modemOk && modem.c > l.modemHotC)) {
            hotSeen = true;
            lastHotMs = now;
            return ChargeVerdict::TooHot;
        }
        if (coldSeen && now - lastColdMs < l.coldHoldMs)
            return ChargeVerdict::ColdHold;
        if (hotSeen && now - lastHotMs < l.hotHoldMs)
            return ChargeVerdict::HotHold;
        return ChargeVerdict::Allowed;
    }

    // The charger chip's estimate of the room once warmed up (its raw reading minus its rise).
    float chargerRoomC(uint32_t now) const { return now - firstChargerMs < l.warmUpMs ? charger.c : charger.c - l.chargerRiseC; }
    // For the log and the screen: the readings that count now (NaN-free: valid flags).
    bool chargerFresh(uint32_t now) const { return fresh(charger, now); }
    bool modemFresh(uint32_t now) const { return fresh(modem, now); }
    float chargerC() const { return charger.c; }
    float modemC() const { return modem.c; }
    // Milliseconds left on a running hold (0 when none).
    uint32_t holdLeftMs(uint32_t now, ChargeVerdict v) const
    {
        if (v == ChargeVerdict::ColdHold && now - lastColdMs < l.coldHoldMs)
            return l.coldHoldMs - (now - lastColdMs);
        if (v == ChargeVerdict::HotHold && now - lastHotMs < l.hotHoldMs)
            return l.hotHoldMs - (now - lastHotMs);
        return 0;
    }

  private:
    struct Reading {
        bool valid = false;
        float c = 0.0f;
        uint32_t atMs = 0;
    };

    static void note(Reading &r, uint32_t now, float c)
    {
        // A value that is no number, or outside what either chip can report, is no reading.
        if (!(c > -60.0f && c < 150.0f)) {
            r.valid = false;
            return;
        }
        r.valid = true;
        r.c = c;
        r.atMs = now;
    }

    bool fresh(const Reading &r, uint32_t now) const { return r.valid && now - r.atMs <= l.staleMs; }

    ChargeGuardLimits l;
    Reading charger;
    Reading modem;
    bool chargerEverValid = false;
    uint32_t firstChargerMs = 0;
    bool coldSeen = false;
    bool hotSeen = false;
    uint32_t lastColdMs = 0;
    uint32_t lastHotMs = 0;
};

inline const char *chargeVerdictText(ChargeVerdict v)
{
    switch (v) {
    case ChargeVerdict::Allowed:
        return "Charging allowed";
    case ChargeVerdict::NoReading:
        return "Charging paused: no temperature";
    case ChargeVerdict::WarmingUp:
        return "Charging waits: warming up";
    case ChargeVerdict::TooCold:
        return "Charging paused: too cold";
    case ChargeVerdict::TooHot:
        return "Charging paused: too hot";
    case ChargeVerdict::ColdHold:
        return "Charging paused: cell warming";
    case ChargeVerdict::HotHold:
        return "Charging paused: cell cooling";
    }
    return "";
}
} // namespace meshsat
