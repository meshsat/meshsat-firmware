#include "meshsat/IridiumPipe.h"

#if MESHSAT_IRIDIUM

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <HardwareSerial.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

// Holds a full AT+SBDWB payload (340 bytes + checksum) plus the command line.
static constexpr size_t INCOMING_BYTES = 1024;
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

static IridiumPipe *pipeInstance = nullptr;
static HardwareSerial modemUart(MESHSAT_IRIDIUM_UART_NUM);
static BLEServer *bleServer = nullptr;
static BLECharacteristic *txCharacteristic = nullptr;

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
};

static IridiumPipeRxCallbacks rxCallbacks;
static IridiumPipeTxCallbacks txCallbacks;

IridiumPipe::IridiumPipe() : concurrency::OSThread("IridiumPipe") {}

void IridiumPipe::begin()
{
    if (pipeInstance)
        return;
    incoming = xStreamBufferCreateStatic(INCOMING_BYTES, 1, incomingStorage, &incomingControl);
    pipeInstance = new IridiumPipe();
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

bool IridiumPipe::tryAcquireForNode()
{
    IridiumModemOwner expected = IridiumModemOwner::None;
    return currentOwner.compare_exchange_strong(expected, IridiumModemOwner::Node);
}

void IridiumPipe::releaseFromNode()
{
    IridiumModemOwner expected = IridiumModemOwner::Node;
    currentOwner.compare_exchange_strong(expected, IridiumModemOwner::None);
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
    phoneConnHandle.store(connHandle);
    phoneSubscribed.store(subscribed);
}

int32_t IridiumPipe::runOnce()
{
    updateOwner();

    const uint32_t dropped = phoneBytesDropped.exchange(0);
    if (dropped > 0)
        LOG_WARN("MeshSat Iridium: %u bytes from the phone dropped, incoming buffer full", static_cast<unsigned>(dropped));

    bool busy = false;
    switch (currentOwner.load()) {
    case IridiumModemOwner::Phone:
        busy |= pumpPhoneToModem();
        busy |= pumpModemToPhone();
        break;
    case IridiumModemOwner::None:
        busy |= drainUnowned();
        break;
    case IridiumModemOwner::Node:
        break;
    }
    return busy ? BUSY_INTERVAL_MS : IDLE_INTERVAL_MS;
}

void IridiumPipe::updateOwner()
{
    // NimBLE may not report an unsubscribe when the link drops, so a lost connection counts as one.
    if (phoneSubscribed.load() && (!bleServer || bleServer->getConnectedCount() == 0))
        phoneSubscribed.store(false);

    const IridiumModemOwner owner = currentOwner.load();
    if (phoneSubscribed.load()) {
        if (owner == IridiumModemOwner::None) {
            currentOwner.store(IridiumModemOwner::Phone);
            LOG_INFO("MeshSat Iridium: phone owns the modem");
        }
    } else if (owner == IridiumModemOwner::Phone) {
        currentOwner.store(IridiumModemOwner::None);
        if (incoming)
            xStreamBufferReset(incoming);
        LOG_INFO("MeshSat Iridium: phone released the modem");
    }
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
        modemUart.write(chunk, count);
        moved = true;
    }
    return moved;
}

bool IridiumPipe::pumpModemToPhone()
{
    if (!txCharacteristic)
        return false;
    bool moved = false;
    uint8_t chunk[MAX_NOTIFY_BYTES];
    const size_t limit = notifyChunkLimit();
    for (int i = 0; i < MAX_NOTIFIES_PER_RUN && modemUart.available() > 0; ++i) {
        size_t count = 0;
        while (count < limit && modemUart.available() > 0) {
            const int value = modemUart.read();
            if (value < 0)
                break;
            chunk[count++] = static_cast<uint8_t>(value);
        }
        if (count == 0)
            break;
        txCharacteristic->setValue(chunk, count);
        txCharacteristic->notify();
        moved = true;
    }
    return moved;
}

bool IridiumPipe::drainUnowned()
{
    size_t count = 0;
    while (modemUart.available() > 0 && modemUart.read() >= 0)
        ++count;
    if (count == 0)
        return false;
    unownedBytes += static_cast<uint32_t>(count);
    LOG_DEBUG("MeshSat Iridium: %u modem bytes with no owner discarded (%u total)", static_cast<unsigned>(count),
              static_cast<unsigned>(unownedBytes));
    return true;
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
