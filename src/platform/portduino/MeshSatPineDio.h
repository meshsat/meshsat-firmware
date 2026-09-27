#pragma once
#ifdef MESHSAT_PINEDIO_BRIDGE

// MeshSat: the Pine64 LoRa back cover of the PinePhone and PinePhone Pro as the daemon's radio.
// An SX1262 sits behind an ATtiny84 that turns the phone's pogo-pin I2C into SPI, with no reset,
// BUSY or DIO1 line. `Lora: spidev: pinedio-i2c` selects it. The transport and the RadioLib HAL
// live in the meshsat-pinedio-bridge library (github.com/meshsat/meshsat-lora-backplate); this
// file is only where the daemon meets them, in the same places it meets the CH341 adapter.

#include "yaml-cpp/yaml.h"
#include <RadioLib.h>
#include <string>

namespace meshsat_pinedio
{
extern const char *const spidevName;

struct Settings {
    std::string device = "/dev/i2c-5"; // the pogo-pin bus; its number depends on the kernel
    int address = 0x28;
    int maxSpiFrame = 120;
    int pollMs = 20;
    // Preamble symbols. The back cover's radio runs on a plain crystal that drifts for the first
    // second of a transmission; with the usual 16 symbols a frame longer than about 80 bytes
    // arrives damaged. A receiver locks at the end of the preamble, so a long one lets the
    // crystal settle before the data begins. Measured on the bench: 96 is too short, 128 holds.
    int preamble = 160;
};
extern Settings settings;

/// True when the configuration names the back cover as the radio's bus.
bool selected();
/// Lora.I2CDevice, Lora.I2CAddress, Lora.BridgeFrame, Lora.BridgePollMs, Lora.Preamble.
void readYaml(const YAML::Node &lora);
void writeYaml(YAML::Emitter &out);
/// Opens the bus, lines the bridge up and hands the radio its four pins, none of them a real
/// line. Throws std::runtime_error when the back cover does not answer.
void start();
void stop();
RadioLibHal *hal();
bool inError();
} // namespace meshsat_pinedio

#endif // MESHSAT_PINEDIO_BRIDGE
