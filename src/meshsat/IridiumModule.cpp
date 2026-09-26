#include "meshsat/IridiumModule.h"

#if MESHSAT_IRIDIUM

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "gps/RTC.h"
#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/IridiumPipe.h"
#include "meshsat/Smaz2.h"

#include <Preferences.h>
#include <cctype>
#include <cstdlib>
#include <cstring>

IridiumModule *iridiumModule = nullptr;

static constexpr int32_t POLL_BUSY_MS = 20;
static constexpr int32_t POLL_IDLE_MS = 250;
static constexpr int32_t POLL_OFF_MS = 1000;

static constexpr uint32_t COMMAND_TIMEOUT_MS = 3 * 1000UL;
static constexpr uint32_t SESSION_TIMEOUT_MS = 90 * 1000UL;
static constexpr uint32_t READ_TIMEOUT_MS = 10 * 1000UL;
static constexpr uint32_t ATTEMPT_GAP_MS = 10 * 1000UL;
static constexpr uint32_t HOLD_AFTER_32_36_MS = 3 * 60 * 1000UL;
static constexpr uint32_t SBDSX_INTERVAL_MS = 10 * 60 * 1000UL;
// The modem answers about 10 s after power; the first command is retried this many times.
static constexpr uint8_t INIT_RETRIES = 20;
static constexpr uint32_t QUEUE_MAX_AGE_MS = 30 * 60 * 1000UL;
static constexpr size_t MO_MAX_BYTES = 340;
static constexpr size_t MT_MAX_BYTES = 270;

static constexpr const char *NVS_NAMESPACE = "meshsat";
static constexpr const char *NVS_DAY = "ridxDay";
static constexpr const char *NVS_COUNT = "ridxCnt";

// The Bridge's connect sequence. AT&K0 first: two Ground Control pages disagree on the flow-control default.
static const char *const INIT_SEQUENCE[] = {"AT&K0", "ATE0", "AT&D0", "AT", "AT+CGSN", "AT+SBDMTA=1", "AT+SBDD0", "AT+SBDD1"};
static constexpr uint8_t INIT_STEPS = sizeof(INIT_SEQUENCE) / sizeof(INIT_SEQUENCE[0]);

IridiumModule::IridiumModule()
    : SinglePortModule("IridiumRoute", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("IridiumRoute")
{
    iridiumModule = this;
    LOG_INFO("MeshSat Iridium: node carries channel \"%s\" over Iridium when no phone holds the modem, %u sessions a day",
             MESHSAT_IRIDIUM_CHANNEL_NAME, (unsigned)MESHSAT_IRIDIUM_DAILY_SESSIONS);
}

// ---- channel and queue ----

int8_t IridiumModule::iridiumChannelIndex() const
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

bool IridiumModule::isIridiumChannel(uint8_t channelIndex) const
{
    const int8_t index = iridiumChannelIndex();
    return index >= 0 && channelIndex == static_cast<uint8_t>(index);
}

ProcessMessage IridiumModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Texts from other nodes over LoRa, and texts the node's own phone sends as broadcasts
    // (Router::sendLocal hands those to the modules too). The satellite messages this module
    // broadcasts itself arrive as RX_SRC_LOCAL, which callModules keeps away from modules,
    // so nothing loops back to the modem.
    if (mp.decoded.payload.size == 0 || !isIridiumChannel(mp.channel))
        return ProcessMessage::CONTINUE;
    enqueue(mp);
    return ProcessMessage::CONTINUE;
}

void IridiumModule::enqueue(const meshtastic_MeshPacket &mp)
{
    if (queueCount == QUEUE_SLOTS) {
        LOG_WARN("MeshSat Iridium: outbound queue full, dropping the oldest text");
        popOutbound();
    }
    Outbound &slot = queue[(queueHead + queueCount) % QUEUE_SLOTS];
    slot.length = static_cast<uint16_t>(encodeMo(mp, slot.bytes, sizeof(slot.bytes)));
    if (slot.length == 0) {
        LOG_WARN("MeshSat Iridium: text from 0x%08x does not fit an SBD message, dropped", (unsigned)mp.from);
        return;
    }
    slot.queuedMs = millis();
    slot.from = mp.from;
    queueCount++;
    LOG_INFO("MeshSat Iridium: text from 0x%08x on \"%s\" queued for satellite (%u B, %u waiting)", (unsigned)mp.from,
             MESHSAT_IRIDIUM_CHANNEL_NAME, (unsigned)slot.length, (unsigned)queueCount);
}

void IridiumModule::popOutbound()
{
    if (queueCount == 0)
        return;
    queueHead = (queueHead + 1) % QUEUE_SLOTS;
    queueCount--;
}

// ---- payload format ----

size_t IridiumModule::encodeMo(const meshtastic_MeshPacket &mp, uint8_t *out, size_t capacity)
{
    // Plain UTF-8 text, as the Hub reads a RockBLOCK message with no envelope.
    const size_t length = mp.decoded.payload.size;
    if (length == 0 || length > capacity || length > MO_MAX_BYTES)
        return 0;
    memcpy(out, mp.decoded.payload.bytes, length);
    return length;
}

void IridiumModule::deliverMt(const uint8_t *data, size_t length)
{
    const int8_t index = iridiumChannelIndex();
    if (index < 0) {
        LOG_WARN("MeshSat Iridium: channel \"%s\" is not configured, satellite message dropped", MESHSAT_IRIDIUM_CHANNEL_NAME);
        return;
    }
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
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
}

// ---- modem ownership ----

void IridiumModule::takeOrReleaseModem()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    if (state == State::Off) {
        if (pipe->phoneWantsModem() || pipe->owner() != IridiumModemOwner::None)
            return;
        if (!pipe->tryAcquireForNode())
            return;
        LOG_INFO("MeshSat Iridium: node owns the modem");
        startInit();
        return;
    }
    // A phone gets the modem between commands, never during a session or a read.
    if (pipe->phoneWantsModem() && state != State::Session && state != State::ReadMt && state != State::WriteStatus)
        release("phone asked for it");
}

void IridiumModule::release(const char *why)
{
    IridiumPipe *pipe = IridiumPipe::instance();
    state = State::Off;
    command = Command::None;
    lineLength = 0;
    if (pipe)
        pipe->releaseFromNode();
    LOG_INFO("MeshSat Iridium: node released the modem (%s)", why);
}

// ---- commands ----

void IridiumModule::startInit()
{
    initStep = 0;
    initRetries = 0;
    moWritten = false;
    state = State::Init;
    sendCommand(INIT_SEQUENCE[0], Command::InitStep, COMMAND_TIMEOUT_MS);
}

void IridiumModule::sendCommand(const char *text, Command what, uint32_t timeoutMs)
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    char buffer[40];
    const int n = snprintf(buffer, sizeof(buffer), "%s\r", text);
    pipe->nodeWrite(reinterpret_cast<const uint8_t *>(buffer), static_cast<size_t>(n));
    command = what;
    commandSentMs = millis();
    commandTimeoutMs = timeoutMs;
    lineLength = 0;
    if (state != State::Init)
        state = State::Command;
}

void IridiumModule::startWrite()
{
    const Outbound &slot = queue[queueHead];
    char text[24];
    snprintf(text, sizeof(text), "AT+SBDWB=%u", (unsigned)slot.length);
    sendCommand(text, Command::None, COMMAND_TIMEOUT_MS);
    state = State::WriteReady;
}

void IridiumModule::startSession()
{
    lastAttemptMs = millis();
    sendCommand("AT+SBDIX", Command::Sbdix, SESSION_TIMEOUT_MS);
    state = State::Session;
    sessionsAsNode++;
}

void IridiumModule::startReadMt()
{
    sendCommand("AT+SBDRB", Command::None, READ_TIMEOUT_MS);
    state = State::ReadMt;
    frameLength = 0;
    frameExpected = 0;
}

// ---- responses ----

void IridiumModule::pumpLines()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    while (pipe->nodeAvailable() > 0) {
        const int value = pipe->nodeRead();
        if (value < 0)
            break;
        if (value == '\r' || value == '\n') {
            if (lineLength > 0) {
                line[lineLength] = '\0';
                onLine(line);
            }
            lineLength = 0;
            continue;
        }
        if (lineLength < LINE_BYTES - 1)
            line[lineLength++] = static_cast<char>(value);
    }
}

void IridiumModule::onLine(const char *text)
{
    switch (state) {
    case State::Init:
    case State::Command:
        if (strncmp(text, "+SBDSX:", 7) == 0) {
            onSbdsx(text);
        } else if (strcmp(text, "OK") == 0) {
            onCommandDone(true);
        } else if (strcmp(text, "ERROR") == 0) {
            onCommandDone(false);
        } else if (state == State::Init && initStep == 4 && strlen(text) == 15 && isdigit(static_cast<unsigned char>(text[0]))) {
            LOG_INFO("MeshSat Iridium: modem IMEI %s", text);
        }
        break;
    case State::WriteReady:
        if (strcmp(text, "READY") == 0) {
            const Outbound &slot = queue[queueHead];
            uint16_t sum = 0;
            for (uint16_t i = 0; i < slot.length; ++i)
                sum = static_cast<uint16_t>(sum + slot.bytes[i]);
            const uint8_t checksum[2] = {static_cast<uint8_t>(sum >> 8), static_cast<uint8_t>(sum & 0xFF)};
            IridiumPipe *pipe = IridiumPipe::instance();
            pipe->nodeWrite(slot.bytes, slot.length);
            pipe->nodeWrite(checksum, sizeof(checksum));
            state = State::WriteStatus;
            commandSentMs = millis();
        } else if (strcmp(text, "ERROR") == 0) {
            LOG_WARN("MeshSat Iridium: SBDWB refused");
            popOutbound();
            state = State::Idle;
        }
        break;
    case State::WriteStatus:
        if (strlen(text) == 1 && isdigit(static_cast<unsigned char>(text[0]))) {
            if (text[0] == '0') {
                moWritten = true;
            } else {
                LOG_WARN("MeshSat Iridium: SBDWB status %c, text dropped", text[0]);
                popOutbound();
            }
        } else if (strcmp(text, "OK") == 0 || strcmp(text, "ERROR") == 0) {
            state = State::Idle;
        }
        break;
    case State::Session:
        if (strncmp(text, "+SBDIX", 6) == 0) {
            onSessionResult(text);
        } else if (strcmp(text, "OK") == 0 || strcmp(text, "ERROR") == 0) {
            state = State::Idle;
        }
        break;
    case State::ReadMt:
    case State::Off:
    case State::Idle:
        break;
    }
}

void IridiumModule::onCommandDone(bool ok)
{
    const Command done = command;
    command = Command::None;
    if (state == State::Init) {
        if (!ok && initStep < 3) {
            LOG_WARN("MeshSat Iridium: %s answered ERROR", INIT_SEQUENCE[initStep]);
        }
        initStep++;
        if (initStep >= INIT_STEPS) {
            state = State::Idle;
            lastSbdsxMs = 0;
            LOG_INFO("MeshSat Iridium: modem ready for the node");
            return;
        }
        sendCommand(INIT_SEQUENCE[initStep], Command::InitStep, COMMAND_TIMEOUT_MS);
        return;
    }
    state = State::Idle;
    if (done == Command::ClearMo && ok) {
        moWritten = false;
        needClearMo = false;
    }
    if (done == Command::ClearMt)
        mtInBuffer = false;
}

void IridiumModule::onSbdsx(const char *text)
{
    // +SBDSX: <MO flag>, <MOMSN>, <MT flag>, <MTMSN>, <RA flag>, <msg waiting>
    long fields[6] = {0, 0, 0, 0, 0, 0};
    const char *cursor = text + 7;
    for (int i = 0; i < 6; ++i) {
        char *end = nullptr;
        fields[i] = strtol(cursor, &end, 10);
        if (end == cursor)
            break;
        cursor = end;
        while (*cursor == ',' || *cursor == ' ')
            ++cursor;
    }
    if (fields[2] == 1 || fields[4] == 1 || fields[5] > 0) {
        mtWaiting = true;
        LOG_INFO("MeshSat Iridium: SBDSX says a message is waiting (MT %ld, RA %ld, queued %ld)", fields[2], fields[4],
                 fields[5]);
    }
}

void IridiumModule::onSessionResult(const char *text)
{
    // +SBDIX: <MO status>, <MOMSN>, <MT status>, <MTMSN>, <MT length>, <MT queued>
    long fields[6] = {-1, 0, -1, 0, 0, 0};
    const char *cursor = strchr(text, ':');
    cursor = cursor ? cursor + 1 : text;
    for (int i = 0; i < 6; ++i) {
        char *end = nullptr;
        fields[i] = strtol(cursor, &end, 10);
        if (end == cursor)
            break;
        cursor = end;
        while (*cursor == ',' || *cursor == ' ')
            ++cursor;
    }
    const long mo = fields[0];
    const long mt = fields[2];
    mtQueued = fields[5] > 0 ? static_cast<uint32_t>(fields[5]) : 0;
    mtWaiting = false;
    // Status 32 never reached the gateway and is not billed; everything else counts against the day.
    if (mo != 32)
        countSession();

    if (moWritten) {
        if (mo >= 0 && mo <= 4) {
            popOutbound();
            sentAsNode++;
            // The modem keeps the sent MO until it is cleared, so clear it before the next session.
            needClearMo = true;
            LOG_INFO("MeshSat Iridium: text sent by the node, MOMSN %ld", fields[1]);
        } else {
            LOG_WARN("MeshSat Iridium: node session failed with MO status %ld, text kept", mo);
        }
    }
    if (mo == 32 || mo == 36) {
        holding = true;
        holdUntilMs = millis();
        LOG_INFO("MeshSat Iridium: holding sends for %u s after status %ld", (unsigned)(HOLD_AFTER_32_36_MS / 1000), mo);
    } else if (mo >= 0 && mo <= 4) {
        holding = false;
    }
    // MT status 1: a message landed in the modem's MT buffer; the read starts from Idle after the OK.
    if (mt == 1)
        mtInBuffer = true;
}

void IridiumModule::pumpBinary()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    while (pipe->nodeAvailable() > 0 && frameLength < sizeof(frame)) {
        const int value = pipe->nodeRead();
        if (value < 0)
            break;
        frame[frameLength++] = static_cast<uint8_t>(value);
        if (frameLength == 2)
            frameExpected = static_cast<uint16_t>((frame[0] << 8) | frame[1]);
        if (frameLength >= 2 && frameLength == static_cast<size_t>(frameExpected) + 4) {
            onMtFrame();
            return;
        }
    }
}

void IridiumModule::onMtFrame()
{
    const uint16_t length = frameExpected;
    uint16_t sum = 0;
    for (uint16_t i = 0; i < length; ++i)
        sum = static_cast<uint16_t>(sum + frame[2 + i]);
    const uint16_t given = static_cast<uint16_t>((frame[2 + length] << 8) | frame[3 + length]);
    state = State::Idle;
    lineLength = 0;
    mtInBuffer = false;
    if (length == 0 || length > MT_MAX_BYTES) {
        LOG_WARN("MeshSat Iridium: SBDRB frame of %u B ignored", (unsigned)length);
    } else if (sum != given) {
        LOG_WARN("MeshSat Iridium: SBDRB checksum mismatch, message dropped");
    } else {
        deliverMt(frame + 2, length);
    }
    // The OK after the frame is consumed by the next command's line reader; clear the MT buffer now.
    sendCommand("AT+SBDD1", Command::ClearMt, COMMAND_TIMEOUT_MS);
}

// ---- session budget ----

uint32_t IridiumModule::today() const
{
    const uint32_t now = getValidTime(RTCQualityDevice, true);
    if (now > 0)
        return now / 86400UL;
    return 1000000UL + millis() / 86400000UL;
}

void IridiumModule::loadDailyCount()
{
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, true)) {
        dailyDay = prefs.getUInt(NVS_DAY, 0);
        dailyCount = prefs.getUInt(NVS_COUNT, 0);
        prefs.end();
    }
    dailyLoaded = true;
}

void IridiumModule::saveDailyCount()
{
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false))
        return;
    prefs.putUInt(NVS_DAY, dailyDay);
    prefs.putUInt(NVS_COUNT, dailyCount);
    prefs.end();
}

void IridiumModule::countSession()
{
    if (!dailyLoaded)
        loadDailyCount();
    const uint32_t day = today();
    if (day != dailyDay) {
        dailyDay = day;
        dailyCount = 0;
    }
    dailyCount++;
    saveDailyCount();
}

bool IridiumModule::canOpenSession()
{
    if (!dailyLoaded)
        loadDailyCount();
    if (today() == dailyDay && dailyCount >= MESHSAT_IRIDIUM_DAILY_SESSIONS) {
        static uint32_t lastCapLogMs = 0;
        if (Throttle::hasElapsed(lastCapLogMs, 60 * 1000UL)) {
            LOG_WARN("MeshSat Iridium: daily cap of %u sessions reached, not opening another",
                     (unsigned)MESHSAT_IRIDIUM_DAILY_SESSIONS);
            lastCapLogMs = millis();
        }
        return false;
    }
    if (holding && !Throttle::hasElapsed(holdUntilMs, HOLD_AFTER_32_36_MS))
        return false;
    holding = false;
    return Throttle::hasElapsed(lastAttemptMs, ATTEMPT_GAP_MS);
}

// ---- the loop ----

int32_t IridiumModule::runOnce()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return POLL_OFF_MS;

    // Texts that waited too long for the modem are not worth a credit any more.
    while (queueCount > 0 && Throttle::hasElapsed(queue[queueHead].queuedMs, QUEUE_MAX_AGE_MS)) {
        LOG_WARN("MeshSat Iridium: text from 0x%08x waited %u min for the modem, dropped", (unsigned)queue[queueHead].from,
                 (unsigned)(QUEUE_MAX_AGE_MS / 60000));
        popOutbound();
    }

    takeOrReleaseModem();
    if (state == State::Off)
        return POLL_OFF_MS;

    if (state == State::ReadMt)
        pumpBinary();
    else
        pumpLines();

    // Timeouts.
    if (state == State::Init && Throttle::hasElapsed(commandSentMs, commandTimeoutMs)) {
        if (++initRetries > INIT_RETRIES) {
            release("modem did not answer the init sequence");
            return POLL_OFF_MS;
        }
        sendCommand(INIT_SEQUENCE[initStep], Command::InitStep, COMMAND_TIMEOUT_MS);
    } else if ((state == State::Command || state == State::WriteReady || state == State::WriteStatus) &&
               Throttle::hasElapsed(commandSentMs, commandTimeoutMs)) {
        LOG_WARN("MeshSat Iridium: no answer to the last command, back to idle");
        state = State::Idle;
    } else if (state == State::Session && Throttle::hasElapsed(commandSentMs, SESSION_TIMEOUT_MS)) {
        LOG_WARN("MeshSat Iridium: session gave no result in %u s", (unsigned)(SESSION_TIMEOUT_MS / 1000));
        // It may have reached the gateway before it stalled, so it counts.
        countSession();
        state = State::Idle;
    } else if (state == State::ReadMt && Throttle::hasElapsed(commandSentMs, READ_TIMEOUT_MS)) {
        LOG_WARN("MeshSat Iridium: SBDRB gave no frame, clearing");
        mtInBuffer = false;
        sendCommand("AT+SBDD1", Command::ClearMt, COMMAND_TIMEOUT_MS);
    }

    if (state != State::Idle)
        return POLL_BUSY_MS;

    // Idle: decide the next command.
    const IridiumStats &st = pipe->stats();
    if (mtInBuffer) {
        startReadMt();
        return POLL_BUSY_MS;
    }
    if (needClearMo) {
        sendCommand("AT+SBDD0", Command::ClearMo, COMMAND_TIMEOUT_MS);
        return POLL_BUSY_MS;
    }
    if (queueCount > 0 && !moWritten) {
        startWrite();
        return POLL_BUSY_MS;
    }
    const bool wantSession = moWritten || mtWaiting || mtQueued > 0 || st.ringPending;
    if (wantSession && canOpenSession()) {
        startSession();
        return POLL_BUSY_MS;
    }
    if (Throttle::hasElapsed(lastSbdsxMs, SBDSX_INTERVAL_MS) || lastSbdsxMs == 0) {
        lastSbdsxMs = millis();
        sendCommand("AT+SBDSX", Command::Sbdsx, COMMAND_TIMEOUT_MS);
        return POLL_BUSY_MS;
    }
    return POLL_IDLE_MS;
}

#endif
