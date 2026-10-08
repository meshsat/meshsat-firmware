#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM && MESHSAT_IRIDIUM_JSPR

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

#include <cstddef>
#include <cstdint>

// The mesh channel the node carries over Iridium on its own (owner 02 of the BLE contract).
#ifndef MESHSAT_IRIDIUM_CHANNEL_NAME
#define MESHSAT_IRIDIUM_CHANNEL_NAME "i9603"
#endif
// The IMT topic the node sends on and reads from; 244 is RAW, provisioned on every RockBLOCK 9704.
#ifndef MESHSAT_IRIDIUM_IMT_TOPIC
#define MESHSAT_IRIDIUM_IMT_TOPIC 244
#endif

// Carries texts on the MESHSAT_IRIDIUM_CHANNEL_NAME channel over a RockBLOCK 9704 (Iridium
// Messaging Transport, JSPR) when no client holds the modem: texts from the mesh go out as IMT
// messages on the topic, IMT messages the modem pushes come back as broadcasts on the same
// channel. A state machine advanced from runOnce(). The modem transmits on its own once it sees
// the constellation, so there are no sessions, credits or pass windows here.
class IridiumImtModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    IridiumImtModule();

    // Counters for the STATS characteristic (contract v2).
    uint32_t sentAsNodeCount() const { return sentAsNode; }
    uint32_t receivedAsNodeCount() const { return receivedAsNode; }
    uint32_t failedAsNodeCount() const { return failedAsNode; }

  protected:
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    enum class State : uint8_t {
        // A client has the modem, or nobody does and the node has not taken it.
        Off,
        // Bringing the modem up: API version, SIM, operational state, one request at a time.
        Init,
        Idle,
        // messageOriginate out, waiting for the modem to accept it.
        Originating,
        // Accepted; waiting for the modem to ask for the payload and take it.
        Sending,
    };

    enum class Request : uint8_t {
        None,
        GetApiVersion,
        PutApiVersion,
        GetSimConfig,
        PutSimConfig,
        GetOperationalState,
        PutOperationalState,
        Originate,
        Segment,
    };

    // A mesh text is at most 233 bytes; two checksum bytes follow it on the wire.
    static constexpr size_t MO_MAX_BYTES = 240;
    static constexpr size_t QUEUE_SLOTS = 4;
    // One JSPR line: a segment carries up to 1446 bytes as base64, plus the JSON around it.
    static constexpr size_t LINE_BYTES = 2304;
    static constexpr size_t MT_MAX_BYTES = 1024;

    struct Outbound {
        uint8_t bytes[MO_MAX_BYTES];
        uint16_t length;
        uint32_t queuedMs;
        uint32_t from;
        uint8_t attempts;
    };

    bool isIridiumChannel(uint8_t channelIndex) const;
    int8_t iridiumChannelIndex() const;
    void enqueue(const meshtastic_MeshPacket &mp);
    void popOutbound();

    void takeOrReleaseModem();
    void release(const char *why);
    void startInit();
    void initFailed(const char *step);
    void send(Request what, const char *method, const char *target, const char *json);
    void pumpLines();
    void onLine(char *text);
    void onResponse(int code, const char *target, const char *json);
    void onEvent(const char *target, const char *json);
    void startOriginate();
    void originateFailed(int code);
    void sendSegment(size_t start, size_t length);
    void onMtStatus(const char *status);
    bool deliverMt(const uint8_t *data, size_t length);

    State state = State::Off;
    Request pending = Request::None;
    uint32_t requestSentMs = 0;
    uint8_t initRetries = 0;

    Outbound queue[QUEUE_SLOTS];
    size_t queueHead = 0;
    size_t queueCount = 0;
    uint32_t lastAttemptMs = 0;

    char line[LINE_BYTES];
    size_t lineLength = 0;
    bool lineOverflow = false;

    // The message the modem is taking now, and the last one it took whose final status is awaited.
    int moMessageId = -1;
    int inFlightMessageId = -1;
    uint8_t requestReference = 1;

    // A message the modem is pushing, assembled from its segments.
    int mtMessageId = -1;
    uint8_t mtBuffer[MT_MAX_BYTES];
    size_t mtReceived = 0;
    bool mtOverflow = false;

    uint32_t sentAsNode = 0;
    uint32_t receivedAsNode = 0;
    uint32_t failedAsNode = 0;
};

extern IridiumImtModule *iridiumImtModule;

#endif
