#include "meshsat/IridiumPipe.h"

#if MESHSAT_IRIDIUM

#include "Power.h"
#include "mesh/Throttle.h"
#include "meshsat/BleWatchdog.h"
#include "meshsat/IridiumModule.h"
#include "sleep.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <services/gatt/ble_svc_gatt.h>

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
static BLECharacteristic *statsCharacteristic = nullptr;
static constexpr uint32_t STATS_NOTIFY_INTERVAL_MS = 2 * 1000UL;

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

class IridiumPipePassCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *characteristic) override
    {
        if (pipeInstance)
            pipeInstance->onPassWrite(characteristic->getData(), characteristic->getLength());
    }
};

static IridiumPipeRxCallbacks rxCallbacks;
static IridiumPipeTxCallbacks txCallbacks;
static IridiumPipePassCallbacks passCallbacks;

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

    // Service Changed bookkeeping: after a table change, announce for the next boots.
    Preferences prefs;
    if (prefs.begin("meshsat", false)) {
        uint32_t left = prefs.getUInt("gattLeft", 0);
        if (prefs.getUInt("gattVer", 0) != GATT_TABLE_VERSION) {
            prefs.putUInt("gattVer", GATT_TABLE_VERSION);
            left = SERVICE_CHANGED_BOOTS;
        } else if (left > 0) {
            left--;
        }
        prefs.putUInt("gattLeft", left);
        prefs.end();
        pipeInstance->announceServiceChanged = left > 0;
        if (left > 0)
            LOG_INFO("MeshSat Iridium: GATT table version %u, Service Changed announced on authenticated links for %u more boots",
                     (unsigned)GATT_TABLE_VERSION, (unsigned)left);
    }
}

void IridiumPipe::onAuthenticated(uint16_t connHandle)
{
    (void)connHandle;
    if (!announceServiceChanged)
        return;
    // Indicate the whole range: a bonded phone then drops its cached table and discovers again.
    ble_svc_gatt_changed(0x0001, 0xFFFF);
    LOG_INFO("MeshSat Iridium: Service Changed indicated to the new link");
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
    uint8_t status[STATUS_BYTES] = {CONTRACT_VERSION, owner};
    if (STATUS_BYTES >= 4) {
        status[2] = 0;
        status[3] = 0xFF;
    }
    statusCharacteristic->setValue(status, sizeof(status));
    // Contract v2, additive: STATS (read + notify) and PASS (write).
    statsCharacteristic = service->createCharacteristic(STATS_UUID, txProperties);
    uint8_t emptyStats[STATS_BYTES] = {0};
    emptyStats[0] = STATS_VERSION;
    statsCharacteristic->setValue(emptyStats, sizeof(emptyStats));
    BLECharacteristic *pass = service->createCharacteristic(PASS_UUID, rxProperties);
    pass->setCallbacks(&passCallbacks);
    if (pipeInstance)
        pipeInstance->lastStatsMs = 0;
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
    // A health probe still out would be counted as a miss on release; the node's own traffic answers it.
    healthAwaiting = false;
    publishStatus();
    return true;
}

void IridiumPipe::releaseFromNode()
{
    IridiumModemOwner expected = IridiumModemOwner::Node;
    if (currentOwner.compare_exchange_strong(expected, IridiumModemOwner::None))
        publishStatus();
}

size_t IridiumPipe::nodeWrite(const uint8_t *data, size_t length)
{
    if (currentOwner.load() != IridiumModemOwner::Node || !data || length == 0)
        return 0;
    notePhoneBytes(data, length);
    return modemUart.write(data, length);
}

int IridiumPipe::nodeAvailable()
{
    if (currentOwner.load() != IridiumModemOwner::Node)
        return 0;
    return modemUart.available();
}

int IridiumPipe::nodeRead()
{
    if (currentOwner.load() != IridiumModemOwner::Node)
        return -1;
    const int value = modemUart.read();
    if (value >= 0)
        noteModemByte(static_cast<uint8_t>(value));
    return value;
}

void IridiumPipe::setOwner(IridiumModemOwner owner)
{
    currentOwner.store(owner);
    publishStatus();
}

// STATUS, contract v2: [version][owner][flags][csq]. Notified when the owner or the flags change;
// a new signal reading updates the value without a notification (STATS carries it).
void IridiumPipe::publishStatus()
{
    if (!statusCharacteristic)
        return;
    uint8_t status[STATUS_BYTES] = {CONTRACT_VERSION, static_cast<uint8_t>(currentOwner.load())};
    if (STATUS_BYTES >= 4) {
        status[2] = statusFlags();
        status[3] = stat.lastCsq >= 0 ? static_cast<uint8_t>(stat.lastCsq > 5 ? 5 : stat.lastCsq) : 0xFF;
    }
    const bool notify = status[1] != lastStatus[1] || (STATUS_BYTES >= 4 && status[2] != lastStatus[2]);
    statusCharacteristic->setValue(status, sizeof(status));
    if (notify)
        statusCharacteristic->notify();
    memcpy(lastStatus, status, sizeof(status));
}

uint8_t IridiumPipe::statusFlags() const
{
    uint8_t flags = 0;
    if (stat.sessionInFlight)
        flags |= 0x01;
    if (stat.ringPending)
        flags |= 0x02;
    if (stat.modemAnswered)
        flags |= 0x04;
    if (incoming && xStreamBufferBytesAvailable(incoming) > INCOMING_BYTES * 3 / 4)
        flags |= 0x08;
    return flags;
}

static void put16(uint8_t *out, size_t &at, uint16_t value)
{
    out[at++] = static_cast<uint8_t>(value & 0xFF);
    out[at++] = static_cast<uint8_t>(value >> 8);
}

static void put32(uint8_t *out, size_t &at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out[at++] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

// STATS, contract v2: 52 bytes, little-endian, the layout in MESHSAT-1378.
void IridiumPipe::publishStats()
{
    if (!statsCharacteristic || !Throttle::hasElapsed(lastStatsMs, STATS_NOTIFY_INTERVAL_MS))
        return;
    const uint32_t now = millis();
    uint8_t out[STATS_BYTES] = {0};
    size_t at = 0;
    out[at++] = STATS_VERSION;
    out[at++] = static_cast<uint8_t>(currentOwner.load());
    out[at++] = statusFlags();
    out[at++] = stat.lastCsq >= 0 ? static_cast<uint8_t>(stat.lastCsq > 5 ? 5 : stat.lastCsq) : 0xFF;
    put32(out, at, stat.lastCsq >= 0 ? (now - stat.lastCsqMs) / 1000 : 0xFFFFFFFFu);
    put32(out, at, stat.sessions);
    put16(out, at, static_cast<uint16_t>(static_cast<int16_t>(stat.lastMoStatus)));
    put16(out, at, static_cast<uint16_t>(stat.lastMomsn));
    put16(out, at, static_cast<uint16_t>(static_cast<int16_t>(stat.lastMtStatus)));
    put16(out, at, static_cast<uint16_t>(stat.lastMtQueued));
    put32(out, at, stat.lastSessionMs != 0 ? (now - stat.lastSessionMs) / 1000 : 0xFFFFFFFFu);
    put32(out, at, now / 1000);
    put32(out, at, BleWatchdog::rebootCount());
    put32(out, at, stat.phoneBytesDropped);
    uint32_t nodeSessions = 0, nodeSent = 0, nodeReceived = 0;
    uint8_t dayUsed = 0, dayCap = 0;
    if (iridiumModule) {
        nodeSessions = iridiumModule->sessionsAsNodeCount();
        nodeSent = iridiumModule->sentAsNodeCount();
        nodeReceived = iridiumModule->receivedAsNodeCount();
        dayUsed = iridiumModule->daySessionsUsed();
        dayCap = iridiumModule->daySessionsCap();
    }
    put32(out, at, nodeSessions);
    put32(out, at, nodeSent);
    put32(out, at, nodeReceived);
    out[at++] = dayUsed;
    out[at++] = dayCap;
    // Two reserved bytes stay zero.

    // Ages move every second; compare everything but them so idle nodes stay quiet.
    uint8_t compareNew[STATS_BYTES];
    uint8_t compareOld[STATS_BYTES];
    memcpy(compareNew, out, STATS_BYTES);
    memcpy(compareOld, lastStats, STATS_BYTES);
    memset(compareNew + 4, 0, 4);
    memset(compareOld + 4, 0, 4);
    memset(compareNew + 20, 0, 4);
    memset(compareOld + 20, 0, 4);
    memset(compareNew + 24, 0, 4);
    memset(compareOld + 24, 0, 4);
    const bool changed = memcmp(compareNew, compareOld, STATS_BYTES) != 0;
    statsCharacteristic->setValue(out, STATS_BYTES);
    if (changed)
        statsCharacteristic->notify();
    memcpy(lastStats, out, STATS_BYTES);
    lastStatsMs = now;
}

// PASS, contract v2: [01][n][n x (u32 startEpochS, u16 durationS, u8 maxElevationDeg)], little-endian.
void IridiumPipe::onPassWrite(const uint8_t *data, size_t length)
{
    if (!data || length < 2 || data[0] != 1)
        return;
    size_t count = data[1];
    if (count > MAX_PASS_WINDOWS)
        count = MAX_PASS_WINDOWS;
    if (length < 2 + count * 7)
        return;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *p = data + 2 + i * 7;
        pendingPass[i].startEpochS = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                     (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        pendingPass[i].durationS = static_cast<uint16_t>(p[4] | (p[5] << 8));
        pendingPass[i].maxElevationDeg = p[6];
    }
    pendingPassCount = count;
    pendingPassReady.store(true);
}

void IridiumPipe::applyPendingPassList()
{
    if (!pendingPassReady.exchange(false))
        return;
    passCount = pendingPassCount;
    for (size_t i = 0; i < passCount; ++i)
        passWindows[i] = pendingPass[i];
    passListEverWritten = true;
    LOG_INFO("MeshSat Iridium: phone wrote %u pass window(s)", static_cast<unsigned>(passCount));
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
    applyPendingPassList();
    if (STATUS_BYTES >= 4 && statusFlags() != lastStatus[2])
        publishStatus();
    publishStats();

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
    stat.lastPhoneReleaseMs = millis();
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
        // A session that reached the gateway settles whether a message is still waiting; 32 never did.
        if (stat.lastMoStatus != 32) {
            const bool stillWaiting = stat.lastMtQueued > 0;
            if (stillWaiting && !stat.ringPending)
                stat.ringMs = now;
            stat.ringPending = stillWaiting;
        }
        sessionInFlightFlag.store(false);
        stat.modemAnswered = true;
        stat.lastModemOkMs = now;
        LOG_INFO("MeshSat Iridium: session result MO %d MOMSN %u MT %d length %u queued %u", stat.lastMoStatus,
                 static_cast<unsigned>(stat.lastMomsn), stat.lastMtStatus, static_cast<unsigned>(stat.lastMtLength),
                 static_cast<unsigned>(stat.lastMtQueued));
        return;
    }

    // +CSQ: (a fresh scan) and +CSQF: (the last known value) both carry 0-5.
    if (strncmp(responseLine, "+CSQ", 4) == 0) {
        const char *colon = strchr(responseLine, ':');
        stat.lastCsq = colon ? static_cast<int>(strtol(colon + 1, nullptr, 10)) : -1;
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

    // +SBDSX: <MO flag>, <MOMSN>, <MT flag>, <MTMSN>, <RA flag>, <msg waiting>: the free way to learn
    // that the gateway holds a message.
    if (strncmp(responseLine, "+SBDSX:", 7) == 0) {
        long fields[6] = {0, 0, 0, 0, 0, 0};
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
        if ((fields[2] == 1 || fields[4] == 1 || fields[5] > 0) && !stat.ringPending) {
            stat.ringPending = true;
            stat.ringMs = now;
        }
        stat.modemAnswered = true;
        stat.lastModemOkMs = now;
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
