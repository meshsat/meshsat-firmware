#include "meshsat/IridiumPipe.h"

#if MESHSAT_IRIDIUM

#include "Power.h"
#include "mesh/Throttle.h"
#include "sleep.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <HardwareSerial.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

// Holds several AT+SBDWB payloads (340 bytes + checksum each) plus their command lines.
static constexpr size_t INCOMING_BYTES = 2048;
static constexpr size_t UART_RX_BUFFER_BYTES = 1024;
static constexpr size_t UART_TX_BUFFER_BYTES = 512;
static constexpr size_t COPY_CHUNK_BYTES = 128;

static constexpr size_t ATT_HEADER_BYTES = 3;
static constexpr size_t DEFAULT_NOTIFY_BYTES = 20;
static constexpr size_t MAX_NOTIFY_BYTES = 244;
static constexpr int MAX_NOTIFIES_PER_RUN = 4;

static constexpr int32_t BUSY_INTERVAL_MS = 5;
static constexpr int32_t IDLE_INTERVAL_MS = 20;

// CCCD bit 0: notifications enabled.
static constexpr uint16_t CCCD_NOTIFY = 0x0001;

// A phone that unsubscribes and comes back within this window keeps the modem without a release.
static constexpr uint32_t RELEASE_DEBOUNCE_MS = 2 * 1000UL;
// An SBDIX answers within 90 s or not at all (Bridge and Android use 90 and 95 s).
static constexpr uint32_t SESSION_CAP_MS = 95 * 1000UL;
static constexpr uint32_t DROP_LOG_INTERVAL_MS = 10 * 1000UL;

// Free AT probe while nobody owns the modem; three misses power-cycle the supply where there is one.
static constexpr uint32_t HEALTH_INTERVAL_MS = 10 * 60 * 1000UL;
static constexpr uint32_t HEALTH_FIRST_DELAY_MS = 30 * 1000UL;
static constexpr uint32_t HEALTH_REPLY_MS = 3 * 1000UL;
static constexpr uint32_t HEALTH_MISSES_TO_CYCLE = 3;
// Iridium 9603 developer's guide 3.2.1: off for at least 2 s; it answers about 10 s after power.
static constexpr uint32_t POWER_CYCLE_OFF_MS = 2 * 1000UL;
static constexpr uint32_t FLUSH_MS = 200;

static IridiumPipe *pipeInstance = nullptr;
// NimBLE drops a notification it has no buffer for, so a chunk is kept until it was accepted.
static uint8_t pendingChunk[MAX_NOTIFY_BYTES];
static size_t pendingLength = 0;
static bool notifyFailed = false;
static HardwareSerial modemUart(MESHSAT_IRIDIUM_UART_NUM);
static BLEServer *bleServer = nullptr;
static BLECharacteristic *txCharacteristic = nullptr;
static BLECharacteristic *statusCharacteristic = nullptr;

static StaticStreamBuffer_t incomingControl;
static uint8_t incomingStorage[INCOMING_BYTES + 1];
static StreamBufferHandle_t incoming = nullptr;

class IridiumPipeRxCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *characteristic) override
    {
        if (pipeInstance)
            pipeInstance->onPhoneWrite(characteristic->getData(), characteristic->getLength());
    }
};

class IridiumPipeTxCallbacks : public BLECharacteristicCallbacks
{
    void onSubscribe(BLECharacteristic *characteristic, ble_gap_conn_desc *desc, uint16_t subValue) override
    {
        (void)characteristic;
        if (pipeInstance && desc)
            pipeInstance->onPhoneSubscribe(desc->conn_handle, (subValue & CCCD_NOTIFY) != 0);
    }

    // Runs synchronously inside notify(), on the calling thread.
    void onStatus(BLECharacteristic *characteristic, Status status, uint32_t code) override
    {
        (void)characteristic;
        (void)code;
        if (status == Status::ERROR_GATT)
            notifyFailed = true;
    }
};

static IridiumPipeRxCallbacks rxCallbacks;
static IridiumPipeTxCallbacks txCallbacks;

IridiumPipe::IridiumPipe() : concurrency::OSThread("IridiumPipe")
{
    nextHealthDelayMs = HEALTH_FIRST_DELAY_MS;
    lastHealthMs = millis();
}

void IridiumPipe::begin()
{
    if (pipeInstance)
        return;
    incoming = xStreamBufferCreateStatic(INCOMING_BYTES, 1, incomingStorage, &incomingControl);
    pipeInstance = new IridiumPipe();
    pipeInstance->deepSleepObserver.observe(&notifyDeepSleep);
    pipeInstance->powerModem(true);
    pipeInstance->openUart();
}

IridiumPipe *IridiumPipe::instance()
{
    return pipeInstance;
}

void IridiumPipe::setupBleService(BLEServer *server, bool requireEncryption)
{
    bleServer = server;

    uint32_t rxProperties = BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR;
    uint32_t txProperties = BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ;
    if (requireEncryption) {
        rxProperties |= BLECharacteristic::PROPERTY_WRITE_AUTHEN | BLECharacteristic::PROPERTY_WRITE_ENC;
        txProperties |= BLECharacteristic::PROPERTY_READ_AUTHEN | BLECharacteristic::PROPERTY_READ_ENC;
    }

    BLEService *service = server->createService(SERVICE_UUID);
    BLECharacteristic *rx = service->createCharacteristic(RX_UUID, rxProperties);
    rx->setCallbacks(&rxCallbacks);
    txCharacteristic = service->createCharacteristic(TX_UUID, txProperties);
    txCharacteristic->setCallbacks(&txCallbacks);
    statusCharacteristic = service->createCharacteristic(STATUS_UUID, txProperties);
    const uint8_t owner = pipeInstance ? static_cast<uint8_t>(pipeInstance->owner()) : 0;
    const uint8_t status[2] = {CONTRACT_VERSION, owner};
    statusCharacteristic->setValue(status, sizeof(status));
    service->start();
    LOG_INFO("MeshSat Iridium: BLE serial service up (%s)", requireEncryption ? "pairing required" : "open");
}

void IridiumPipe::openUart()
{
    modemUart.setRxBufferSize(UART_RX_BUFFER_BYTES);
    modemUart.setTxBufferSize(UART_TX_BUFFER_BYTES);
    modemUart.begin(MESHSAT_IRIDIUM_BAUD, SERIAL_8N1, MESHSAT_IRIDIUM_RX_PIN, MESHSAT_IRIDIUM_TX_PIN);
    modemUart.setHwFlowCtrlMode(UART_HW_FLOWCTRL_DISABLE);
    // Keeps RX idle-high when no modem is connected, so a floating wire is not read as data.
    gpio_pullup_en(static_cast<gpio_num_t>(MESHSAT_IRIDIUM_RX_PIN));
    LOG_INFO("MeshSat Iridium: RockBLOCK on UART%d, TX GPIO%d, RX GPIO%d, %d 8N1", MESHSAT_IRIDIUM_UART_NUM,
             MESHSAT_IRIDIUM_TX_PIN, MESHSAT_IRIDIUM_RX_PIN, MESHSAT_IRIDIUM_BAUD);
}

void IridiumPipe::closeUart()
{
    modemUart.end();
    // An idle-high TX pin or a pull-up would feed current into an unpowered modem.
    pinMode(MESHSAT_IRIDIUM_TX_PIN, INPUT);
    gpio_pullup_dis(static_cast<gpio_num_t>(MESHSAT_IRIDIUM_RX_PIN));
}

void IridiumPipe::powerModem(bool on)
{
#ifdef MESHSAT_IRIDIUM_DCDC5_MV
    if (!PMU) {
        LOG_WARN("MeshSat Iridium: no PMU, modem supply unchanged");
        return;
    }
    if (on) {
        // DCDC5 follows the battery once it drops below the set voltage; its under-voltage power-off must not shut the node down.
        if (PMU->getChipModel() == XPOWERS_AXP2101)
            static_cast<XPowersAXP2101 *>(PMU)->disableDC5LowVoltageTurnOff();
        PMU->setPowerChannelVoltage(XPOWERS_DCDC5, MESHSAT_IRIDIUM_DCDC5_MV);
        PMU->enablePowerOutput(XPOWERS_DCDC5);
    } else {
        PMU->disablePowerOutput(XPOWERS_DCDC5);
    }
    LOG_INFO("MeshSat Iridium: modem supply %s (DCDC5, %d mV)", on ? "on" : "off", MESHSAT_IRIDIUM_DCDC5_MV);
#else
    (void)on;
#endif
}

int IridiumPipe::prepareDeepSleep(void *unused)
{
    (void)unused;
    closeUart();
    powerModem(false);
    return 0;
}

bool IridiumPipe::tryAcquireForNode()
{
    IridiumModemOwner expected = IridiumModemOwner::None;
    if (!currentOwner.compare_exchange_strong(expected, IridiumModemOwner::Node))
        return false;
    publishStatus();
    return true;
}

void IridiumPipe::releaseFromNode()
{
    IridiumModemOwner expected = IridiumModemOwner::Node;
    if (currentOwner.compare_exchange_strong(expected, IridiumModemOwner::None))
        publishStatus();
}

void IridiumPipe::setOwner(IridiumModemOwner owner)
{
    currentOwner.store(owner);
    publishStatus();
}

void IridiumPipe::publishStatus()
{
    if (!statusCharacteristic)
        return;
    const uint8_t status[2] = {CONTRACT_VERSION, static_cast<uint8_t>(currentOwner.load())};
    statusCharacteristic->setValue(status, sizeof(status));
    statusCharacteristic->notify();
}

void IridiumPipe::onPhoneWrite(const uint8_t *data, size_t length)
{
    if (!incoming || !data || length == 0)
        return;
    const size_t sent = xStreamBufferSend(incoming, data, length, 0);
    if (sent < length)
        phoneBytesDropped.fetch_add(static_cast<uint32_t>(length - sent));
}

void IridiumPipe::onPhoneSubscribe(uint16_t connHandle, bool subscribed)
{
    if (subscribed) {
        // While one link holds the modem, a CCCD write from another link changes nothing.
        if (phoneSubscribed.load() && phoneConnHandle.load() != connHandle) {
            foreignSubscribes++;
            return;
        }
        phoneConnHandle.store(connHandle);
        phoneSubscribed.store(true);
    } else if (phoneConnHandle.load() == connHandle) {
        phoneSubscribed.store(false);
        lastUnsubscribeMs.store(millis());
    }
}

void IridiumPipe::onLinkClosed(uint16_t connHandle)
{
    // Only the link that claimed the modem releases it. Before this, the claim was dropped only
    // when no link at all was left, so a phone killed without unsubscribing kept the modem for as
    // long as any other central stayed connected - and the BLE watchdog, which holds off while the
    // modem is owned, never fired (MESHSAT-1267, tested 20 Sep 2026).
    if (phoneSubscribed.load() && phoneConnHandle.load() == connHandle) {
        phoneSubscribed.store(false);
        lastUnsubscribeMs.store(millis());
    }
}

int32_t IridiumPipe::runOnce()
{
    updateOwner();
    reportDrops();

    // Bytes a phone writes without owning the modem are dropped, so nothing stale runs later.
    if (currentOwner.load() != IridiumModemOwner::Phone)
        discardPhoneBytes();

    bool busy = false;
    switch (currentOwner.load()) {
    case IridiumModemOwner::Phone:
        if (phoneSubscribed.load()) {
            busy |= pumpPhoneToModem();
            busy |= pumpModemToPhone();
        } else {
            // The phone is gone but its session is still in flight: read the result, forward nothing.
            busy |= drainModem();
        }
        break;
    case IridiumModemOwner::None:
        busy |= drainModem();
        runHealthCheck();
        break;
    case IridiumModemOwner::Node:
        break;
    }
    return busy ? BUSY_INTERVAL_MS : IDLE_INTERVAL_MS;
}

void IridiumPipe::updateOwner()
{
    // NimBLE may not report an unsubscribe when the link drops, so a lost connection counts as one.
    if (phoneSubscribed.load() && (!bleServer || bleServer->getConnectedCount() == 0)) {
        phoneSubscribed.store(false);
        lastUnsubscribeMs.store(millis());
    }

    const IridiumModemOwner owner = currentOwner.load();
    if (phoneSubscribed.load()) {
        if (owner == IridiumModemOwner::None) {
            if (healthAwaiting) {
                // The probe's reply must not land in the phone's first command.
                healthAwaiting = false;
                flushing = true;
                flushStartMs = millis();
            }
            holdLogged = false;
            setOwner(IridiumModemOwner::Phone);
            LOG_INFO("MeshSat Iridium: phone owns the modem");
        }
        return;
    }

    if (owner != IridiumModemOwner::Phone)
        return;

    if (stat.sessionInFlight && !Throttle::hasElapsed(stat.sessionStartMs, SESSION_CAP_MS)) {
        if (!holdLogged) {
            LOG_INFO("MeshSat Iridium: phone gone with a session in flight, holding the modem for its result");
            holdLogged = true;
        }
        return;
    }
    // A resubscribe from the same phone within the window keeps the claim, so a flapping CCCD
    // does not release and re-acquire the modem.
    if (!Throttle::hasElapsed(lastUnsubscribeMs.load(), RELEASE_DEBOUNCE_MS))
        return;

    if (stat.sessionInFlight) {
        LOG_WARN("MeshSat Iridium: session gave no result within %us, releasing anyway", (unsigned)(SESSION_CAP_MS / 1000));
        stat.sessionInFlight = false;
        sessionInFlightFlag.store(false);
    }
    pendingLength = 0;
    if (incoming)
        xStreamBufferReset(incoming);
    setOwner(IridiumModemOwner::None);
    LOG_INFO("MeshSat Iridium: phone released the modem");
}

void IridiumPipe::reportDrops()
{
    const uint32_t dropped = phoneBytesDropped.exchange(0);
    if (dropped > 0) {
        dropsSinceLog += dropped;
        stat.phoneBytesDropped += dropped;
    }
    if (dropsSinceLog > 0 && Throttle::hasElapsed(lastDropLogMs, DROP_LOG_INTERVAL_MS)) {
        LOG_WARN("MeshSat Iridium: %u bytes from the phone dropped, incoming buffer full (%u since boot)",
                 static_cast<unsigned>(dropsSinceLog), static_cast<unsigned>(stat.phoneBytesDropped));
        dropsSinceLog = 0;
        lastDropLogMs = millis();
    }
    const uint32_t foreign = foreignSubscribes.exchange(0);
    if (foreign > 0) {
        stat.foreignSubscribes += foreign;
        LOG_WARN("MeshSat Iridium: %u subscribe(s) from a link that does not own the modem, ignored",
                 static_cast<unsigned>(foreign));
    }
}

void IridiumPipe::discardPhoneBytes()
{
    if (!incoming)
        return;
    uint8_t scratch[COPY_CHUNK_BYTES];
    size_t discarded = 0;
    size_t count = 0;
    while ((count = xStreamBufferReceive(incoming, scratch, sizeof(scratch), 0)) > 0)
        discarded += count;
    if (discarded > 0)
        LOG_WARN("MeshSat Iridium: %u bytes written without owning the modem, discarded", static_cast<unsigned>(discarded));
}

bool IridiumPipe::pumpPhoneToModem()
{
    if (!incoming)
        return false;
    bool moved = false;
    uint8_t chunk[COPY_CHUNK_BYTES];
    while (true) {
        const int space = modemUart.availableForWrite();
        if (space <= 0)
            break;
        const size_t want = static_cast<size_t>(space) < sizeof(chunk) ? static_cast<size_t>(space) : sizeof(chunk);
        const size_t count = xStreamBufferReceive(incoming, chunk, want, 0);
        if (count == 0)
            break;
        notePhoneBytes(chunk, count);
        modemUart.write(chunk, count);
        moved = true;
    }
    return moved;
}

bool IridiumPipe::pumpModemToPhone()
{
    if (!txCharacteristic)
        return false;
    if (flushing) {
        if (!Throttle::hasElapsed(flushStartMs, FLUSH_MS))
            return drainModem();
        flushing = false;
    }
    bool moved = false;
    const size_t limit = notifyChunkLimit();
    for (int i = 0; i < MAX_NOTIFIES_PER_RUN; ++i) {
        if (pendingLength == 0) {
            while (pendingLength < limit && modemUart.available() > 0) {
                const int value = modemUart.read();
                if (value < 0)
                    break;
                noteModemByte(static_cast<uint8_t>(value));
                pendingChunk[pendingLength++] = static_cast<uint8_t>(value);
            }
            if (pendingLength == 0)
                break;
        }
        notifyFailed = false;
        txCharacteristic->setValue(pendingChunk, pendingLength);
        txCharacteristic->notify();
        if (notifyFailed)
            return true;
        pendingLength = 0;
        moved = true;
    }
    return moved;
}

bool IridiumPipe::drainModem()
{
    size_t count = 0;
    while (modemUart.available() > 0) {
        const int value = modemUart.read();
        if (value < 0)
            break;
        noteModemByte(static_cast<uint8_t>(value));
        ++count;
    }
    if (count == 0)
        return false;
    unownedBytes += static_cast<uint32_t>(count);
    LOG_DEBUG("MeshSat Iridium: %u modem bytes read with no phone attached (%u total)", static_cast<unsigned>(count),
              static_cast<unsigned>(unownedBytes));
    return true;
}

void IridiumPipe::runHealthCheck()
{
    const uint32_t now = millis();

    if (powerCycling) {
        if (!Throttle::hasElapsed(powerCycleOffMs, POWER_CYCLE_OFF_MS))
            return;
        powerCycling = false;
        openUart();
        powerModem(true);
        lastHealthMs = now;
        nextHealthDelayMs = HEALTH_FIRST_DELAY_MS;
        return;
    }

    if (healthAwaiting) {
        if (!Throttle::hasElapsed(healthSentMs, HEALTH_REPLY_MS))
            return;
        healthAwaiting = false;
        stat.healthMisses++;
        lastHealthMs = now;
        nextHealthDelayMs = HEALTH_REPLY_MS * 2;
        LOG_WARN("MeshSat Iridium: modem did not answer AT (%u of %u)", static_cast<unsigned>(stat.healthMisses),
                 static_cast<unsigned>(HEALTH_MISSES_TO_CYCLE));
        if (stat.healthMisses < HEALTH_MISSES_TO_CYCLE)
            return;
        stat.healthMisses = 0;
        stat.modemAnswered = false;
#ifdef MESHSAT_IRIDIUM_DCDC5_MV
        stat.healthPowerCycles++;
        LOG_ERROR("MeshSat Iridium: modem silent, power-cycling its supply (%u so far)",
                  static_cast<unsigned>(stat.healthPowerCycles));
        closeUart();
        powerModem(false);
        powerCycling = true;
        powerCycleOffMs = now;
#else
        LOG_ERROR("MeshSat Iridium: modem silent, and this board cannot switch its supply");
        nextHealthDelayMs = HEALTH_INTERVAL_MS;
#endif
        return;
    }

    if (!Throttle::hasElapsed(lastHealthMs, nextHealthDelayMs))
        return;
    // Free: no session is opened, so no credit.
    modemUart.write("AT\r");
    healthAwaiting = true;
    healthSentMs = now;
    lastHealthMs = now;
    nextHealthDelayMs = HEALTH_INTERVAL_MS;
}

void IridiumPipe::notePhoneBytes(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        const uint8_t value = data[i];
        if (value == '\r' || value == '\n') {
            if (commandLength > 0)
                onCommandLine();
            commandLength = 0;
            continue;
        }
        if (commandLength >= COMMAND_LINE_BYTES - 1) {
            // Binary payload (SBDWB), not a command line.
            commandLength = 0;
            continue;
        }
        commandLine[commandLength++] = static_cast<char>(toupper(value));
    }
}

void IridiumPipe::noteModemByte(uint8_t value)
{
    if (value == '\r' || value == '\n') {
        if (responseLength > 0)
            onResponseLine();
        responseLength = 0;
        return;
    }
    if (responseLength >= RESPONSE_LINE_BYTES - 1) {
        // Binary payload (SBDRB), not a response line.
        responseLength = 0;
        return;
    }
    responseLine[responseLength++] = static_cast<char>(value);
}

void IridiumPipe::onCommandLine()
{
    commandLine[commandLength] = '\0';
    // AT+SBDIX and AT+SBDIXA both open a billed session.
    if (strncmp(commandLine, "AT+SBDIX", 8) == 0) {
        const uint32_t now = millis();
        stat.sessions++;
        stat.sessionInFlight = true;
        stat.sessionStartMs = now;
        stat.ringPending = false;
        sessionInFlightFlag.store(true);
        LOG_INFO("MeshSat Iridium: session %u started (%s)", static_cast<unsigned>(stat.sessions),
                 currentOwner.load() == IridiumModemOwner::Phone ? "phone" : "node");
    }
}

void IridiumPipe::onResponseLine()
{
    responseLine[responseLength] = '\0';
    const uint32_t now = millis();

    if (strncmp(responseLine, "+SBDIX:", 7) == 0) {
        // +SBDIX: <MO status>, <MOMSN>, <MT status>, <MTMSN>, <MT length>, <MT queued>
        long fields[6] = {-1, 0, -1, 0, 0, 0};
        const char *cursor = responseLine + 7;
        for (int i = 0; i < 6; ++i) {
            char *end = nullptr;
            fields[i] = strtol(cursor, &end, 10);
            if (end == cursor)
                break;
            cursor = end;
            while (*cursor == ',' || *cursor == ' ')
                ++cursor;
        }
        stat.lastMoStatus = static_cast<int>(fields[0]);
        stat.lastMomsn = static_cast<uint32_t>(fields[1]);
        stat.lastMtStatus = static_cast<int>(fields[2]);
        stat.lastMtLength = static_cast<uint32_t>(fields[4]);
        stat.lastMtQueued = static_cast<uint32_t>(fields[5]);
        stat.lastSessionMs = now;
        stat.sessionInFlight = false;
        sessionInFlightFlag.store(false);
        stat.modemAnswered = true;
        stat.lastModemOkMs = now;
        LOG_INFO("MeshSat Iridium: session result MO %d MOMSN %u MT %d length %u queued %u", stat.lastMoStatus,
                 static_cast<unsigned>(stat.lastMomsn), stat.lastMtStatus, static_cast<unsigned>(stat.lastMtLength),
                 static_cast<unsigned>(stat.lastMtQueued));
        return;
    }

    if (strncmp(responseLine, "+CSQ:", 5) == 0) {
        stat.lastCsq = static_cast<int>(strtol(responseLine + 5, nullptr, 10));
        stat.lastCsqMs = now;
        stat.modemAnswered = true;
        stat.lastModemOkMs = now;
        return;
    }

    if (strstr(responseLine, "SBDRING") != nullptr) {
        stat.ringPending = true;
        stat.ringMs = now;
        LOG_INFO("MeshSat Iridium: ring alert, a message is waiting");
        return;
    }

    if (strcmp(responseLine, "OK") == 0) {
        if (!stat.modemAnswered)
            LOG_INFO("MeshSat Iridium: modem answers");
        stat.modemAnswered = true;
        stat.lastModemOkMs = now;
        if (healthAwaiting) {
            healthAwaiting = false;
            stat.healthMisses = 0;
        }
        return;
    }

    if (strcmp(responseLine, "ERROR") == 0 && stat.sessionInFlight) {
        stat.sessionInFlight = false;
        sessionInFlightFlag.store(false);
        stat.lastSessionMs = now;
        LOG_WARN("MeshSat Iridium: session %u ended with ERROR", static_cast<unsigned>(stat.sessions));
    }
}

size_t IridiumPipe::notifyChunkLimit() const
{
    if (!bleServer)
        return DEFAULT_NOTIFY_BYTES;
    const uint16_t mtu = bleServer->getPeerMTU(phoneConnHandle.load());
    size_t limit = mtu > ATT_HEADER_BYTES + DEFAULT_NOTIFY_BYTES ? mtu - ATT_HEADER_BYTES : DEFAULT_NOTIFY_BYTES;
    return limit > MAX_NOTIFY_BYTES ? MAX_NOTIFY_BYTES : limit;
}

#endif
