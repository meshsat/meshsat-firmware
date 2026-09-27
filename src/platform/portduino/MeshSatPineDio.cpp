#ifdef MESHSAT_PINEDIO_BRIDGE

#include "MeshSatPineDio.h"
#include "PortduinoGlue.h"
#include <LinuxI2cPort.h>
#include <PineDioBridgeHal.h>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace meshsat_pinedio
{
using meshsat::pinedio::Bridge;

const char *const spidevName = "pinedio-i2c";
Settings settings;

namespace
{
meshsat::pinedio::SystemClock systemClock;
std::unique_ptr<meshsat::pinedio::LinuxI2cPort> port;
std::unique_ptr<Bridge> bridge;
std::unique_ptr<meshsat::pinedio::PineDioBridgeHal> bridgeHal;

void claim(pinMapping &mapping, int pin)
{
    mapping.pin = pin;
    mapping.enabled = true;
}
} // namespace

bool selected()
{
    return portduino_config.lora_spi_dev == spidevName;
}

void readYaml(const YAML::Node &lora)
{
    settings.device = lora["I2CDevice"].as<std::string>(settings.device);
    settings.address = lora["I2CAddress"].as<int>(settings.address);
    settings.maxSpiFrame = lora["BridgeFrame"].as<int>(settings.maxSpiFrame);
    settings.pollMs = lora["BridgePollMs"].as<int>(settings.pollMs);
    settings.preamble = lora["Preamble"].as<int>(settings.preamble);
}

void writeYaml(YAML::Emitter &out)
{
    const Settings defaults;
    out << YAML::Key << "I2CDevice" << YAML::Value << settings.device;
    if (settings.address != defaults.address)
        out << YAML::Key << "I2CAddress" << YAML::Value << YAML::Hex << settings.address;
    if (settings.maxSpiFrame != defaults.maxSpiFrame)
        out << YAML::Key << "BridgeFrame" << YAML::Value << YAML::Dec << settings.maxSpiFrame;
    if (settings.pollMs != defaults.pollMs)
        out << YAML::Key << "BridgePollMs" << YAML::Value << YAML::Dec << settings.pollMs;
    if (settings.preamble != defaults.preamble)
        out << YAML::Key << "Preamble" << YAML::Value << YAML::Dec << settings.preamble;
}

void stop()
{
    // The HAL goes first: it stops the polling thread that uses the other two.
    if (bridgeHal)
        bridgeHal->term();
    bridgeHal.reset();
    bridge.reset();
    port.reset();
}

void start()
{
    stop();
    port = std::make_unique<meshsat::pinedio::LinuxI2cPort>();
    if (!port->open(settings.device, settings.address)) {
        const std::string why = port->lastError();
        port.reset();
        throw std::runtime_error("LoRa back cover: " + why);
    }

    meshsat::pinedio::Config config;
    if (settings.maxSpiFrame > 0)
        config.maxSpiFrame = (size_t)settings.maxSpiFrame;
    if (settings.pollMs > 0)
        config.pollIntervalUs = (uint32_t)settings.pollMs * 1000;
    bridge = std::make_unique<Bridge>(*port, systemClock, config);
    if (!bridge->begin()) {
        const std::string why = port->lastError();
        stop();
        throw std::runtime_error("LoRa back cover does not answer on " + settings.device + (why.empty() ? "" : " (" + why + ")"));
    }
    bridgeHal = std::make_unique<meshsat::pinedio::PineDioBridgeHal>(*bridge, systemClock);

    claim(portduino_config.lora_cs_pin, Bridge::PinCs);
    claim(portduino_config.lora_irq_pin, Bridge::PinIrq);
    claim(portduino_config.lora_busy_pin, Bridge::PinBusy);
    claim(portduino_config.lora_reset_pin, Bridge::PinReset);
    std::cout << "LoRa back cover on " << settings.device << " at 0x" << std::hex << settings.address << std::dec
              << ", bridge lined up after " << bridge->stats().syncReads << " reads" << std::endl;
}

RadioLibHal *hal()
{
    return bridgeHal.get();
}

bool inError()
{
    return bridge && bridge->inError();
}
} // namespace meshsat_pinedio

#endif // MESHSAT_PINEDIO_BRIDGE
