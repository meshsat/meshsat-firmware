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

// What the pipe has seen pass on the serial line, for the screen and the apps. Written from
// runOnce() only; a reader on another thread gets a copy that may be one field behind.
struct IridiumStats {
    // Modem health, probed with a free AT while nobody owns the modem.
    bool modemAnswered = false;
    uint32_t lastModemOkMs = 0;
    uint32_t healthMisses = 0;
    uint32_t healthPowerCycles = 0;
    // AT+SBDIX sessions seen from any owner.
    uint32_t sessions = 0;
    bool sessionInFlight = false;
    uint32_t sessionStartMs = 0;
    int lastMoStatus = -1;
    uint32_t lastMomsn = 0;
    int lastMtStatus = -1;
    uint32_t lastMtLength = 0;
    uint32_t lastMtQueued = 0;
    uint32_t lastSessionMs = 0;
    // +CSQ as last asked by whoever owned the modem. Never a send gate.
    int lastCsq = -1;
    uint32_t lastCsqMs = 0;
    // An SBDRING arrived and no session has run since.
    bool ringPending = false;
    uint32_t ringMs = 0;
    // Bytes the phone wrote faster than the modem took them, since boot.
    uint32_t phoneBytesDropped = 0;
    // CCCD writes from a link that was not the owner, since boot.
    uint32_t foreignSubscribes = 0;
};

// Binary-safe BLE <-> UART pipe to the RockBLOCK 9603, served next to the Meshtastic service.
// BLE callbacks only fill a stream buffer and flags; UART, notifications and the owner switch run
// in runOnce(). The pipe also reads what passes (SBDIX, CSQ, SBDRING) without ever adding a
// command of its own while a phone owns the modem.
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
    bool sessionInFlight() const { return sessionInFlightFlag.load(); }
    const IridiumStats &stats() const { return stat; }

    // The node's logic may only take an unowned modem, and must release it outside an SBDIX.
    bool tryAcquireForNode();
    void releaseFromNode();

    void onPhoneWrite(const uint8_t *data, size_t length);
    void onPhoneSubscribe(uint16_t connHandle, bool subscribed);
    // A BLE link closed. If it was the one holding the modem, the phone no longer does.
    void onLinkClosed(uint16_t connHandle);

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
    void publishStatus();
    void reportDrops();
    void discardPhoneBytes();
    bool pumpPhoneToModem();
    bool pumpModemToPhone();
    bool drainModem();
    void runHealthCheck();
    size_t notifyChunkLimit() const;

    // Passive line readers on both directions.
    void notePhoneBytes(const uint8_t *data, size_t length);
    void noteModemByte(uint8_t value);
    void onCommandLine();
    void onResponseLine();

    std::atomic<IridiumModemOwner> currentOwner{IridiumModemOwner::None};
    std::atomic<bool> phoneSubscribed{false};
    std::atomic<uint16_t> phoneConnHandle{0};
    std::atomic<uint32_t> lastUnsubscribeMs{0};
    std::atomic<uint32_t> phoneBytesDropped{0};
    std::atomic<uint32_t> foreignSubscribes{0};
    std::atomic<bool> sessionInFlightFlag{false};

    IridiumStats stat;
    uint32_t unownedBytes = 0;
    uint32_t dropsSinceLog = 0;
    uint32_t lastDropLogMs = 0;
    bool holdLogged = false;

    // Modem health while unowned.
    bool healthAwaiting = false;
    uint32_t healthSentMs = 0;
    uint32_t lastHealthMs = 0;
    uint32_t nextHealthDelayMs = 0;
    bool powerCycling = false;
    uint32_t powerCycleOffMs = 0;
    // After a claim while a health probe is out, its reply is dropped instead of reaching the phone.
    bool flushing = false;
    uint32_t flushStartMs = 0;

    static constexpr size_t COMMAND_LINE_BYTES = 64;
    static constexpr size_t RESPONSE_LINE_BYTES = 96;
    char commandLine[COMMAND_LINE_BYTES];
    size_t commandLength = 0;
    char responseLine[RESPONSE_LINE_BYTES];
    size_t responseLength = 0;

    CallbackObserver<IridiumPipe, void *> deepSleepObserver =
        CallbackObserver<IridiumPipe, void *>(this, &IridiumPipe::prepareDeepSleep);
};

#endif
