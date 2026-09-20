#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "concurrency/OSThread.h"

#include <atomic>
#include <cstdint>

// Reboots a node whose BLE stack accepts links but never authenticates one, and optionally when
// it has been idle for MESHSAT_BLE_WATCHDOG_IDLE_HOURS. Never while the Iridium modem is owned.
class BleWatchdog : private concurrency::OSThread
{
  public:
    static void begin();
    static void noteConnect();
    static void noteHealthy();

  protected:
    int32_t runOnce() override;

  private:
    BleWatchdog();

    bool modemBusy() const;
    void reboot(const char *why);

    static std::atomic<uint32_t> unauthenticatedConnects;
    static std::atomic<uint32_t> lastHealthyMs;
};

#endif
