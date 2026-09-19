#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "Observer.h"
#include "concurrency/OSThread.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

class BLEServer;

enum class IridiumModemOwner : uint8_t {
    None,
    // A phone is subscribed to the Iridium serial service.
    Phone,
    // The node's own Iridium logic (routing without a phone).
    Node,
};

// Binary-safe BLE <-> UART pipe to the RockBLOCK 9603, served next to the Meshtastic service.
// BLE callbacks only fill a stream buffer; UART and notifications run in runOnce().
class IridiumPipe : private concurrency::OSThread
{
  public:
    static constexpr const char *SERVICE_UUID = "b3d305a2-7310-4877-ad12-8e245e71951a";
    static constexpr const char *RX_UUID = "b9e2d4ba-f386-4728-b77a-7df7121db7a9";
    static constexpr const char *TX_UUID = "469354dc-4c89-41ed-b939-d707c7a11f49";
    // Two bytes: contract version, then the IridiumModemOwner value; notified on every owner change.
    static constexpr const char *STATUS_UUID = "69a4064d-78b9-46e5-a30a-1862e553245a";
    static constexpr uint8_t CONTRACT_VERSION = 1;

    static void begin();
    // Called from NimbleBluetooth::setupService(), which re-runs on every BLE re-enable.
    static void setupBleService(BLEServer *server, bool requireEncryption);
    static IridiumPipe *instance();

    IridiumModemOwner owner() const { return currentOwner.load(); }

    // The node's logic may only take an unowned modem, and must release it outside an SBDIX.
    bool tryAcquireForNode();
    void releaseFromNode();

    void onPhoneWrite(const uint8_t *data, size_t length);
    void onPhoneSubscribe(uint16_t connHandle, bool subscribed);

  protected:
    int32_t runOnce() override;

  private:
    IridiumPipe();

    // Switches the modem supply where the board has one (MESHSAT_IRIDIUM_DCDC5_MV); otherwise a no-op.
    void powerModem(bool on);
    void openUart();
    void closeUart();
    int prepareDeepSleep(void *unused);
    void updateOwner();
    void setOwner(IridiumModemOwner owner);
    void discardPhoneBytes();
    bool pumpPhoneToModem();
    bool pumpModemToPhone();
    bool drainUnowned();
    size_t notifyChunkLimit() const;

    std::atomic<IridiumModemOwner> currentOwner{IridiumModemOwner::None};
    std::atomic<bool> phoneSubscribed{false};
    std::atomic<uint16_t> phoneConnHandle{0};
    std::atomic<uint32_t> phoneBytesDropped{0};
    uint32_t unownedBytes = 0;

    CallbackObserver<IridiumPipe, void *> deepSleepObserver =
        CallbackObserver<IridiumPipe, void *>(this, &IridiumPipe::prepareDeepSleep);
};

#endif
