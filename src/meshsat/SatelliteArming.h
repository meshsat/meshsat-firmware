#pragma once

// The panel button's arming logic (MESHSAT-1667), with no Arduino in it so a host test can drive it.
//
// One input, the button pin read as "pressed" (pulled up, the switch to ground), sampled often. The press edge
// arms the satellite and, on a latching switch, the release edge disarms it; on a momentary switch a second press
// disarms. The kind of switch is told by how long the pin stays down: held LATCHING_MS or more, it is a latching
// switch (nobody holds a momentary key that long), and that is kept from then on. Until then a press toggles, so
// a momentary switch needs nothing. Arming happens only on a press edge, never on a level: a latching switch left
// in after a timeout, a reset or a refusal arms again only once it has been released and pressed.
//
// Inactive until the first debounced press this unit has ever seen (kept by the caller in NVS): a unit with no
// button wired, or one never pressed, keeps the modem as it runs without the button (MESHSAT-1657).
//
// While armed, 15 minutes without satellite traffic and 30 minutes after arming disarm by themselves (CE
// technical file, section 2, Shutdown pending). A reset never arms: restore() starts disarmed.

#include <cstdint>

namespace meshsat
{
enum class SwitchKind : uint8_t {
    // A press toggles; right for a momentary switch, and for a latching one until it is known.
    Unknown = 0,
    // The pin's level is the user's choice: in = armed, out = disarmed.
    Latching = 1,
};

enum class ArmEvent : uint8_t {
    None = 0,
    Armed,
    DisarmedByButton,
    DisarmedIdle,
    DisarmedLimit,
    DisarmedCellLow,
    // A press asked to arm while the cell was under the arming threshold.
    RefusedCellLow,
    // The pin stayed down LATCHING_MS: a latching switch, kept from now on.
    LatchingDetected,
};

struct ArmingTimes {
    uint32_t debounceMs = 50;
    uint32_t latchingMs = 10 * 1000UL;
    uint32_t idleMs = 15 * 60 * 1000UL;
    uint32_t limitMs = 30 * 60 * 1000UL;
};

class SatelliteArming
{
  public:
    explicit SatelliteArming(ArmingTimes times = ArmingTimes()) : t(times) {}

    // At boot, from NVS: whether this unit's button was ever pressed, the switch kind, and the pin as read now.
    // Always disarmed afterwards. A pin already down is the debounced state, so it makes no edge.
    void restore(uint32_t now, bool wasActive, SwitchKind savedKind, bool pressedNow)
    {
        isActive = wasActive;
        switchKind = savedKind;
        rawPressed = pressedNow;
        rawSinceMs = now;
        stablePressed = pressedNow;
        pressedAtMs = now;
        isArmed = false;
        dirty = false;
    }

    // Called on every sample. canArm: the cell is above the arming threshold (or the unit is on USB).
    ArmEvent update(uint32_t now, bool pressed, bool canArm)
    {
        if (pressed != rawPressed) {
            rawPressed = pressed;
            rawSinceMs = now;
        }
        if (rawPressed != stablePressed && now - rawSinceMs >= t.debounceMs) {
            stablePressed = rawPressed;
            lastEdge = stablePressed ? Edge::Pressed : Edge::Released;
            lastHeldMs = stablePressed ? 0 : now - pressedAtMs;
            const ArmEvent ev = stablePressed ? onPress(now, canArm) : onRelease();
            if (ev != ArmEvent::None)
                return ev;
        }
        if (isActive && stablePressed && switchKind != SwitchKind::Latching && now - pressedAtMs >= t.latchingMs) {
            switchKind = SwitchKind::Latching;
            dirty = true;
            return ArmEvent::LatchingDetected;
        }
        if (isArmed) {
            if (now - armedAtMs >= t.limitMs)
                return disarm(ArmEvent::DisarmedLimit);
            if (now - lastTrafficMs >= t.idleMs)
                return disarm(ArmEvent::DisarmedIdle);
        }
        return ArmEvent::None;
    }

    // A satellite message went in or out (JSPR messageOriginate/messageTerminate): restarts the idle timer.
    void noteTraffic(uint32_t now)
    {
        if (isArmed)
            lastTrafficMs = now;
    }

    // The supply logic stopped the modem for a low cell: disarmed, and voltage recovery never arms again.
    ArmEvent disarmForCell() { return isArmed ? disarm(ArmEvent::DisarmedCellLow) : ArmEvent::None; }

    bool active() const { return isActive; }
    bool armed() const { return isArmed; }
    SwitchKind kind() const { return switchKind; }
    bool pressed() const { return stablePressed; }
    // The edge behind the last update that changed the debounced level, for the log.
    enum class Edge : uint8_t { None, Pressed, Released };
    Edge takeEdge(uint32_t *heldMs)
    {
        const Edge e = lastEdge;
        lastEdge = Edge::None;
        if (heldMs)
            *heldMs = lastHeldMs;
        return e;
    }
    // True once after active or kind changed: the caller writes both to NVS.
    bool takeDirty()
    {
        const bool d = dirty;
        dirty = false;
        return d;
    }

  private:
    ArmEvent onPress(uint32_t now, bool canArm)
    {
        pressedAtMs = now;
        if (!isActive) {
            isActive = true;
            dirty = true;
        }
        if (switchKind != SwitchKind::Latching && isArmed)
            return disarm(ArmEvent::DisarmedByButton);
        if (isArmed)
            return ArmEvent::None;
        if (!canArm)
            return ArmEvent::RefusedCellLow;
        isArmed = true;
        armedAtMs = now;
        lastTrafficMs = now;
        return ArmEvent::Armed;
    }

    ArmEvent onRelease()
    {
        if (switchKind == SwitchKind::Latching && isArmed)
            return disarm(ArmEvent::DisarmedByButton);
        return ArmEvent::None;
    }

    ArmEvent disarm(ArmEvent why)
    {
        isArmed = false;
        return why;
    }

    ArmingTimes t;
    bool isActive = false;
    SwitchKind switchKind = SwitchKind::Unknown;
    bool rawPressed = false;
    uint32_t rawSinceMs = 0;
    bool stablePressed = false;
    uint32_t pressedAtMs = 0;
    bool isArmed = false;
    uint32_t armedAtMs = 0;
    uint32_t lastTrafficMs = 0;
    bool dirty = false;
    Edge lastEdge = Edge::None;
    uint32_t lastHeldMs = 0;
};

inline const char *armEventText(ArmEvent ev)
{
    switch (ev) {
    case ArmEvent::Armed:
        return "Satellite armed";
    case ArmEvent::DisarmedByButton:
        return "Satellite disarmed";
    case ArmEvent::DisarmedIdle:
        return "Disarmed: 15 min idle";
    case ArmEvent::DisarmedLimit:
        return "Disarmed: 30 min limit";
    case ArmEvent::DisarmedCellLow:
        return "Disarmed: cell low";
    case ArmEvent::RefusedCellLow:
        return "Not armed: cell low";
    case ArmEvent::LatchingDetected:
        return "Latching switch";
    case ArmEvent::None:
        break;
    }
    return "";
}
} // namespace meshsat
