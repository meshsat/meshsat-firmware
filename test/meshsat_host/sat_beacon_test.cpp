// Host test for src/meshsat/SatBeacon.h (MESHSAT-1667). Not a PlatformIO suite (no test_ prefix):
// the CI job test:meshsat-host builds and runs it with g++.
//   g++ -std=c++17 -Wall -Wextra -Werror -I src test/meshsat_host/sat_beacon_test.cpp -o /tmp/t && /tmp/t

#include "meshsat/SatBeacon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using meshsat::BeaconFix;
using meshsat::BeaconKind;
using meshsat::BeaconOffReason;
using meshsat::SatBeacon;

static int failures = 0;
#define CHECK(cond)                                                                                                              \
    do {                                                                                                                         \
        if (!(cond)) {                                                                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
            failures++;                                                                                                          \
        }                                                                                                                        \
    } while (0)

static constexpr uint32_t MIN = 60 * 1000UL;

// Takes whatever is due at `now`, the way the pipe does each run; returns it.
static BeaconKind step(SatBeacon &b, uint32_t now)
{
    const BeaconKind k = b.due(now);
    if (k != BeaconKind::None)
        b.taken(now, k);
    return k;
}

static void offUntilArmed()
{
    SatBeacon b;
    CHECK(!b.on());
    CHECK(b.due(1000) == BeaconKind::None);
    CHECK(b.due(100 * MIN) == BeaconKind::None);
    CHECK(!b.holdsModem(1000, false));
}

static void activatedAtOnceThenEveryTenMinutes()
{
    SatBeacon b;
    const uint32_t t0 = 5000;
    b.onArmed(t0);
    CHECK(b.on());
    CHECK(step(b, t0) == BeaconKind::Activated);
    CHECK(step(b, t0 + 1) == BeaconKind::None);
    CHECK(step(b, t0 + 10 * MIN - 1) == BeaconKind::None);
    CHECK(b.nextInMs(t0 + 9 * MIN) == MIN);
    CHECK(step(b, t0 + 10 * MIN) == BeaconKind::Position);
    CHECK(step(b, t0 + 10 * MIN + 1) == BeaconKind::None);
    // Taken late, the rhythm stays on the switch's clock: the next is at 20 min, not 10 min after the late one.
    CHECK(step(b, t0 + 20 * MIN + 30 * 1000) == BeaconKind::Position);
    CHECK(b.nextInMs(t0 + 20 * MIN + 30 * 1000) == 10 * MIN - 30 * 1000);
}

static void offMessageAfterSomethingWentOut()
{
    SatBeacon b;
    const uint32_t t0 = 1000;
    b.onArmed(t0);
    CHECK(step(b, t0) == BeaconKind::Activated);
    CHECK(!b.onDisarmed(t0 + 30 * MIN, BeaconOffReason::Limit, true));
    CHECK(!b.on());
    CHECK(b.isDraining());
    CHECK(b.lastOffReason() == BeaconOffReason::Limit);
    // The modem is held while the off message is due, even if everything else is settled.
    CHECK(b.holdsModem(t0 + 30 * MIN, true));
    CHECK(step(b, t0 + 30 * MIN) == BeaconKind::Off);
    // Then held until it is out (settled) ...
    CHECK(b.holdsModem(t0 + 31 * MIN, false));
    CHECK(!b.finishDrain(t0 + 31 * MIN, false));
    CHECK(!b.holdsModem(t0 + 31 * MIN, true));
    CHECK(b.finishDrain(t0 + 31 * MIN, true));
    CHECK(!b.isDraining());
    // ... and no position after the switch-off.
    CHECK(step(b, t0 + 40 * MIN) == BeaconKind::None);
}

static void drainEndsAtTheLimitWithoutSky()
{
    SatBeacon b;
    const uint32_t t0 = 1000;
    b.onArmed(t0);
    step(b, t0);
    b.onDisarmed(t0 + MIN, BeaconOffReason::Switch, true);
    step(b, t0 + MIN);
    CHECK(b.holdsModem(t0 + MIN + 3 * MIN - 1, false));
    CHECK(!b.holdsModem(t0 + MIN + 3 * MIN, false));
    CHECK(b.finishDrain(t0 + MIN + 3 * MIN, false));
}

static void aFlickSendsNothing()
{
    SatBeacon b;
    const uint32_t t0 = 1000;
    b.onArmed(t0);
    CHECK(step(b, t0) == BeaconKind::Activated);
    // Off before the activated message reached the modem: the caller drops it, no off message, no hold.
    CHECK(b.onDisarmed(t0 + 2000, BeaconOffReason::Switch, false));
    CHECK(!b.isDraining());
    CHECK(b.due(t0 + 2000) == BeaconKind::None);
    CHECK(!b.holdsModem(t0 + 2000, false));
}

static void cellLowEndsEverything()
{
    SatBeacon b;
    const uint32_t t0 = 1000;
    b.onArmed(t0);
    step(b, t0);
    b.onDisarmed(t0 + MIN, BeaconOffReason::Switch, true);
    b.onCellLow();
    CHECK(!b.isDraining());
    CHECK(b.due(t0 + MIN) == BeaconKind::None);
    CHECK(!b.holdsModem(t0 + MIN, false));
}

static void armingAgainDuringTheDrain()
{
    SatBeacon b;
    const uint32_t t0 = 1000;
    b.onArmed(t0);
    step(b, t0);
    b.onDisarmed(t0 + MIN, BeaconOffReason::Switch, true);
    b.onArmed(t0 + MIN + 5000);
    CHECK(b.on());
    CHECK(!b.isDraining());
    // The pending off message is replaced by a new activation.
    CHECK(step(b, t0 + MIN + 5000) == BeaconKind::Activated);
    CHECK(step(b, t0 + MIN + 5001) == BeaconKind::None);
}

static void disarmWhileOffIsNoOp()
{
    SatBeacon b;
    CHECK(!b.onDisarmed(1000, BeaconOffReason::Switch, false));
    CHECK(!b.isDraining());
}

static void texts()
{
    char out[200];
    BeaconFix fix;
    fix.have = true;
    fix.latitudeI = 521601234; // 52.1601234
    fix.longitudeI = 44970456; // 4.4970456
    fix.ageS = 30;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Activated, BeaconOffReason::Switch, fix);
    CHECK(std::strcmp(out, "@beacon BEACON ACTIVATED https://maps.google.com/?q=52.16012,4.49705 GPS fix now") == 0);

    fix.ageS = 125;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Position, BeaconOffReason::Switch, fix);
    CHECK(std::strcmp(out, "@beacon BEACON https://maps.google.com/?q=52.16012,4.49705 GPS fix 2 min old") == 0);

    fix.ageS = 9 * 60;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Off, BeaconOffReason::Limit, fix);
    CHECK(std::strcmp(out, "@beacon BEACON OFF: 30 min limit https://maps.google.com/?q=52.16012,4.49705 GPS fix 9 min old") ==
          0);

    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Off, BeaconOffReason::Switch, fix);
    CHECK(std::strstr(out, "BEACON OFF: switched off https://") != nullptr);
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Off, BeaconOffReason::Idle, fix);
    CHECK(std::strstr(out, "BEACON OFF: 15 min idle https://") != nullptr);

    fix.ageS = 3 * 3600;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Position, BeaconOffReason::Switch, fix);
    CHECK(std::strstr(out, " GPS fix 3 h old") != nullptr);
    fix.ageS = 3 * 86400;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Position, BeaconOffReason::Switch, fix);
    CHECK(std::strstr(out, " GPS fix 3 d old") != nullptr);

    fix.manual = true;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Position, BeaconOffReason::Switch, fix);
    CHECK(std::strstr(out, " position set by hand") != nullptr);

    // Southern and western hemispheres, and values under one degree.
    fix.manual = false;
    fix.ageS = 0;
    fix.latitudeI = -338688000; // -33.8688
    fix.longitudeI = -5000;     // -0.0005
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Position, BeaconOffReason::Switch, fix);
    CHECK(std::strstr(out, "?q=-33.86880,-0.00050 ") != nullptr);
    fix.latitudeI = 49;  // rounds down to 0.00000
    fix.longitudeI = 51; // rounds up to 0.00001
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Position, BeaconOffReason::Switch, fix);
    CHECK(std::strstr(out, "?q=0.00000,0.00001 ") != nullptr);

    BeaconFix none;
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Activated, BeaconOffReason::Switch, none);
    CHECK(std::strcmp(out, "@beacon BEACON ACTIVATED, no GPS fix yet") == 0);
    meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Off, BeaconOffReason::Switch, none);
    CHECK(std::strcmp(out, "@beacon BEACON OFF: switched off, no GPS fix yet") == 0);

    // The longest message fits the node's 240-byte satellite text and one 160-character SMS with the Hub's label.
    fix.latitudeI = -899999999;
    fix.longitudeI = -1799999999;
    fix.ageS = 99 * 60;
    const int n = meshsat::formatBeaconText(out, sizeof(out), "beacon", BeaconKind::Off, BeaconOffReason::Switch, fix);
    CHECK(n > 0 && n < 240);
    const int smsBody = n - static_cast<int>(std::strlen("@beacon "));
    CHECK(smsBody + 34 <= 160);
}

int main()
{
    offUntilArmed();
    activatedAtOnceThenEveryTenMinutes();
    offMessageAfterSomethingWentOut();
    drainEndsAtTheLimitWithoutSky();
    aFlickSendsNothing();
    cellLowEndsEverything();
    armingAgainDuringTheDrain();
    disarmWhileOffIsNoOp();
    texts();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("sat_beacon_test: all passed\n");
    return EXIT_SUCCESS;
}
