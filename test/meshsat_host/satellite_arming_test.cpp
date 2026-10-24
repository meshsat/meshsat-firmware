// Host test for src/meshsat/SatelliteArming.h (MESHSAT-1667). Not a PlatformIO suite (no test_ prefix):
// the CI job test:meshsat-host builds and runs it with g++.
//   g++ -std=c++17 -Wall -Wextra -Werror -I src test/meshsat_host/satellite_arming_test.cpp -o /tmp/t && /tmp/t

#include "meshsat/SatelliteArming.h"

#include <cstdio>
#include <cstdlib>

using meshsat::ArmEvent;
using meshsat::SatelliteArming;
using meshsat::SwitchKind;

static int failures = 0;
#define CHECK(cond)                                                                                                              \
    do {                                                                                                                         \
        if (!(cond)) {                                                                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
            failures++;                                                                                                          \
        }                                                                                                                        \
    } while (0)

// Holds the pin at one level from `from` to `to` in 10 ms samples; returns the first event other than None.
static ArmEvent hold(SatelliteArming &a, uint32_t &now, uint32_t forMs, bool pressed, bool canArm = true)
{
    ArmEvent first = ArmEvent::None;
    const uint32_t end = now + forMs;
    for (; now < end; now += 10) {
        const ArmEvent ev = a.update(now, pressed, canArm);
        if (first == ArmEvent::None && ev != ArmEvent::None)
            first = ev;
    }
    return first;
}

static void neverPressedStaysInactive()
{
    SatelliteArming a;
    uint32_t now = 1000;
    a.restore(now, false, SwitchKind::Unknown, false);
    CHECK(hold(a, now, 60 * 60 * 1000UL, false) == ArmEvent::None);
    CHECK(!a.active());
    CHECK(!a.armed());
    CHECK(!a.takeDirty());
}

static void glitchShorterThanDebounceIsIgnored()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, false, SwitchKind::Unknown, false);
    CHECK(hold(a, now, 30, true) == ArmEvent::None);
    CHECK(hold(a, now, 1000, false) == ArmEvent::None);
    CHECK(!a.active());
}

static void firstPressActivatesAndArms()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, false, SwitchKind::Unknown, false);
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
    CHECK(a.active());
    CHECK(a.armed());
    CHECK(a.takeDirty());
    uint32_t held = 0;
    CHECK(a.takeEdge(&held) == SatelliteArming::Edge::Pressed);
}

static void momentaryToggles()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, true, SwitchKind::Unknown, false);
    CHECK(hold(a, now, 300, true) == ArmEvent::Armed);
    CHECK(hold(a, now, 2000, false) == ArmEvent::None); // release: still armed
    CHECK(a.armed());
    CHECK(hold(a, now, 300, true) == ArmEvent::DisarmedByButton);
    CHECK(hold(a, now, 2000, false) == ArmEvent::None);
    CHECK(!a.armed());
    CHECK(a.kind() == SwitchKind::Unknown);
}

static void latchingIsDetectedAndReleaseDisarms()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, true, SwitchKind::Unknown, false);
    CHECK(hold(a, now, 100, true) == ArmEvent::Armed);
    CHECK(hold(a, now, 10 * 1000UL, true) == ArmEvent::LatchingDetected);
    CHECK(a.kind() == SwitchKind::Latching);
    CHECK(a.takeDirty());
    CHECK(a.armed());
    CHECK(hold(a, now, 200, false) == ArmEvent::DisarmedByButton);
    CHECK(!a.armed());
    // Next press in arms, the release disarms, however short.
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
    CHECK(hold(a, now, 200, false) == ArmEvent::DisarmedByButton);
}

static void idleTimeoutDisarmsAndHeldLatchDoesNotRearm()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, true, SwitchKind::Latching, false);
    CHECK(hold(a, now, 100, true) == ArmEvent::Armed);
    CHECK(hold(a, now, 15 * 60 * 1000UL, true) == ArmEvent::DisarmedIdle);
    CHECK(!a.armed());
    // Left in: nothing arms it again.
    CHECK(hold(a, now, 60 * 60 * 1000UL, true) == ArmEvent::None);
    CHECK(!a.armed());
    // Out (no event, already disarmed), then in again: armed.
    CHECK(hold(a, now, 200, false) == ArmEvent::None);
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
}

static void trafficKeepsItArmedUntilTheLimit()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, true, SwitchKind::Latching, false);
    CHECK(hold(a, now, 100, true) == ArmEvent::Armed);
    // A message every 10 min: never idle, but 30 min after arming it disarms.
    ArmEvent ev = ArmEvent::None;
    for (int i = 0; i < 4 && ev == ArmEvent::None; ++i) {
        ev = hold(a, now, 10 * 60 * 1000UL, true);
        a.noteTraffic(now);
    }
    CHECK(ev == ArmEvent::DisarmedLimit);
    CHECK(!a.armed());
}

static void momentaryIdleThenPressArms()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, true, SwitchKind::Unknown, false);
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
    CHECK(hold(a, now, 15 * 60 * 1000UL, false) == ArmEvent::DisarmedIdle);
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
}

static void resetNeverArmsEvenWithTheLatchIn()
{
    SatelliteArming a;
    uint32_t now = 5000;
    a.restore(now, true, SwitchKind::Latching, true);
    CHECK(!a.armed());
    CHECK(hold(a, now, 60 * 1000UL, true) == ArmEvent::None);
    CHECK(!a.armed());
    CHECK(hold(a, now, 200, false) == ArmEvent::None);
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
}

static void lowCellRefusesAndRecoveryDoesNotArm()
{
    SatelliteArming a;
    uint32_t now = 0;
    a.restore(now, true, SwitchKind::Latching, false);
    CHECK(hold(a, now, 200, true, false) == ArmEvent::RefusedCellLow);
    CHECK(!a.armed());
    // The cell recovers with the switch still in: still not armed.
    CHECK(hold(a, now, 60 * 1000UL, true, true) == ArmEvent::None);
    CHECK(!a.armed());
    CHECK(hold(a, now, 200, false) == ArmEvent::None);
    CHECK(hold(a, now, 200, true) == ArmEvent::Armed);
    CHECK(a.disarmForCell() == ArmEvent::DisarmedCellLow);
    CHECK(!a.armed());
    CHECK(a.disarmForCell() == ArmEvent::None);
}

static void millisWrapIsHarmless()
{
    SatelliteArming a;
    uint32_t now = 0xFFFFFF00u;
    a.restore(now, true, SwitchKind::Latching, false);
    CHECK(hold(a, now, 100, true) == ArmEvent::Armed);
    CHECK(hold(a, now, 60 * 1000UL, true) == ArmEvent::None);
    CHECK(a.armed());
}

int main()
{
    neverPressedStaysInactive();
    glitchShorterThanDebounceIsIgnored();
    firstPressActivatesAndArms();
    momentaryToggles();
    latchingIsDetectedAndReleaseDisarms();
    idleTimeoutDisarmsAndHeldLatchDoesNotRearm();
    trafficKeepsItArmedUntilTheLimit();
    momentaryIdleThenPressArms();
    resetNeverArmsEvenWithTheLatchIn();
    lowCellRefusesAndRecoveryDoesNotArm();
    millisWrapIsHarmless();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("satellite_arming_test: all checks passed\n");
    return EXIT_SUCCESS;
}
