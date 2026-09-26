#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

#include <cstddef>
#include <cstdint>

// The mesh channel the node carries over Iridium on its own (owner 02 of the BLE contract).
#ifndef MESHSAT_IRIDIUM_CHANNEL_NAME
#define MESHSAT_IRIDIUM_CHANNEL_NAME "i9603"
#endif

// Unattended sessions per day that reached the gateway (status 32 is free); a phone's sessions do not count.
#ifndef MESHSAT_IRIDIUM_DAILY_SESSIONS
#define MESHSAT_IRIDIUM_DAILY_SESSIONS 10
#endif

// Carries texts on the MESHSAT_IRIDIUM_CHANNEL_NAME channel over the RockBLOCK when no phone
// holds the modem: texts from the mesh go out as SBD messages, fetched SBD messages come back as
// broadcasts on the same channel from the node. A state machine advanced from runOnce(), with
// the Bridge's rules: init sequence, SBDSX before SBDIX, 10 s between attempts, 3 min after
// 32/36, never a CSQ gate, one session at a time, hand-over to a phone only between commands.
class IridiumModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    IridiumModule();

  protected:
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    enum class State : uint8_t {
        // A phone has the modem, or nobody does and the node has not taken it.
        Off,
        // Running the init sequence, one command at a time.
        Init,
        Idle,
        // One AT command out, waiting for its final line.
        Command,
        // AT+SBDWB: waiting for READY, then the payload's status digit.
        WriteReady,
        WriteStatus,
        // AT+SBDIX out, waiting for the +SBDIX: line.
        Session,
        // AT+SBDRB out, reading the binary frame.
        ReadMt,
    };

    enum class Command : uint8_t { None, InitStep, Sbdsx, ClearMo, ClearMt, Sbdix };

    struct Outbound {
        uint8_t bytes[340];
        uint16_t length;
        uint32_t queuedMs;
        uint32_t from;
    };

    static constexpr size_t QUEUE_SLOTS = 4;
    static constexpr size_t LINE_BYTES = 128;
    static constexpr size_t MT_FRAME_BYTES = 270 + 4;

    bool isIridiumChannel(uint8_t channelIndex) const;
    int8_t iridiumChannelIndex() const;
    void enqueue(const meshtastic_MeshPacket &mp);
    void popOutbound();

    void takeOrReleaseModem();
    void release(const char *why);
    void startInit();
    void sendCommand(const char *command, Command what, uint32_t timeoutMs);
    void startWrite();
    void startSession();
    void startReadMt();
    void pumpLines();
    void onLine(const char *line);
    void onCommandDone(bool ok);
    void onSessionResult(const char *line);
    void onSbdsx(const char *line);
    void onMtFrame();
    void pumpBinary();
    bool canOpenSession();
    void countSession();
    void loadDailyCount();
    void saveDailyCount();
    uint32_t today() const;

    // Payload on the wire, kept apart so the format can follow the Hub's.
    size_t encodeMo(const meshtastic_MeshPacket &mp, uint8_t *out, size_t capacity);
    void deliverMt(const uint8_t *data, size_t length);

    State state = State::Off;
    Command command = Command::None;
    uint32_t commandSentMs = 0;
    uint32_t commandTimeoutMs = 0;
    uint8_t initStep = 0;
    uint8_t initRetries = 0;

    Outbound queue[QUEUE_SLOTS];
    size_t queueHead = 0;
    size_t queueCount = 0;

    char line[LINE_BYTES];
    size_t lineLength = 0;
    uint8_t frame[MT_FRAME_BYTES];
    size_t frameLength = 0;
    uint16_t frameExpected = 0;

    bool moWritten = false;
    // A sent MO stays in the modem until cleared, and would go again with the next session.
    bool needClearMo = false;
    // SBDSX, a ring or a queued count says the gateway holds a message: open a session.
    bool mtWaiting = false;
    // The last session put a message in the modem's MT buffer: read it.
    bool mtInBuffer = false;
    uint32_t mtQueued = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t holdUntilMs = 0;
    bool holding = false;
    uint32_t lastSbdsxMs = 0;
    uint32_t dailyDay = 0;
    uint32_t dailyCount = 0;
    bool dailyLoaded = false;
    uint32_t sessionsAsNode = 0;
    uint32_t sentAsNode = 0;
    uint32_t receivedAsNode = 0;
};

extern IridiumModule *iridiumModule;

#endif
