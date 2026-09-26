#include "meshsat/BleWatchdog.h"

#if MESHSAT_IRIDIUM

#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/IridiumPipe.h"

#include <Preferences.h>

#ifndef MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS
#define MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS 3
#endif

// Silence after the last authenticated link before failed links count.
#ifndef MESHSAT_BLE_WATCHDOG_SILENCE_MS
#define MESHSAT_BLE_WATCHDOG_SILENCE_MS (5 * 60 * 1000UL)
#endif

// A node that has never authenticated anyone gets a longer window, measured from its first
// failed link, so first pairings on a fresh node are never interrupted (MESHSAT-1313).
#ifndef MESHSAT_BLE_WATCHDOG_FIRST_SILENCE_MS
#define MESHSAT_BLE_WATCHDOG_FIRST_SILENCE_MS (15 * 60 * 1000UL)
#endif

// 0 disables the idle reboot.
#ifndef MESHSAT_BLE_WATCHDOG_IDLE_HOURS
#define MESHSAT_BLE_WATCHDOG_IDLE_HOURS 0
#endif

static constexpr uint32_t CHECK_INTERVAL_MS = 30 * 1000UL;
static constexpr uint32_t REBOOT_DELAY_MS = 2 * 1000UL;
// A passkey on the screen means a pairing is in progress; the links rule waits this long for it.
static constexpr uint32_t PAIRING_GRACE_MS = 120 * 1000UL;
// Failed links this old are forgotten, so one stale link never blocks the idle path (MESHSAT-1267).
static constexpr uint32_t STALE_FAILURE_MS = 30 * 60 * 1000UL;

static constexpr const char *NVS_NAMESPACE = "meshsat";
static constexpr const char *NVS_REBOOTS = "wdReboots";
static constexpr const char *NVS_REASON = "wdReason";

std::atomic<uint32_t> BleWatchdog::unauthenticatedConnects{0};
std::atomic<uint32_t> BleWatchdog::firstFailedConnectMs{0};
std::atomic<uint32_t> BleWatchdog::lastFailedConnectMs{0};
std::atomic<uint32_t> BleWatchdog::lastPairingMs{0};
std::atomic<uint32_t> BleWatchdog::lastHealthyMs{0};
std::atomic<int32_t> BleWatchdog::openLinks{0};
std::atomic<int32_t> BleWatchdog::authenticatedLinks{0};
std::atomic<bool> BleWatchdog::pairingSeen{false};
std::atomic<bool> BleWatchdog::everHealthy{false};

static BleWatchdog *watchdogInstance = nullptr;

BleWatchdog::BleWatchdog() : concurrency::OSThread("BleWatchdog") {}

void BleWatchdog::begin()
{
    if (watchdogInstance)
        return;
    watchdogInstance = new BleWatchdog();
    LOG_INFO("BLE watchdog armed: reboot after %u unauthenticated links and %us of silence (%us before the first "
             "authentication), idle reboot %uh",
             (unsigned)MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS, (unsigned)(MESHSAT_BLE_WATCHDOG_SILENCE_MS / 1000),
             (unsigned)(MESHSAT_BLE_WATCHDOG_FIRST_SILENCE_MS / 1000), (unsigned)MESHSAT_BLE_WATCHDOG_IDLE_HOURS);

    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, true)) {
        const uint32_t reboots = prefs.getUInt(NVS_REBOOTS, 0);
        if (reboots > 0) {
            const String reason = prefs.getString(NVS_REASON, "");
            LOG_INFO("BLE watchdog: %u reboots so far, last reason: %s", (unsigned)reboots, reason.c_str());
        }
        prefs.end();
    }
}

void BleWatchdog::noteConnect()
{
    openLinks++;
    const uint32_t now = millis();
    if (unauthenticatedConnects.fetch_add(1) == 0)
        firstFailedConnectMs = now;
    lastFailedConnectMs = now;
}

void BleWatchdog::noteDisconnect(bool wasAuthenticated)
{
    if (openLinks.load() > 0)
        openLinks--;
    if (wasAuthenticated && authenticatedLinks.load() > 0)
        authenticatedLinks--;
}

void BleWatchdog::notePairing()
{
    pairingSeen = true;
    lastPairingMs = millis();
}

void BleWatchdog::noteHealthy()
{
    unauthenticatedConnects = 0;
    lastHealthyMs = millis();
    everHealthy = true;
    authenticatedLinks++;
}

bool BleWatchdog::modemBusy() const
{
    IridiumPipe *pipe = IridiumPipe::instance();
    return pipe && pipe->sessionInFlight();
}

void BleWatchdog::persistReboot(const char *why)
{
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false))
        return;
    prefs.putUInt(NVS_REBOOTS, prefs.getUInt(NVS_REBOOTS, 0) + 1);
    prefs.putString(NVS_REASON, why);
    prefs.end();
}

void BleWatchdog::reboot(const char *why)
{
    LOG_ERROR("BLE watchdog: %s, rebooting (links open %d, authenticated %d, unauthenticated %u, ever authenticated %s)", why,
              (int)openLinks.load(), (int)authenticatedLinks.load(), (unsigned)unauthenticatedConnects.load(),
              everHealthy.load() ? "yes" : "no");
    persistReboot(why);
    rebootAtMsec = millis() + REBOOT_DELAY_MS;
}

int32_t BleWatchdog::runOnce()
{
    uint32_t failed = unauthenticatedConnects.load();
    const uint32_t lastHealthy = lastHealthyMs.load();
    const bool healthyOnce = everHealthy.load();

    if (failed > 0 && Throttle::hasElapsed(lastFailedConnectMs.load(), STALE_FAILURE_MS)) {
        LOG_INFO("BLE watchdog: forgetting %u unauthenticated links older than %us", (unsigned)failed,
                 (unsigned)(STALE_FAILURE_MS / 1000));
        unauthenticatedConnects = 0;
        failed = 0;
    }

    const bool pairingInProgress = pairingSeen.load() && !Throttle::hasElapsed(lastPairingMs.load(), PAIRING_GRACE_MS);

    // A stack that serves an authenticated link right now is not wedged, whatever else knocks.
    if (failed >= MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS && !pairingInProgress && authenticatedLinks.load() == 0) {
        const bool silent = healthyOnce
                                ? Throttle::hasElapsed(lastHealthy, MESHSAT_BLE_WATCHDOG_SILENCE_MS)
                                : Throttle::hasElapsed(firstFailedConnectMs.load(), MESHSAT_BLE_WATCHDOG_FIRST_SILENCE_MS);
        if (silent) {
            if (modemBusy()) {
                LOG_WARN("BLE watchdog: %u links without authentication, holding off, satellite session in flight",
                         (unsigned)failed);
            } else {
                reboot(healthyOnce ? "links open but never authenticate" : "links open, none authenticated since boot");
                return CHECK_INTERVAL_MS;
            }
        }
    }

#if MESHSAT_BLE_WATCHDOG_IDLE_HOURS > 0
    const uint32_t idleMs = MESHSAT_BLE_WATCHDOG_IDLE_HOURS * 60UL * 60UL * 1000UL;
    // Idle means: nobody connected right now, and no authenticated link for idleMs (since boot if never).
    const uint32_t idleSince = healthyOnce ? lastHealthy : 0;
    if (openLinks.load() == 0 && Throttle::hasElapsed(0, idleMs) && Throttle::hasElapsed(idleSince, idleMs) && !modemBusy())
        reboot("idle");
#endif

    return CHECK_INTERVAL_MS;
}

#endif
