// Host test for src/meshsat/ChargeGuard.h (MESHSAT-1774). Not a PlatformIO suite (no test_ prefix):
// the CI job test:meshsat-host builds and runs it with g++.
//   g++ -std=c++17 -Wall -Wextra -Werror -I src test/meshsat_host/charge_guard_test.cpp -o /tmp/t && /tmp/t

#include "meshsat/ChargeGuard.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using meshsat::ChargeGuard;
using meshsat::ChargeVerdict;

static int failures = 0;
#define CHECK(cond)                                                                                                              \
    do {                                                                                                                         \
        if (!(cond)) {                                                                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
            failures++;                                                                                                          \
        }                                                                                                                        \
    } while (0)

static constexpr uint32_t MIN = 60 * 1000UL;

// The rule alone, without the charger chip's rise or the warm-up.
static meshsat::ChargeGuardLimits plain()
{
    meshsat::ChargeGuardLimits l;
    l.chargerRiseC = 0.0f;
    l.warmUpMs = 0;
    return l;
}

static void noReadingNoCharging()
{
    ChargeGuard g;
    CHECK(g.evaluate(1000) == ChargeVerdict::NoReading);
    // A modem reading alone is not enough: the charger's own reading is required.
    g.noteModem(1000, 22.0f);
    CHECK(g.evaluate(1000) == ChargeVerdict::NoReading);
    // A failed read (NaN) is no reading.
    g.noteCharger(1000, NAN);
    CHECK(g.evaluate(1000) == ChargeVerdict::NoReading);
}

static void warmUpThenTheRoomEstimate()
{
    // Unit 0001 at 21 C: the charger chip reads about 36,5 C at rest.
    ChargeGuard g;
    uint32_t now = 1000;
    g.noteCharger(now, 36.5f);
    CHECK(g.evaluate(now) == ChargeVerdict::WarmingUp);
    now += 10 * MIN - 1;
    g.noteCharger(now, 36.5f);
    CHECK(g.evaluate(now) == ChargeVerdict::WarmingUp);
    now += 1;
    g.noteCharger(now, 36.5f);
    CHECK(g.evaluate(now) == ChargeVerdict::Allowed);
    CHECK(g.chargerRoomC(now) > 21.4f && g.chargerRoomC(now) < 21.6f);
    // 38 C in the room: the chip near 53 C.
    g.noteCharger(now, 53.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooHot);
}

static void aColdStartIsCaughtDuringWarmUp()
{
    // Out of a cold night: the chip has not warmed yet, so its raw reading is the room.
    ChargeGuard g;
    uint32_t now = 1000;
    g.noteCharger(now, 2.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooCold);
    // It warms within minutes, the cell does not; still cold once the room estimate applies (10 + 15 = 25 C needed).
    now += 11 * MIN;
    g.noteCharger(now, 18.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooCold);
    const uint32_t lastCold = now;
    now += MIN;
    g.noteCharger(now, 36.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::ColdHold);
    now = lastCold + 60 * MIN;
    g.noteCharger(now, 36.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::Allowed);
}

static void insideTheWindowCharges()
{
    ChargeGuard g(plain());
    g.noteCharger(1000, 22.0f);
    CHECK(g.evaluate(1000) == ChargeVerdict::Allowed);
    g.noteCharger(2000, 10.0f);
    CHECK(g.evaluate(2000) == ChargeVerdict::Allowed);
    g.noteCharger(3000, 35.0f);
    CHECK(g.evaluate(3000) == ChargeVerdict::Allowed);
}

static void coldStopsThenHoldsAnHour()
{
    ChargeGuard g(plain());
    uint32_t now = 1000;
    g.noteCharger(now, 4.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooCold);
    now += 5 * MIN;
    g.noteCharger(now, 9.5f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooCold);
    const uint32_t lastCold = now;
    now += MIN;
    g.noteCharger(now, 18.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::ColdHold);
    CHECK(g.holdLeftMs(now, ChargeVerdict::ColdHold) == 59 * MIN);
    now = lastCold + 60 * MIN - 1;
    g.noteCharger(now, 20.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::ColdHold);
    now = lastCold + 60 * MIN;
    g.noteCharger(now, 20.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::Allowed);
}

static void hotStopsThenHoldsHalfAnHour()
{
    ChargeGuard g(plain());
    uint32_t now = 1000;
    g.noteCharger(now, 38.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooHot);
    now += MIN;
    g.noteCharger(now, 30.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::HotHold);
    now = 1000 + 30 * MIN;
    g.noteCharger(now, 30.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::Allowed);
}

static void theModemCountsForColdAndForGreatHeat()
{
    uint32_t now = 1000;
    // A warm modem board (its own rise unknown) does not stop charging below 50 C ...
    ChargeGuard w(plain());
    w.noteCharger(now, 25.0f);
    w.noteModem(now, 45.0f);
    CHECK(w.evaluate(now) == ChargeVerdict::Allowed);
    // ... above it does.
    w.noteModem(now, 52.0f);
    CHECK(w.evaluate(now) == ChargeVerdict::TooHot);
    // A cold modem board stops charging.
    ChargeGuard c(plain());
    c.noteCharger(now, 12.0f);
    c.noteModem(now, 8.0f);
    CHECK(c.evaluate(now) == ChargeVerdict::TooCold);
    // A modem reading older than 15 minutes no longer counts (the modem is off).
    ChargeGuard s(plain());
    s.noteModem(now, 2.0f);
    now += 16 * MIN;
    s.noteCharger(now, 24.0f);
    CHECK(s.evaluate(now) == ChargeVerdict::Allowed);
    CHECK(!s.modemFresh(now));
}

static void aStaleChargerReadingStopsCharging()
{
    ChargeGuard g(plain());
    g.noteCharger(1000, 24.0f);
    CHECK(g.evaluate(1000 + 15 * MIN) == ChargeVerdict::Allowed);
    CHECK(g.evaluate(1000 + 15 * MIN + 1) == ChargeVerdict::NoReading);
}

static void impossibleValuesAreNoReading()
{
    ChargeGuard g;
    g.noteCharger(1000, -100.0f);
    CHECK(g.evaluate(1000) == ChargeVerdict::NoReading);
    g.noteCharger(1000, 200.0f);
    CHECK(g.evaluate(1000) == ChargeVerdict::NoReading);
}

int main()
{
    noReadingNoCharging();
    warmUpThenTheRoomEstimate();
    aColdStartIsCaughtDuringWarmUp();
    insideTheWindowCharges();
    coldStopsThenHoldsAnHour();
    hotStopsThenHoldsHalfAnHour();
    theModemCountsForColdAndForGreatHeat();
    aStaleChargerReadingStopsCharging();
    impossibleValuesAreNoReading();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("charge_guard_test: all passed\n");
    return EXIT_SUCCESS;
}
