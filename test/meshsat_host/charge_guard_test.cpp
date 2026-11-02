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

static void insideTheWindowCharges()
{
    ChargeGuard g;
    g.noteCharger(1000, 22.0f);
    CHECK(g.evaluate(1000) == ChargeVerdict::Allowed);
    g.noteCharger(2000, 10.0f);
    CHECK(g.evaluate(2000) == ChargeVerdict::Allowed);
    g.noteCharger(3000, 35.0f);
    CHECK(g.evaluate(3000) == ChargeVerdict::Allowed);
}

static void coldStopsThenHoldsAnHour()
{
    ChargeGuard g;
    uint32_t now = 1000;
    g.noteCharger(now, 4.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooCold);
    // The board warms within minutes; the cell does not: an hour from the last cold reading.
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
    ChargeGuard g;
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

static void theModemCountsWhileFresh()
{
    ChargeGuard g;
    uint32_t now = 1000;
    g.noteCharger(now, 25.0f);
    g.noteModem(now, 37.0f);
    CHECK(g.evaluate(now) == ChargeVerdict::TooHot);
    // A cold modem board stops charging too.
    ChargeGuard c;
    c.noteCharger(now, 12.0f);
    c.noteModem(now, 8.0f);
    CHECK(c.evaluate(now) == ChargeVerdict::TooCold);
    // A modem reading older than 15 minutes no longer counts (the modem is off).
    ChargeGuard s;
    s.noteModem(now, 50.0f);
    now += 16 * MIN;
    s.noteCharger(now, 24.0f);
    CHECK(s.evaluate(now) == ChargeVerdict::Allowed);
    CHECK(!s.modemFresh(now));
}

static void aStaleChargerReadingStopsCharging()
{
    ChargeGuard g;
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
    insideTheWindowCharges();
    coldStopsThenHoldsAnHour();
    hotStopsThenHoldsHalfAnHour();
    theModemCountsWhileFresh();
    aStaleChargerReadingStopsCharging();
    impossibleValuesAreNoReading();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("charge_guard_test: all passed\n");
    return EXIT_SUCCESS;
}
