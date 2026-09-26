#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "concurrency/OSThread.h"

#include <atomic>
#include <cstdint>

// Reboots a node whose BLE stack accepts links but never authenticates one, and optionally when
// it has been idle for MESHSAT_BLE_WATCHDOG_IDLE_HOURS. Never while an Iridium session is in flight.
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

  protected:
    int32_t runOnce() override;

  private:
    BleWatchdog();

    bool modemBusy() const;
    void reboot(const char *why);
    static void persistReboot(const char *why);

    static std::atomic<uint32_t> unauthenticatedConnects;
    static std::atomic<uint32_t> firstFailedConnectMs;
    static std::atomic<uint32_t> lastFailedConnectMs;
    static std::atomic<uint32_t> lastPairingMs;
    static std::atomic<uint32_t> lastHealthyMs;
    static std::atomic<int32_t> openLinks;
    static std::atomic<int32_t> authenticatedLinks;
    static std::atomic<bool> pairingSeen;
    static std::atomic<bool> everHealthy;
};

#endif
