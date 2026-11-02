#pragma once

// The satellite beacon (MESHSAT-1667, owner 2 Nov 2026), with no Arduino in it so a host test can drive it.
//
// While the panel switch has the satellite on, the node writes its position to the Hub, which passes each message on
// by SMS to the tenant's contact named MESHSAT_BEACON_CONTACT through the "@" lane: one message the moment the switch
// goes on, one every 10 minutes while it stays on, and one when the beacon goes off (the switch, the 15 minute idle
// or the 30 minute limit), each with the last GPS fix the node has seen. After the switch goes off the modem stays up
// a few minutes for that last message.
//
// Two cases send nothing more: switched off before any beacon message reached the modem (a flick of the switch is no
// alarm, and what was waiting is dropped), and a cell low enough to stop the modem (the modem cannot run).

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace meshsat
{
enum class BeaconKind : uint8_t {
    None = 0,
    Activated,
    Position,
    Off,
};

// Why the beacon went off, for its last message.
enum class BeaconOffReason : uint8_t {
    Switch,
    Idle,
    Limit,
};

struct BeaconTimes {
    uint32_t intervalMs = 10 * 60 * 1000UL;
    // After the beacon goes off, the modem stays up at most this long for the last message and what is still in it.
    uint32_t drainMs = 3 * 60 * 1000UL;
};

class SatBeacon
{
  public:
    explicit SatBeacon(BeaconTimes times = BeaconTimes()) : t(times) {}

    void onArmed(uint32_t now)
    {
        isOn = true;
        draining = false;
        offDue = false;
        activatedDue = true;
        nextPositionMs = now + t.intervalMs;
    }

    // The satellite was disarmed. handedOver: a beacon message of this arming reached the modem. Returns true when
    // the caller must drop the beacon messages still waiting on the node: nothing went out, so nothing will.
    bool onDisarmed(uint32_t now, BeaconOffReason why, bool handedOver)
    {
        if (!isOn)
            return false;
        isOn = false;
        activatedDue = false;
        if (!handedOver) {
            offDue = false;
            draining = false;
            return true;
        }
        offReason = why;
        offDue = true;
        draining = true;
        disarmedAtMs = now;
        return false;
    }

    // The supply stopped the modem for a low cell: the beacon ends with nothing more sent.
    void onCellLow()
    {
        isOn = false;
        activatedDue = false;
        offDue = false;
        draining = false;
    }

    // The message due now, if any; the caller queues it and calls taken().
    BeaconKind due(uint32_t now) const
    {
        if (activatedDue)
            return BeaconKind::Activated;
        if (offDue)
            return BeaconKind::Off;
        if (isOn && static_cast<int32_t>(now - nextPositionMs) >= 0)
            return BeaconKind::Position;
        return BeaconKind::None;
    }

    void taken(uint32_t now, BeaconKind kind)
    {
        switch (kind) {
        case BeaconKind::Activated:
            activatedDue = false;
            break;
        case BeaconKind::Off:
            offDue = false;
            break;
        case BeaconKind::Position:
            // Counted from the switch, not from the hand-over, so the messages keep their 10 minute rhythm.
            while (static_cast<int32_t>(now - nextPositionMs) >= 0)
                nextPositionMs += t.intervalMs;
            break;
        case BeaconKind::None:
            break;
        }
    }

    // While the beacon goes off, the modem stays up until its last message is out (settled: nothing of the beacon's
    // waiting on the node and nothing in the modem), or drainMs after the switch, whichever comes first.
    bool holdsModem(uint32_t now, bool settled) const
    {
        return draining && now - disarmedAtMs < t.drainMs && (offDue || !settled);
    }

    // Ends the drain once holdsModem() turns false; true when it just ended.
    bool finishDrain(uint32_t now, bool settled)
    {
        if (!draining || holdsModem(now, settled))
            return false;
        draining = false;
        return true;
    }

    bool on() const { return isOn; }
    bool isDraining() const { return draining; }
    BeaconOffReason lastOffReason() const { return offReason; }
    // Milliseconds to the next position message while on; 0 when one is due or the beacon is off.
    uint32_t nextInMs(uint32_t now) const
    {
        if (!isOn || static_cast<int32_t>(now - nextPositionMs) >= 0)
            return 0;
        return nextPositionMs - now;
    }

  private:
    BeaconTimes t;
    bool isOn = false;
    bool activatedDue = false;
    bool offDue = false;
    bool draining = false;
    BeaconOffReason offReason = BeaconOffReason::Switch;
    uint32_t nextPositionMs = 0;
    uint32_t disarmedAtMs = 0;
};

// The last position the node knows, as the beacon reports it.
struct BeaconFix {
    bool have = false;
    // Degrees x 1e7, as Meshtastic keeps them.
    int32_t latitudeI = 0;
    int32_t longitudeI = 0;
    uint32_t ageS = 0;
    // A fixed position set by hand, not a GPS fix.
    bool manual = false;
};

// Degrees x 1e7 as decimal degrees with 5 decimals (about a metre), rounded half away from zero.
inline void formatBeaconDegrees(char *out, size_t size, int32_t valueI)
{
    const bool negative = valueI < 0;
    int64_t magnitude = negative ? -static_cast<int64_t>(valueI) : static_cast<int64_t>(valueI);
    magnitude = (magnitude + 50) / 100;
    snprintf(out, size, "%s%ld.%05ld", negative ? "-" : "", static_cast<long>(magnitude / 100000),
             static_cast<long>(magnitude % 100000));
}

// "GPS fix now", "GPS fix 7 min old", "GPS fix 3 h old", "GPS fix 2 d old".
inline void formatBeaconFixAge(char *out, size_t size, uint32_t ageS)
{
    if (ageS < 60)
        snprintf(out, size, "GPS fix now");
    else if (ageS < 100 * 60)
        snprintf(out, size, "GPS fix %u min old", static_cast<unsigned>(ageS / 60));
    else if (ageS < 48 * 3600)
        snprintf(out, size, "GPS fix %u h old", static_cast<unsigned>(ageS / 3600));
    else
        snprintf(out, size, "GPS fix %u d old", static_cast<unsigned>(ageS / 86400));
}

inline const char *beaconOffReasonText(BeaconOffReason why)
{
    switch (why) {
    case BeaconOffReason::Switch:
        return "switched off";
    case BeaconOffReason::Idle:
        return "15 min idle";
    case BeaconOffReason::Limit:
        return "30 min limit";
    }
    return "";
}

// The message as it goes over satellite, plain ASCII for the Hub's "@" lane:
//   @beacon BEACON ACTIVATED https://maps.google.com/?q=52.16012,4.49701 GPS fix now
//   @beacon BEACON https://maps.google.com/?q=52.16012,4.49701 GPS fix 2 min old
//   @beacon BEACON OFF: switched off https://maps.google.com/?q=52.16012,4.49701 GPS fix 9 min old
//   @beacon BEACON ACTIVATED, no GPS fix yet
// Returns the length written, as snprintf does.
inline int formatBeaconText(char *out, size_t size, const char *contact, BeaconKind kind, BeaconOffReason why,
                            const BeaconFix &fix)
{
    const char *head = kind == BeaconKind::Activated ? "BEACON ACTIVATED" : (kind == BeaconKind::Off ? "BEACON OFF" : "BEACON");
    char reason[24] = "";
    if (kind == BeaconKind::Off)
        snprintf(reason, sizeof(reason), ": %s", beaconOffReasonText(why));
    if (!fix.have)
        return snprintf(out, size, "@%s %s%s, no GPS fix yet", contact, head, reason);
    char latitude[16];
    char longitude[16];
    formatBeaconDegrees(latitude, sizeof(latitude), fix.latitudeI);
    formatBeaconDegrees(longitude, sizeof(longitude), fix.longitudeI);
    char when[24];
    if (fix.manual)
        snprintf(when, sizeof(when), "position set by hand");
    else
        formatBeaconFixAge(when, sizeof(when), fix.ageS);
    return snprintf(out, size, "@%s %s%s https://maps.google.com/?q=%s,%s %s", contact, head, reason, latitude, longitude, when);
}
} // namespace meshsat
