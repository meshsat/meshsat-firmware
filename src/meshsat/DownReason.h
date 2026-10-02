#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include <cstdint>

namespace meshsat
{

// Why the node's previous run ended. The values are the wire values of STATS byte 50.
enum class DownCause : uint8_t {
    Unknown = 0,
    // The chip started from cold: power came back after being gone, the reset button, or the
    // reset a flashing tool ends with. The ESP32-S3 reports all three alike.
    PowerOn = 1,
    // A reset over USB or JTAG while the node ran.
    ExternalReset = 2,
    // A restart the firmware asked for: a settings change, a reboot from an app.
    Restart = 3,
    BleWatchdog = 4,
    Crash = 5,
    // A hardware watchdog timer: the firmware hung.
    Watchdog = 6,
    Brownout = 7,
    // The battery read empty and the node shut itself off.
    LowBattery = 8,
    SwitchedOff = 9,
    Sleep = 10,
};

// Once at boot: the marker a planned stop left in NVS, else the chip's reset reason.
void loadDownReason();
// Before a planned stop, so the next boot knows. The battery reading is stored with it.
void noteDown(DownCause cause);
// Before deep sleep: low battery when the cell reads empty and no USB feeds the node, else switched off.
void noteDeepSleep();

DownCause lastDownCause();
// Battery at a planned stop in units of 100 mV, 0 when it was not recorded.
uint8_t lastDownBatteryDecivolts();
const char *downCauseText(DownCause cause);
// True for a crash, a watchdog timer or a brownout: stops that nobody planned.
bool downCauseIsFault(DownCause cause);

} // namespace meshsat

#endif
