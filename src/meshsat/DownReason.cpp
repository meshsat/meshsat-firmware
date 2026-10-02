#include "meshsat/DownReason.h"

#if MESHSAT_IRIDIUM

#include "PowerStatus.h"

#include <Preferences.h>
#include <esp_system.h>

namespace meshsat
{

static constexpr const char *NVS_NAMESPACE = "meshsat";
static constexpr const char *NVS_CAUSE = "downWhy";
static constexpr const char *NVS_BATTERY = "downMv";

// A little above the 3100 mV at which the firmware shuts the node off, since the reading moves.
static constexpr uint16_t LOW_BATTERY_MV = 3200;

static DownCause lastCause = DownCause::Unknown;
static uint8_t lastBatteryDecivolts = 0;

static uint16_t batteryMillivolts()
{
    if (!powerStatus || !powerStatus->getHasBattery())
        return 0;
    const int mv = powerStatus->getBatteryVoltageMv();
    return mv > 0 && mv < 65535 ? static_cast<uint16_t>(mv) : 0;
}

static DownCause fromResetReason(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return DownCause::PowerOn;
    case ESP_RST_EXT:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
        return DownCause::ExternalReset;
    case ESP_RST_SW:
        return DownCause::Restart;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:
        return DownCause::Crash;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return DownCause::Watchdog;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
        return DownCause::Brownout;
    case ESP_RST_DEEPSLEEP:
        return DownCause::Sleep;
    default:
        return DownCause::Unknown;
    }
}

void loadDownReason()
{
    const esp_reset_reason_t reason = esp_reset_reason();
    DownCause marker = DownCause::Unknown;
    uint16_t batteryMv = 0;
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        marker = static_cast<DownCause>(prefs.getUChar(NVS_CAUSE, 0));
        batteryMv = prefs.getUShort(NVS_BATTERY, 0);
        // The marker speaks for one stop only.
        if (marker != DownCause::Unknown) {
            prefs.remove(NVS_CAUSE);
            prefs.remove(NVS_BATTERY);
        }
        prefs.end();
    }

    const DownCause seen = fromResetReason(reason);
    // A crash, a watchdog or a brownout is what really happened, whatever was planned before it.
    if (downCauseIsFault(seen) || marker == DownCause::Unknown)
        lastCause = seen;
    else if (seen == DownCause::Sleep && marker == DownCause::SwitchedOff)
        // A timed deep sleep that the node woke from by itself.
        lastCause = DownCause::Sleep;
    else
        lastCause = marker;
    if (marker != DownCause::Unknown)
        lastBatteryDecivolts = static_cast<uint8_t>(batteryMv / 100 > 255 ? 255 : batteryMv / 100);
    if (lastBatteryDecivolts > 0)
        LOG_INFO("MeshSat: last stop: %s (reset reason %d, battery %u.%u V)", downCauseText(lastCause), (int)reason,
                 (unsigned)(lastBatteryDecivolts / 10), (unsigned)(lastBatteryDecivolts % 10));
    else
        LOG_INFO("MeshSat: last stop: %s (reset reason %d)", downCauseText(lastCause), (int)reason);
}

void noteDown(DownCause cause)
{
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false))
        return;
    prefs.putUChar(NVS_CAUSE, static_cast<uint8_t>(cause));
    prefs.putUShort(NVS_BATTERY, batteryMillivolts());
    prefs.end();
}

void noteDeepSleep()
{
    const uint16_t mv = batteryMillivolts();
    const bool onBattery = powerStatus && !powerStatus->getHasUSB();
    noteDown(onBattery && mv > 0 && mv < LOW_BATTERY_MV ? DownCause::LowBattery : DownCause::SwitchedOff);
}

DownCause lastDownCause()
{
    return lastCause;
}

uint8_t lastDownBatteryDecivolts()
{
    return lastBatteryDecivolts;
}

const char *downCauseText(DownCause cause)
{
    switch (cause) {
    case DownCause::PowerOn:
        return "power loss";
    case DownCause::ExternalReset:
        return "reset button or USB";
    case DownCause::Restart:
        return "restart";
    case DownCause::BleWatchdog:
        return "Bluetooth watchdog";
    case DownCause::Crash:
        return "crash";
    case DownCause::Watchdog:
        return "watchdog timer";
    case DownCause::Brownout:
        return "brownout";
    case DownCause::LowBattery:
        return "low battery";
    case DownCause::SwitchedOff:
        return "switched off";
    case DownCause::Sleep:
        return "sleep";
    case DownCause::Unknown:
        break;
    }
    return "unknown";
}

bool downCauseIsFault(DownCause cause)
{
    return cause == DownCause::Crash || cause == DownCause::Watchdog || cause == DownCause::Brownout;
}

} // namespace meshsat

#endif
