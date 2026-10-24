#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "Observer.h"
#include "concurrency/OSThread.h"
#include "meshsat/SatelliteArming.h"

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

// The modem's supply and readiness, owned by the pipe in the 9704 build (the 9603 build is always Running).
enum class IridiumModemPower : uint8_t {
    // Supply off: the cell is under the off-threshold, or the minimum off interval runs.
    Inhibited,
    // Supply on, boot not confirmed: inputs stay high-impedance until the guard or I_BTD says booted.
    Starting,
    // Supply on and the UART open; a JSPR reply has been seen, or a client is on the pipe.
    Running,
    // Supply on, no reply to any probe: latched, never power-cycled, until a client gets an answer or a reboot.
    Fault,
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
    // When a phone last gave the modem back (0 = never); the node's own routing waits after it.
    uint32_t lastPhoneReleaseMs = 0;
    // Messages the 9704 accepted (JSPR 200 messageOriginate) and has not reported final (299
    // messageOriginateStatus), whoever put them in; kept across a software reset. 0 after a power-on,
    // when the node cannot know what the modem still holds.
    uint32_t moInModem = 0;
    // The panel button's last arming event (meshsat::ArmEvent) and when, for the screen (MESHSAT-1667).
    uint8_t armEvent = 0;
    uint32_t armEventMs = 0;
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
    // Contract v2: version, owner, flags, signal; notified when the owner or the flags change.
    // A v1 client reads the first two bytes and sees the same owner byte as before.
    static constexpr const char *STATUS_UUID = "69a4064d-78b9-46e5-a30a-1862e553245a";
// STATUS at version 2 only once the apps that read it are INSTALLED, not merely committed: a
// version 1 app on the phone ignored the 4-byte notification and never got the modem (27 Sep 2026).
#ifndef MESHSAT_STATUS_V2
#define MESHSAT_STATUS_V2 0
#endif
#if MESHSAT_STATUS_V2
    static constexpr uint8_t CONTRACT_VERSION = 2;
    static constexpr size_t STATUS_BYTES = 4;
#else
    static constexpr uint8_t CONTRACT_VERSION = 1;
    static constexpr size_t STATUS_BYTES = 2;
#endif
    // Contract v2 (MESHSAT-1378), additive: node health for the apps, and pass windows from the phone.
    static constexpr const char *STATS_UUID = "9c22cf07-2256-4fc2-b6ee-ab0ceb12198d";
    static constexpr const char *PASS_UUID = "5c1000e8-f411-4f3d-a4c9-5ee0610a8e66";
    // Additive (MESHSAT-1671): the owner's switches for the node, written by the phone as pairs
    // [switch][value]: switch 0x01 the status LED, 0x02 the screen; value 0x00 on, 0x01 off.
    // Kept in NVS; STATUS flags bits 5 (LED off) and 6 (screen off) report them.
    static constexpr const char *CONTROL_UUID = "d6a8a903-6af1-4530-ab3d-49c39b636e04";
    static constexpr uint8_t CONTROL_LED = 0x01;
    static constexpr uint8_t CONTROL_SCREEN = 0x02;
    static constexpr uint8_t STATS_VERSION = 2;
    static constexpr size_t STATS_BYTES = 52;
    static constexpr size_t MAX_PASS_WINDOWS = 8;

    struct PassWindow {
        uint32_t startEpochS;
        uint16_t durationS;
        uint8_t maxElevationDeg;
    };

    // Bumped whenever the service's attribute table changes (a characteristic added or removed).
    // A bonded phone keeps a cached copy of the table; for the boots after a change the node
    // indicates Service Changed to every authenticated peer so the phone discovers again.
    static constexpr uint32_t GATT_TABLE_VERSION = 3;
    static constexpr uint32_t SERVICE_CHANGED_BOOTS = 20;
    // Called from NimbleBluetooth after an authenticated link comes up.
    void onAuthenticated(uint16_t connHandle);

    // The pass list the phone last wrote; empty and "never written" until then.
    bool passListWritten() const { return passListEverWritten; }
    size_t passWindowCount() const { return passCount; }
    const PassWindow &passWindow(size_t index) const { return passWindows[index]; }
    void onPassWrite(const uint8_t *data, size_t length);
    void onControlWrite(const uint8_t *data, size_t length);
    // Signal bars from a JSPR constellationState event, for the screen and the apps; never a send gate.
    void noteSignal(int bars);

    static void begin();
    // Called from NimbleBluetooth::setupService(), which re-runs on every BLE re-enable.
    static void setupBleService(BLEServer *server, bool requireEncryption);
    static IridiumPipe *instance();

    IridiumModemOwner owner() const { return currentOwner.load(); }
    IridiumModemPower modemPower() const;
    // The panel button (MESHSAT-1667): active once this unit's button was ever pressed; then the modem runs only
    // while armed. Without a button, or before its first press, the modem runs as it always did.
    bool panelButtonActive() const;
    bool satelliteArmed() const;
    bool sessionInFlight() const { return sessionInFlightFlag.load(); }
    const IridiumStats &stats() const { return stat; }

    // The node's logic may only take an unowned modem, and must release it outside an SBDIX.
    bool tryAcquireForNode();
    void releaseFromNode();
    // True while a phone is subscribed to TX: the node's logic must hand the modem over.
    bool phoneWantsModem() const { return phoneSubscribed.load(); }
    // Serial access for the node's own logic; both are no-ops unless the node owns the modem.
    // What goes by feeds the same readers as the phone's traffic (sessions, CSQ, ring).
    size_t nodeWrite(const uint8_t *data, size_t length);
    int nodeAvailable();
    int nodeRead();

    void onPhoneWrite(const uint8_t *data, size_t length);
    void onPhoneSubscribe(uint16_t connHandle, bool subscribed);
    // A BLE link closed. If it was the one holding the modem, the phone no longer does.
    void onLinkClosed(uint16_t connHandle);

  protected:
    int32_t runOnce() override;

  private:
    IridiumPipe();

    // Starts or stops the modem: through its enable input where I_EN is wired (MESHSAT_IRIDIUM_ENABLE_PIN, the
    // cell on V_BATT), through the supply rail where the board has one (MESHSAT_IRIDIUM_DCDC5_MV); otherwise a no-op.
    void powerModem(bool on);
    void openUart();
    void closeUart();
    int prepareDeepSleep(void *unused);
    void updateOwner();
    void setOwner(IridiumModemOwner owner);
    void publishStatus();
    uint8_t statusFlags() const;
    void publishStats();
    void applyPendingPassList();
    void applyPendingControl();
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
    bool announceServiceChanged = false;

    // STATS: last published image, to notify only on change and at most every 2 s.
    uint8_t lastStats[STATS_BYTES] = {0};
    uint32_t lastStatsMs = 0;
    uint8_t lastStatus[STATUS_BYTES] = {0};

    // PASS: the BLE thread fills the pending copy, runOnce moves it into the live list.
    PassWindow passWindows[MAX_PASS_WINDOWS];
    size_t passCount = 0;
    bool passListEverWritten = false;
    PassWindow pendingPass[MAX_PASS_WINDOWS];
    size_t pendingPassCount = 0;
    std::atomic<bool> pendingPassReady{false};
    // CONTROL: the BLE thread stores the latest wish per switch (0xFF = none), runOnce acts on it.
    std::atomic<uint8_t> pendingLed{0xFF};
    std::atomic<uint8_t> pendingScreen{0xFF};
    uint32_t dropsSinceLog = 0;
    uint32_t lastDropLogMs = 0;
    bool holdLogged = false;

    bool uartOpen = false;
#if MESHSAT_IRIDIUM_JSPR
    // The supply state machine (MESHSAT-1507): cell eligibility before the rail, a guard from the enable,
    // probes without power cycles, a latched fault.
    IridiumModemPower modemPowerState = IridiumModemPower::Inhibited;
    uint32_t modemPoweredMs = 0;
    uint32_t modemOffMs = 0;
    uint32_t lastCellSampleMs = 0;
    uint8_t cellOkSamples = 0;
    uint8_t cellLowSamples = 0;
    bool probeAwaiting = false;
    uint32_t probeSentMs = 0;
    uint32_t lastProbeMs = 0;
    uint32_t probeIntervalMs = 0;
    uint32_t probeMisses = 0;
    void sampleCell(uint32_t now);
    void runModemPower(uint32_t now);
    void startModem(uint32_t now, bool railAlreadyOn);
    void stopModem(const char *why);
    void sendProbe(uint32_t now);
#ifdef MESHSAT_IRIDIUM_BOOTED_PIN
    // The cell asked for a stop while the start was unconfirmed: the stop waits for I_BTD high first
    // (Ground Control: once I_EN is high it stays high until I_BTD has gone high).
    bool stopAfterBoot = false;
#endif
#if defined(MESHSAT_PANEL_BUTTON_PIN) && defined(MESHSAT_IRIDIUM_ENABLE_PIN)
    meshsat::SatelliteArming arming;
    void restoreArming(uint32_t now);
    void runArming(uint32_t now);
    void noteArmEvent(meshsat::ArmEvent ev, uint32_t now);
    bool cellAllowsArming() const;
#endif
#ifdef MESHSAT_IRIDIUM_ENABLE_PIN
    // I_EN is low and I_BTD has not confirmed the shutdown yet: I_EN stays low until it does.
    bool shutdownPending = false;
    uint32_t shutdownCommandedMs = 0;
    uint32_t lastShutdownWarnMs = 0;
    // The shutdown check at boot: resumes the phase the firmware was in before the reset.
    void resumeEnableAtBoot(uint32_t now);
#endif
#endif

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
    // Bytes of an AT+SBDWB payload still to come: binary with no terminator, never part of a command line.
    size_t payloadSkip = 0;
    char responseLine[RESPONSE_LINE_BYTES];
    size_t responseLength = 0;

    CallbackObserver<IridiumPipe, void *> deepSleepObserver =
        CallbackObserver<IridiumPipe, void *>(this, &IridiumPipe::prepareDeepSleep);
};

#endif
