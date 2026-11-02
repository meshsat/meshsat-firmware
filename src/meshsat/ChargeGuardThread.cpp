#include "meshsat/ChargeGuardThread.h"

#if MESHSAT_IRIDIUM && defined(MESHSAT_CHARGE_GUARD) && defined(HAS_PMU)

#include "Power.h"
#include "concurrency/OSThread.h"
#include "mesh/Throttle.h"
#include "meshsat/ChargeGuard.h"
#if HAS_SCREEN
#include "graphics/Screen.h"
#include "main.h"
#endif

#include <Arduino.h>

namespace meshsat
{
static constexpr int32_t TICK_MS = 5 * 1000;
// While it charges, the charger chip warms itself (about 0,65 W at 500 mA): charging pauses this often for this long
// and the chip is read at the end of the pause, so its own charging heat does not count.
static constexpr uint32_t PAUSE_EVERY_MS = 10 * 60 * 1000UL;
static constexpr uint32_t PAUSE_MS = 45 * 1000UL;
// While it does not charge, the chip is read this often.
static constexpr uint32_t READ_EVERY_MS = 30 * 1000UL;
static constexpr uint32_t BANNER_MS = 4 * 1000UL;

class ChargeGuardThread : public concurrency::OSThread
{
  public:
    ChargeGuardThread() : concurrency::OSThread("ChargeGuard") {}
    void noteModem(float c) { guard.noteModem(millis(), c); }

  protected:
    int32_t runOnce() override;

  private:
    void setCharging(bool on, uint32_t now);
    void readCharger(uint32_t now);
    void report(ChargeVerdict v, uint32_t now);

    XPowersAXP2101 *pmu = nullptr;
    bool started = false;
    bool chargingOn = false;
    bool pausing = false;
    uint32_t pauseStartMs = 0;
    uint32_t chargingSinceMs = 0;
    uint32_t lastReadMs = 0;
    bool reported = false;
    ChargeVerdict lastVerdict = ChargeVerdict::NoReading;
    ChargeGuard guard;
};

static ChargeGuardThread *guardThread = nullptr;

void ChargeGuardThread::setCharging(bool on, uint32_t now)
{
    if (on == chargingOn)
        return;
    if (on)
        pmu->enableCellbatteryCharge();
    else
        pmu->disableCellbatteryCharge();
    chargingOn = on;
    if (on)
        chargingSinceMs = now == 0 ? 1 : now;
}

void ChargeGuardThread::readCharger(uint32_t now)
{
    const float c = pmu->getTemperature();
    guard.noteCharger(now, c);
    lastReadMs = now == 0 ? 1 : now;
    LOG_DEBUG("MeshSat charge guard: charger chip %.1f C%s", c, guard.modemFresh(now) ? "" : ", no modem reading");
}

void ChargeGuardThread::report(ChargeVerdict v, uint32_t now)
{
    char modem[24] = "no modem reading";
    if (guard.modemFresh(now))
        snprintf(modem, sizeof(modem), "modem %.0f C", guard.modemC());
    const uint32_t holdMin = (guard.holdLeftMs(now, v) + 59999) / 60000;
    if (holdMin > 0)
        LOG_INFO("MeshSat charge guard: %s, %u min left (charger chip %.1f C, room estimate %.1f C, %s)", chargeVerdictText(v),
                 (unsigned)holdMin, guard.chargerC(), guard.chargerRoomC(now), modem);
    else
        LOG_INFO("MeshSat charge guard: %s (charger chip %.1f C, room estimate %.1f C, %s)", chargeVerdictText(v),
                 guard.chargerC(), guard.chargerRoomC(now), modem);
#if HAS_SCREEN
    // A banner only where someone plugged in can act on it, and never for the start-up wait for a first reading.
    const bool plugged = pmu->isVbusIn();
    const bool quiet = [](ChargeVerdict x) {
        return x == ChargeVerdict::Allowed || x == ChargeVerdict::NoReading || x == ChargeVerdict::WarmingUp;
    }(lastVerdict);
    const bool wasPausedForTemperature = reported && !quiet;
    if (screen && plugged) {
        if (v == ChargeVerdict::Allowed && wasPausedForTemperature)
            screen->showSimpleBanner("Charging resumed", BANNER_MS);
        else if (v != ChargeVerdict::Allowed && v != ChargeVerdict::NoReading && v != ChargeVerdict::WarmingUp)
            screen->showSimpleBanner(chargeVerdictText(v), BANNER_MS);
    }
#endif
}

int32_t ChargeGuardThread::runOnce()
{
    const uint32_t now = millis();
    if (!started) {
        started = true;
        if (!PMU || PMU->getChipModel() != XPOWERS_AXP2101) {
            LOG_WARN("MeshSat charge guard: no AXP2101 charger, the guard stays off");
            return disable();
        }
        pmu = static_cast<XPowersAXP2101 *>(PMU);
        pmu->enableTemperatureMeasure();
        // Off until a reading allows it: the charger has charged on its own since power-up (P5 measures that gap).
        chargingOn = true;
        setCharging(false, now);
        LOG_INFO("MeshSat charge guard: charging only inside 10 to 35 C (charger chip minus its 15 C rise after a "
                 "10 min warm-up; 9704 board raw for cold, over 50 C for heat); 60 min after cold, 30 min after hot");
        return 1000;
    }

    // The charger chip is read only while it does not charge, or at the end of a short pause.
    if (pausing) {
        if (!Throttle::hasElapsed(pauseStartMs, PAUSE_MS))
            return TICK_MS;
        readCharger(now);
        pausing = false;
    } else if (chargingOn && pmu->isCharging()) {
        if (Throttle::hasElapsed(chargingSinceMs, PAUSE_EVERY_MS)) {
            pausing = true;
            pauseStartMs = now;
            setCharging(false, now);
            return TICK_MS;
        }
    } else if (lastReadMs == 0 || Throttle::hasElapsed(lastReadMs, READ_EVERY_MS)) {
        readCharger(now);
    }

    const ChargeVerdict v = guard.evaluate(now);
    setCharging(v == ChargeVerdict::Allowed, now);
    if (!reported || v != lastVerdict) {
        report(v, now);
        lastVerdict = v;
        reported = true;
    }
    return TICK_MS;
}

void startChargeGuard()
{
    if (!guardThread)
        guardThread = new ChargeGuardThread();
}

void chargeGuardNoteModemC(float c)
{
    if (guardThread)
        guardThread->noteModem(c);
}
} // namespace meshsat

#endif
