#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "concurrency/OSThread.h"

#include <atomic>
#include <cstdint>

// Reboots a node whose BLE stack accepts links but never authenticates one, restarts advertising
// (then reboots) when it stopped with no link open, and optionally reboots a node idle for
// MESHSAT_BLE_WATCHDOG_IDLE_HOURS. Never while an Iridium session is in flight.
class BleWatchdog : private concurrency::OSThread
{
  public:
    static void begin();
    static void noteConnect();
    // wasAuthenticated: the closing link had an encrypted, authenticated session.
    static void noteDisconnect(bool wasAuthenticated);
    // A passkey is being shown: the link is pairing, not failing.
    static void notePairing();
    static void noteHealthy();
    // Watchdog reboots so far, from NVS at boot plus this session's.
    static uint32_t rebootCount() { return reboots.load(); }

  protected:
    int32_t runOnce() override;

  private:
    BleWatchdog();

    bool modemBusy() const;
    void reboot(const char *why);
    static void persistReboot(const char *why);

    // Checks in a row that found Bluetooth up, no link open and nothing advertised.
    uint8_t darkChecks = 0;

    static std::atomic<uint32_t> unauthenticatedConnects;
    static std::atomic<uint32_t> firstFailedConnectMs;
    static std::atomic<uint32_t> lastFailedConnectMs;
    static std::atomic<uint32_t> lastPairingMs;
    static std::atomic<uint32_t> lastHealthyMs;
    static std::atomic<int32_t> openLinks;
    static std::atomic<int32_t> authenticatedLinks;
    static std::atomic<bool> pairingSeen;
    static std::atomic<bool> everHealthy;
    static std::atomic<uint32_t> reboots;
};

#endif
