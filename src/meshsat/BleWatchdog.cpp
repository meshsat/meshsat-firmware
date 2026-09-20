#include "meshsat/BleWatchdog.h"

#if MESHSAT_IRIDIUM

#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/IridiumPipe.h"

#ifndef MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS
#define MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS 3
#endif

#ifndef MESHSAT_BLE_WATCHDOG_SILENCE_MS
#define MESHSAT_BLE_WATCHDOG_SILENCE_MS (5 * 60 * 1000UL)
#endif

// 0 disables the idle reboot.
#ifndef MESHSAT_BLE_WATCHDOG_IDLE_HOURS
#define MESHSAT_BLE_WATCHDOG_IDLE_HOURS 0
#endif

static constexpr uint32_t CHECK_INTERVAL_MS = 30 * 1000UL;
static constexpr uint32_t REBOOT_DELAY_MS = 2 * 1000UL;

std::atomic<uint32_t> BleWatchdog::unauthenticatedConnects{0};
std::atomic<uint32_t> BleWatchdog::lastHealthyMs{0};

static BleWatchdog *watchdogInstance = nullptr;

BleWatchdog::BleWatchdog() : concurrency::OSThread("BleWatchdog") {}

void BleWatchdog::begin()
{
    if (watchdogInstance)
        return;
    lastHealthyMs = millis();
    watchdogInstance = new BleWatchdog();
    LOG_INFO("BLE watchdog armed: reboot after %u unauthenticated links and %us of silence, idle reboot %uh",
             (unsigned)MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS, (unsigned)(MESHSAT_BLE_WATCHDOG_SILENCE_MS / 1000),
             (unsigned)MESHSAT_BLE_WATCHDOG_IDLE_HOURS);
}

void BleWatchdog::noteConnect()
{
    unauthenticatedConnects++;
}

void BleWatchdog::noteHealthy()
{
    unauthenticatedConnects = 0;
    lastHealthyMs = millis();
}

bool BleWatchdog::modemBusy() const
{
    IridiumPipe *pipe = IridiumPipe::instance();
    return pipe && pipe->owner() != IridiumModemOwner::None;
}

void BleWatchdog::reboot(const char *why)
{
    LOG_ERROR("BLE watchdog: %s, rebooting", why);
    rebootAtMsec = millis() + REBOOT_DELAY_MS;
}

int32_t BleWatchdog::runOnce()
{
    const uint32_t failed = unauthenticatedConnects.load();
    const uint32_t lastHealthy = lastHealthyMs.load();

    if (failed >= MESHSAT_BLE_WATCHDOG_FAILED_CONNECTS && Throttle::hasElapsed(lastHealthy, MESHSAT_BLE_WATCHDOG_SILENCE_MS)) {
        if (modemBusy()) {
            LOG_WARN("BLE watchdog: %u links without authentication, holding off, modem in use", failed);
        } else {
            reboot("links open but never authenticate");
            return CHECK_INTERVAL_MS;
        }
    }

#if MESHSAT_BLE_WATCHDOG_IDLE_HOURS > 0
    const uint32_t idleMs = MESHSAT_BLE_WATCHDOG_IDLE_HOURS * 60UL * 60UL * 1000UL;
    if (Throttle::hasElapsed(0, idleMs) && Throttle::hasElapsed(lastHealthy, idleMs) && failed == 0 && !modemBusy())
        reboot("idle");
#endif

    return CHECK_INTERVAL_MS;
}

#endif
