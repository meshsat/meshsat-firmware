#include "meshsat/IridiumImtModule.h"

#if MESHSAT_IRIDIUM && MESHSAT_IRIDIUM_JSPR

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/IridiumPipe.h"
#include "meshsat/Smaz2.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mbedtls/base64.h>

static constexpr int32_t POLL_BUSY_MS = 20;
static constexpr int32_t POLL_IDLE_MS = 250;
static constexpr int32_t POLL_OFF_MS = 1000;
static constexpr uint32_t REQUEST_TIMEOUT_MS = 10 * 1000UL;
// The modem asks for the payload right after it accepts a message, and gives up within about 360 ms.
static constexpr uint32_t SEGMENT_WAIT_MS = 5 * 1000UL;
static constexpr uint32_t ATTEMPT_GAP_MS = 60 * 1000UL;
static constexpr uint8_t ATTEMPTS_MAX = 5;
static constexpr uint32_t QUEUE_MAX_AGE_MS = 30 * 60 * 1000UL;
// A client that just dropped, or one about to connect after a boot, is first in line.
static constexpr uint32_t PHONE_GRACE_MS = 60 * 1000UL;
static constexpr uint8_t INIT_RETRIES = 3;

// Request lines and the one segment the node ever sends, built in place rather than on the stack.
static char outLine[640];
static uint8_t segmentPayload[242];
static unsigned char segmentBase64[332];
static char segmentBody[512];

IridiumImtModule *iridiumImtModule = nullptr;

IridiumImtModule::IridiumImtModule()
    : SinglePortModule("IridiumImt", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("IridiumImt")
{
    iridiumImtModule = this;
    LOG_INFO("MeshSat Iridium: node carries channel \"%s\" over IMT topic %d when no client holds the modem",
             MESHSAT_IRIDIUM_CHANNEL_NAME, MESHSAT_IRIDIUM_IMT_TOPIC);
}

// ---- small readers for the fixed JSPR shapes ----

// Points at the value after "key": or returns nullptr.
static const char *jsonValue(const char *json, const char *key)
{
    char quoted[48];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *at = strstr(json, quoted);
    if (!at)
        return nullptr;
    at += strlen(quoted);
    while (*at == ' ' || *at == ':')
        at++;
    return at;
}

static bool jsonInt(const char *json, const char *key, long &out)
{
    const char *v = jsonValue(json, key);
    if (!v || !(isdigit(static_cast<unsigned char>(*v)) || *v == '-'))
        return false;
    out = strtol(v, nullptr, 10);
    return true;
}

static bool jsonString(const char *json, const char *key, char *out, size_t capacity)
{
    const char *v = jsonValue(json, key);
    if (!v || *v != '"')
        return false;
    v++;
    size_t n = 0;
    while (*v && *v != '"' && n + 1 < capacity)
        out[n++] = *v++;
    out[n] = '\0';
    return *v == '"';
}

static bool jsonBool(const char *json, const char *key, bool &out)
{
    const char *v = jsonValue(json, key);
    if (!v)
        return false;
    if (strncmp(v, "true", 4) == 0) {
        out = true;
        return true;
    }
    if (strncmp(v, "false", 5) == 0) {
        out = false;
        return true;
    }
    return false;
}

// CRC-16/CCITT from a zero start, big-endian on the wire, as the Bridge and Android append it.
static uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// ---- channel and queue ----

int8_t IridiumImtModule::iridiumChannelIndex() const
{
    for (ChannelIndex i = 0; i < channels.getNumChannels(); ++i) {
        const meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED)
            continue;
        if (strcasecmp(ch.settings.name, MESHSAT_IRIDIUM_CHANNEL_NAME) == 0)
            return static_cast<int8_t>(i);
    }
    return -1;
}

bool IridiumImtModule::isIridiumChannel(uint8_t channelIndex) const
{
    const int8_t index = iridiumChannelIndex();
    return index >= 0 && channelIndex == static_cast<uint8_t>(index);
}

ProcessMessage IridiumImtModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Texts from other nodes over LoRa, and texts the node's own phone sends as broadcasts. The
    // satellite messages this module broadcasts itself arrive as RX_SRC_LOCAL, which callModules
    // keeps away from modules, so nothing loops back to the modem.
    if (mp.decoded.payload.size == 0 || !isIridiumChannel(mp.channel))
        return ProcessMessage::CONTINUE;
    // A client on the pipe is the gateway. A text kept here while it holds the modem would go
    // out again when the node takes the modem back.
    const IridiumPipe *pipe = IridiumPipe::instance();
    if (pipe && pipe->phoneWantsModem()) {
        LOG_INFO("MeshSat Iridium: text from 0x%08x on \"%s\" left to the client that holds the modem", (unsigned)mp.from,
                 MESHSAT_IRIDIUM_CHANNEL_NAME);
        return ProcessMessage::CONTINUE;
    }
    enqueue(mp);
    return ProcessMessage::CONTINUE;
}

void IridiumImtModule::enqueue(const meshtastic_MeshPacket &mp)
{
    const size_t length = mp.decoded.payload.size;
    if (length > MO_MAX_BYTES) {
        LOG_WARN("MeshSat Iridium: text from 0x%08x of %u B does not fit a message, dropped", (unsigned)mp.from,
                 (unsigned)length);
        return;
    }
    if (queueCount == QUEUE_SLOTS) {
        LOG_WARN("MeshSat Iridium: outbound queue full, dropping the oldest text");
        popOutbound();
    }
    Outbound &slot = queue[(queueHead + queueCount) % QUEUE_SLOTS];
    // Plain UTF-8 text, as the Hub reads a RockBLOCK message with no envelope.
    memcpy(slot.bytes, mp.decoded.payload.bytes, length);
    slot.length = static_cast<uint16_t>(length);
    slot.queuedMs = millis();
    slot.from = mp.from;
    slot.attempts = 0;
    queueCount++;
    LOG_INFO("MeshSat Iridium: text from 0x%08x on \"%s\" queued for satellite (%u B, %u waiting)", (unsigned)mp.from,
             MESHSAT_IRIDIUM_CHANNEL_NAME, (unsigned)length, (unsigned)queueCount);
}

void IridiumImtModule::popOutbound()
{
    if (queueCount == 0)
        return;
    queueHead = (queueHead + 1) % QUEUE_SLOTS;
    queueCount--;
}

bool IridiumImtModule::deliverMt(const uint8_t *data, size_t length)
{
    const int8_t index = iridiumChannelIndex();
    if (index < 0) {
        LOG_WARN("MeshSat Iridium: channel \"%s\" is not configured, satellite message dropped", MESHSAT_IRIDIUM_CHANNEL_NAME);
        return false;
    }
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return false;
    // The Hub compresses MT text with its SMAZ2 variant by default; plain text passes through unchanged.
    const size_t limit = sizeof(p->decoded.payload.bytes);
    size_t n = meshsat::smaz2Decompress(data, length, p->decoded.payload.bytes, limit);
    if (n == 0) {
        LOG_WARN("MeshSat Iridium: satellite message is not text the node can decode (%u B), passed on raw", (unsigned)length);
        n = length > limit ? limit : length;
        memcpy(p->decoded.payload.bytes, data, n);
    }
    p->decoded.payload.size = n;
    p->to = NODENUM_BROADCAST;
    p->channel = static_cast<uint8_t>(index);
    p->want_ack = false;
    service->sendToMesh(p, RX_SRC_LOCAL, true);
    receivedAsNode++;
    LOG_INFO("MeshSat Iridium: satellite message of %u B broadcast on \"%s\"", (unsigned)n, MESHSAT_IRIDIUM_CHANNEL_NAME);
    return true;
}

// ---- modem ownership ----

void IridiumImtModule::takeOrReleaseModem()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    if (state == State::Off) {
        if (pipe->phoneWantsModem() || pipe->owner() != IridiumModemOwner::None)
            return;
        // Only a modem the pipe has brought up and heard from.
        if (pipe->modemPower() != IridiumModemPower::Running || !pipe->stats().modemAnswered)
            return;
        const uint32_t lastRelease = pipe->stats().lastPhoneReleaseMs;
        if (!Throttle::hasElapsed(0, PHONE_GRACE_MS) || (lastRelease != 0 && !Throttle::hasElapsed(lastRelease, PHONE_GRACE_MS)))
            return;
        if (!pipe->tryAcquireForNode())
            return;
        LOG_INFO("MeshSat Iridium: node owns the modem");
        startInit();
        return;
    }
    // A client gets the modem between requests, never while a message is being handed to the modem.
    if (!pipe->phoneWantsModem() || state == State::Originating || state == State::Sending)
        return;
    release("client asked for it");
}

void IridiumImtModule::release(const char *why)
{
    IridiumPipe *pipe = IridiumPipe::instance();
    state = State::Off;
    pending = Request::None;
    lineLength = 0;
    lineOverflow = false;
    moMessageId = -1;
    mtMessageId = -1;
    if (pipe)
        pipe->releaseFromNode();
    LOG_INFO("MeshSat Iridium: node released the modem (%s)", why);
}

// ---- requests ----

void IridiumImtModule::send(Request what, const char *method, const char *target, const char *json)
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    // The 9704's parser wants a space after every colon and comma; the bodies are written that way.
    const int n = snprintf(outLine, sizeof(outLine), "%s %s %s\r", method, target, json);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(outLine)) {
        LOG_ERROR("MeshSat Iridium: request %s %s does not fit a line", method, target);
        return;
    }
    pipe->nodeWrite(reinterpret_cast<const uint8_t *>(outLine), static_cast<size_t>(n));
    pending = what;
    requestSentMs = millis();
    LOG_DEBUG("MeshSat Iridium: > %s %s %.60s", method, target, json);
}

void IridiumImtModule::startInit()
{
    initRetries = 0;
    lineLength = 0;
    lineOverflow = false;
    moMessageId = -1;
    state = State::Init;
    send(Request::GetApiVersion, "GET", "apiVersion", "{}");
}

void IridiumImtModule::initFailed(const char *step)
{
    if (++initRetries > INIT_RETRIES) {
        release("the modem refused the init");
        return;
    }
    LOG_WARN("MeshSat Iridium: init failed at %s, retry %u of %u", step, (unsigned)initRetries, (unsigned)INIT_RETRIES);
    send(Request::GetApiVersion, "GET", "apiVersion", "{}");
}

void IridiumImtModule::startOriginate()
{
    const Outbound &slot = queue[queueHead];
    char body[96];
    snprintf(body, sizeof(body), "{\"topic_id\": %d, \"message_length\": %u, \"request_reference\": %u}",
             MESHSAT_IRIDIUM_IMT_TOPIC, (unsigned)(slot.length + 2), (unsigned)requestReference);
    // The modem answered 407 to references above about 200 on the bench; this one stays under 100.
    requestReference = requestReference >= 99 ? 1 : static_cast<uint8_t>(requestReference + 1);
    moMessageId = -1;
    state = State::Originating;
    lastAttemptMs = millis();
    send(Request::Originate, "PUT", "messageOriginate", body);
}

void IridiumImtModule::originateFailed(int code)
{
    moMessageId = -1;
    state = State::Idle;
    lastAttemptMs = millis();
    if (queueCount == 0)
        return;
    Outbound &slot = queue[queueHead];
    slot.attempts++;
    if (slot.attempts >= ATTEMPTS_MAX) {
        LOG_WARN("MeshSat Iridium: text from 0x%08x dropped after %u refusals (last code %d)", (unsigned)slot.from,
                 (unsigned)slot.attempts, code);
        popOutbound();
        failedAsNode++;
        return;
    }
    LOG_WARN("MeshSat Iridium: the modem did not take the text from 0x%08x (code %d), attempt %u of %u", (unsigned)slot.from,
             code, (unsigned)slot.attempts, (unsigned)ATTEMPTS_MAX);
}

void IridiumImtModule::sendSegment(size_t start, size_t length)
{
    const Outbound &slot = queue[queueHead];
    const size_t total = static_cast<size_t>(slot.length) + 2;
    memcpy(segmentPayload, slot.bytes, slot.length);
    const uint16_t crc = crc16(slot.bytes, slot.length);
    segmentPayload[slot.length] = static_cast<uint8_t>(crc >> 8);
    segmentPayload[slot.length + 1] = static_cast<uint8_t>(crc & 0xFF);
    if (start >= total)
        length = 0;
    else if (start + length > total)
        length = total - start;
    size_t encoded = 0;
    if (mbedtls_base64_encode(segmentBase64, sizeof(segmentBase64) - 1, &encoded, segmentPayload + start, length) != 0) {
        originateFailed(0);
        return;
    }
    segmentBase64[encoded] = '\0';
    snprintf(segmentBody, sizeof(segmentBody),
             "{\"topic_id\": %d, \"message_id\": %d, \"segment_length\": %u, \"segment_start\": %u, \"data\": \"%s\"}",
             MESHSAT_IRIDIUM_IMT_TOPIC, moMessageId, (unsigned)length, (unsigned)start,
             reinterpret_cast<const char *>(segmentBase64));
    send(Request::Segment, "PUT", "messageOriginateSegment", segmentBody);
}

// ---- lines from the modem ----

void IridiumImtModule::pumpLines()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    while (pipe->nodeAvailable() > 0) {
        const int value = pipe->nodeRead();
        if (value < 0)
            break;
        const char c = static_cast<char>(value);
        if (c == '\r' || c == '\n') {
            if (lineOverflow) {
                lineOverflow = false;
                lineLength = 0;
                continue;
            }
            if (lineLength == 0)
                continue;
            line[lineLength] = '\0';
            onLine(line);
            lineLength = 0;
            continue;
        }
        if (lineLength + 1 >= LINE_BYTES) {
            lineOverflow = true;
            continue;
        }
        line[lineLength++] = c;
    }
}

void IridiumImtModule::onLine(char *text)
{
    // "<code> <target> {json}"
    if (strlen(text) < 5 || !isdigit(static_cast<unsigned char>(text[0])) || !isdigit(static_cast<unsigned char>(text[1])) ||
        !isdigit(static_cast<unsigned char>(text[2])) || text[3] != ' ') {
        LOG_DEBUG("MeshSat Iridium: modem line ignored: %.40s", text);
        return;
    }
    const int code = atoi(text);
    char *target = text + 4;
    char *json = strchr(target, ' ');
    if (json) {
        *json = '\0';
        json++;
    } else {
        json = target + strlen(target);
    }
    if (code == 299)
        onEvent(target, json);
    else
        onResponse(code, target, json);
}

void IridiumImtModule::onResponse(int code, const char *target, const char *json)
{
    const Request what = pending;
    pending = Request::None;
    switch (what) {
    case Request::GetApiVersion: {
        if (code != 200 || strcmp(target, "apiVersion") != 0) {
            initFailed("GET apiVersion");
            return;
        }
        if (jsonValue(json, "active_version")) {
            send(Request::GetSimConfig, "GET", "simConfig", "{}");
            return;
        }
        // The first supported version is the newest.
        const char *list = jsonValue(json, "supported_versions");
        long major = 0, minor = 0, patch = 0;
        if (!list || !jsonInt(list, "major", major) || !jsonInt(list, "minor", minor) || !jsonInt(list, "patch", patch)) {
            initFailed("apiVersion list");
            return;
        }
        char body[96];
        snprintf(body, sizeof(body), "{\"active_version\": {\"major\": %ld, \"minor\": %ld, \"patch\": %ld}}", major, minor,
                 patch);
        send(Request::PutApiVersion, "PUT", "apiVersion", body);
        return;
    }
    case Request::PutApiVersion:
        if (code != 200 && code != 402) {
            initFailed("PUT apiVersion");
            return;
        }
        send(Request::GetSimConfig, "GET", "simConfig", "{}");
        return;
    case Request::GetSimConfig: {
        char iface[16];
        if (code == 200 && jsonString(json, "interface", iface, sizeof(iface)) && strcmp(iface, "internal") == 0) {
            send(Request::GetOperationalState, "GET", "operationalState", "{}");
            return;
        }
        send(Request::PutSimConfig, "PUT", "simConfig", "{\"interface\": \"internal\"}");
        return;
    }
    case Request::PutSimConfig:
        if (code != 200 && code != 402) {
            initFailed("PUT simConfig");
            return;
        }
        send(Request::GetOperationalState, "GET", "operationalState", "{}");
        return;
    case Request::GetOperationalState: {
        char value[16];
        if (code == 200 && jsonString(json, "state", value, sizeof(value)) && strcmp(value, "active") == 0) {
            state = State::Idle;
            LOG_INFO("MeshSat Iridium: 9704 up and active, the node routes \"%s\"", MESHSAT_IRIDIUM_CHANNEL_NAME);
            return;
        }
        send(Request::PutOperationalState, "PUT", "operationalState", "{\"state\": \"active\"}");
        return;
    }
    case Request::PutOperationalState:
        if (code != 200 && code != 402) {
            initFailed("PUT operationalState");
            return;
        }
        state = State::Idle;
        LOG_INFO("MeshSat Iridium: 9704 up and set active, the node routes \"%s\"", MESHSAT_IRIDIUM_CHANNEL_NAME);
        return;
    case Request::Originate: {
        long id = 0;
        if (code != 200 || !jsonInt(json, "message_id", id)) {
            originateFailed(code);
            return;
        }
        moMessageId = static_cast<int>(id);
        state = State::Sending;
        requestSentMs = millis();
        LOG_INFO("MeshSat Iridium: message %d accepted by the modem, handing over the payload", moMessageId);
        return;
    }
    case Request::Segment:
        if (code != 200) {
            originateFailed(code);
            return;
        }
        inFlightMessageId = moMessageId;
        moMessageId = -1;
        LOG_INFO("MeshSat Iridium: message %d is in the modem; it goes out when the sky is there", inFlightMessageId);
        popOutbound();
        state = State::Idle;
        return;
    case Request::None:
        LOG_DEBUG("MeshSat Iridium: unexpected %d %s", code, target);
        return;
    }
}

void IridiumImtModule::onEvent(const char *target, const char *json)
{
    long id = 0;
    if (strcmp(target, "constellationState") == 0) {
        long bars = 0;
        bool visible = false;
        jsonBool(json, "constellation_visible", visible);
        if (jsonInt(json, "signal_bars", bars)) {
            IridiumPipe *pipe = IridiumPipe::instance();
            if (pipe)
                pipe->noteSignal(bars > 5 ? 5 : (bars < 0 ? 0 : static_cast<int>(bars)));
        }
        LOG_DEBUG("MeshSat Iridium: constellation %s, %ld bars", visible ? "visible" : "not visible", bars);
        return;
    }
    if (strcmp(target, "messageOriginateSegment") == 0) {
        long start = 0, length = 0;
        if (!jsonInt(json, "message_id", id) || id != moMessageId || !jsonInt(json, "segment_start", start) ||
            !jsonInt(json, "segment_length", length) || start < 0 || length < 0)
            return;
        sendSegment(static_cast<size_t>(start), static_cast<size_t>(length));
        return;
    }
    if (strcmp(target, "messageOriginateStatus") == 0) {
        char status[48];
        if (!jsonInt(json, "message_id", id) || !jsonString(json, "final_mo_status", status, sizeof(status)))
            return;
        if (strstr(status, "transferred")) {
            sentAsNode++;
            LOG_INFO("MeshSat Iridium: message %ld sent by satellite (%s)", id, status);
        } else {
            failedAsNode++;
            LOG_WARN("MeshSat Iridium: message %ld not sent: %s", id, status);
        }
        if (static_cast<int>(id) == inFlightMessageId)
            inFlightMessageId = -1;
        return;
    }
    if (strcmp(target, "messageTerminate") == 0) {
        long maxLength = 0;
        if (!jsonInt(json, "message_id", id))
            return;
        mtMessageId = static_cast<int>(id);
        mtReceived = 0;
        mtOverflow = false;
        jsonInt(json, "message_length_max", maxLength);
        LOG_INFO("MeshSat Iridium: satellite message %ld announced (up to %ld B)", id, maxLength);
        return;
    }
    if (strcmp(target, "messageTerminateSegment") == 0) {
        long start = 0;
        if (!jsonInt(json, "message_id", id) || static_cast<int>(id) != mtMessageId || !jsonInt(json, "segment_start", start))
            return;
        const char *data = jsonValue(json, "data");
        if (!data || *data != '"')
            return;
        data++;
        const char *end = strchr(data, '"');
        if (!end)
            return;
        if (start < 0 || static_cast<size_t>(start) >= MT_MAX_BYTES) {
            mtOverflow = true;
            return;
        }
        size_t written = 0;
        const int rc = mbedtls_base64_decode(mtBuffer + start, MT_MAX_BYTES - static_cast<size_t>(start), &written,
                                             reinterpret_cast<const unsigned char *>(data), static_cast<size_t>(end - data));
        if (rc != 0) {
            mtOverflow = true;
            LOG_WARN("MeshSat Iridium: a segment of message %ld does not fit or decode (%d)", id, rc);
            return;
        }
        const size_t endAt = static_cast<size_t>(start) + written;
        if (endAt > mtReceived)
            mtReceived = endAt;
        return;
    }
    if (strcmp(target, "messageTerminateStatus") == 0) {
        char status[32];
        if (!jsonInt(json, "message_id", id) || static_cast<int>(id) != mtMessageId ||
            !jsonString(json, "final_mt_status", status, sizeof(status)))
            return;
        onMtStatus(status);
        return;
    }
    LOG_DEBUG("MeshSat Iridium: event %s", target);
}

void IridiumImtModule::onMtStatus(const char *status)
{
    const int id = mtMessageId;
    mtMessageId = -1;
    if (strcmp(status, "complete") != 0) {
        LOG_WARN("MeshSat Iridium: satellite message %d failed: %s", id, status);
        return;
    }
    if (mtOverflow || mtReceived < 2) {
        LOG_WARN("MeshSat Iridium: satellite message %d is too large or too short for the node (%u B)", id, (unsigned)mtReceived);
        return;
    }
    const size_t length = mtReceived - 2;
    const uint16_t want = static_cast<uint16_t>((static_cast<uint16_t>(mtBuffer[length]) << 8) | mtBuffer[length + 1]);
    if (crc16(mtBuffer, length) != want) {
        LOG_WARN("MeshSat Iridium: satellite message %d failed its checksum", id);
        return;
    }
    deliverMt(mtBuffer, length);
}

// ---- the loop ----

int32_t IridiumImtModule::runOnce()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return POLL_OFF_MS;

    while (queueCount > 0 && Throttle::hasElapsed(queue[queueHead].queuedMs, QUEUE_MAX_AGE_MS)) {
        LOG_WARN("MeshSat Iridium: text from 0x%08x waited %u min for the modem, dropped", (unsigned)queue[queueHead].from,
                 (unsigned)(QUEUE_MAX_AGE_MS / 60000));
        popOutbound();
        failedAsNode++;
    }

    if (state != State::Off && pipe->modemPower() != IridiumModemPower::Running) {
        release("the modem's supply is off");
        return POLL_OFF_MS;
    }
    takeOrReleaseModem();
    if (state == State::Off)
        return POLL_OFF_MS;

    pumpLines();

    if (pending != Request::None && Throttle::hasElapsed(requestSentMs, REQUEST_TIMEOUT_MS)) {
        const Request what = pending;
        pending = Request::None;
        if (state == State::Init)
            initFailed("no answer");
        else if (what == Request::Originate || what == Request::Segment)
            originateFailed(0);
        else
            state = State::Idle;
        return POLL_BUSY_MS;
    }
    if (state == State::Sending && pending == Request::None && Throttle::hasElapsed(requestSentMs, SEGMENT_WAIT_MS)) {
        LOG_WARN("MeshSat Iridium: the modem accepted message %d but never asked for its payload", moMessageId);
        originateFailed(0);
        return POLL_BUSY_MS;
    }
    if (state != State::Idle)
        return POLL_BUSY_MS;
    // The hand-over runs at the top of the next run.
    if (pipe->phoneWantsModem())
        return POLL_BUSY_MS;
    if (queueCount > 0 && (queue[queueHead].attempts == 0 || Throttle::hasElapsed(lastAttemptMs, ATTEMPT_GAP_MS))) {
        startOriginate();
        return POLL_BUSY_MS;
    }
    return POLL_IDLE_MS;
}

#endif
