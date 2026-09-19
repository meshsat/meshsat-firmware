<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/meshsat/meshsat/main/docs/images/mark-dark.png">
  <img src="https://raw.githubusercontent.com/meshsat/meshsat/main/docs/images/mark-light.png" alt="MeshSat" width="190">
</picture>

### MeshSat node firmware: Meshtastic, plus an Iridium satellite modem on the same Bluetooth link.

[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue)](LICENSE)
[![Based on Meshtastic 2.8.0](https://img.shields.io/badge/based%20on-Meshtastic%202.8.0-67EA94)](https://github.com/meshtastic/firmware/releases/tag/v2.8.0.47db0e3)
![ESP32-S3 + RockBLOCK 9603](https://img.shields.io/badge/hardware-ESP32--S3%20%2B%20RockBLOCK%209603-555)

[Docs](https://docs.meshsat.net/node/) ·
[The node](https://github.com/meshsat/meshsat-esp32) ·
[Iridium Bluetooth service](https://github.com/meshsat/meshsat-esp32/blob/main/docs/IRIDIUM-BLE.md) ·
[Build and flash](#build-and-flash) ·
[What is proven](#what-is-proven-and-what-is-not) ·
[meshsat.net](https://meshsat.net)

</div>

This is the firmware for the [MeshSat node](https://github.com/meshsat/meshsat-esp32): a Meshtastic LoRa radio with a RockBLOCK 9603 Iridium modem in the same small case. It is the Meshtastic firmware plus one thing: a second Bluetooth service that gives the phone a binary-safe serial line to the modem. The [MeshSat Android](https://github.com/meshsat/meshsat-android) app connects once and uses both, the mesh and the satellite, over that one link.

Everything Meshtastic does, this does the same way. The phone does the satellite work: it speaks the 9603's AT commands through the pipe exactly as it would over a cable. The service's contract, with its UUIDs, owner status and pairing rules, is in [IRIDIUM-BLE.md](https://github.com/meshsat/meshsat-esp32/blob/main/docs/IRIDIUM-BLE.md).

Based on Meshtastic® firmware. This project is not affiliated with or endorsed by Meshtastic LLC. For the upstream firmware, go to [meshtastic/firmware](https://github.com/meshtastic/firmware).

> **Status: pre-release.** This is a prototype under active development, not a finished product. It has never been deployed to a real user and has never been used in an actual emergency. See [What is proven, and what is not](#what-is-proven-and-what-is-not) before you rely on it for anything.

## What this fork adds

| Env                          | Board                                      | RockBLOCK connection                                                                                                 |
| ---------------------------- | ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| `meshsat-tbeam-s3-rockblock` | LILYGO T-Beam Supreme (node v1)            | header PM1: GPIO43/44 on UART2, power from the AXP2101 DCDC5 rail, switched on at boot                               |
| `meshsat-xiao-s3-rockblock`  | Seeed XIAO ESP32-S3 + Wio-SX1262 (node v0) | D6/D7, GPIO43/44 on UART1, external 5 V supply; the XIAO variant's GPS is compiled out because it uses the same pins |

The Iridium pipe is in `src/meshsat/`. It hooks into upstream code in two places, both behind `#if MESHSAT_IRIDIUM`: the Bluetooth service setup in `src/nimble/NimbleBluetooth.cpp` and module setup in `src/modules/Modules.cpp`. Everything else is upstream, unchanged.

## Build and flash

Build with PlatformIO Core **6.1.19**. With 6.2.0 the build stops at `ModuleNotFoundError: SCons.Tool.FortranCommon`, because the platform pins SCons 4.8.1 and 6.2.0 brings 4.11.1.

```sh
pip install "platformio==6.1.19"
pio run -e meshsat-tbeam-s3-rockblock
```

Flash over USB with esptool. On a board that ran something else, erase it first:

```sh
esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash \
  0x0      .pio/build/meshsat-tbeam-s3-rockblock/firmware-meshsat-tbeam-s3-rockblock-*.factory.bin \
  0x670000 .pio/build/meshsat-tbeam-s3-rockblock/littlefs-meshsat-tbeam-s3-rockblock-*.bin
```

Then set it up like any Meshtastic node: region, owner name and a fixed Bluetooth PIN. Updates go over USB the same way. Over-the-air updates are not supported by this fork.

## What is proven, and what is not

|                                                                        | State                                                                                       |
| ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Meshtastic and Iridium services on one Bluetooth connection (XIAO, v0) | Verified 19 Sep 2026 with MeshSat Android and a laptop client                               |
| Taking and releasing the modem                                         | Verified: STATUS `01 00`, `01 01`, `01 00`, and writes from a non-owner discarded           |
| Long modem replies over notifications                                  | A 300-byte reply arrives intact, and a binary loopback of up to 270 bytes returns identical |
| Satellite messages through the pipe                                    | One out and one in, 19 Sep 2026                                                             |
| T-Beam Supreme variant (v1)                                            | Builds. **Not run on hardware yet**                                                         |
| Routing on the node, owner `02`                                        | **Not built yet**                                                                           |
| Deployment to a real end user                                          | **Never**                                                                                   |
| Use in an actual emergency                                             | **Never**                                                                                   |

## Following upstream

`main` is the upstream tag `v2.8.0.47db0e3` with the MeshSat commits on top. Upstream releases come in by merge, never by rewriting history, so every MeshSat change stays a readable commit on top of a Meshtastic release. Upstream's contributor guides (`CLAUDE.md`, `AGENTS.md`, `.github/copilot-instructions.md`) are theirs and apply here unchanged. Issues with the Meshtastic firmware itself belong upstream.

## Related projects

- **[MeshSat node](https://github.com/meshsat/meshsat-esp32)**, the hardware, wiring, bench tools and the Iridium service contract
- **[MeshSat Android](https://github.com/meshsat/meshsat-android)**, the phone gateway that speaks to the Iridium service
- **[MeshSat](https://github.com/meshsat/meshsat)**, the Bridge: a Raspberry Pi gateway that bonds Meshtastic, Iridium, cellular SMS, APRS, ZigBee and TCP
- **[MeshSat Hub](https://hub.meshsat.net)**, multi-tenant fleet management

## License

[GNU General Public License v3.0](LICENSE), the same as upstream. The Meshtastic firmware is copyright its contributors; the MeshSat additions are copyright 2026 Elli and Kyriakos. Meshtastic® is a registered trademark of Meshtastic LLC.
