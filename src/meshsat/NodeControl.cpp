#include "meshsat/NodeControl.h"

#if MESHSAT_IRIDIUM

#include <Preferences.h>
#include <atomic>

#if HAS_SCREEN
#include "graphics/Screen.h"
#include "main.h"
#endif

namespace meshsat
{
static constexpr const char *NVS_NAMESPACE = "meshsat";
static constexpr const char *NVS_LED_OFF = "ledOff";
static constexpr const char *NVS_SCREEN_OFF = "scrOff";

static std::atomic<bool> ledOff{false};
static std::atomic<bool> screenOff{false};

static void persist(const char *key, bool value)
{
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.putBool(key, value);
        prefs.end();
    }
}

void loadNodeControl()
{
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, true)) {
        ledOff.store(prefs.getBool(NVS_LED_OFF, false));
        screenOff.store(prefs.getBool(NVS_SCREEN_OFF, false));
        prefs.end();
    }
    if (ledOff.load() || screenOff.load())
        LOG_INFO("MeshSat: owner switches at boot: LED %s, screen %s", ledOff.load() ? "off" : "on",
                 screenOff.load() ? "off" : "on");
}

bool ledForcedOff()
{
    return ledOff.load();
}

bool screenForcedOff()
{
    return screenOff.load();
}

void setLedForcedOff(bool off)
{
    if (ledOff.exchange(off) == off)
        return;
    persist(NVS_LED_OFF, off);
    LOG_INFO("MeshSat: status LED %s by the owner", off ? "off" : "on");
}

void setScreenForcedOff(bool off)
{
    if (screenOff.exchange(off) == off)
        return;
    persist(NVS_SCREEN_OFF, off);
    LOG_INFO("MeshSat: screen %s by the owner", off ? "off" : "on");
#if HAS_SCREEN
    if (screen)
        screen->setOn(!off);
#endif
}

void onUserButton()
{
    if (!screenOff.load())
        return;
    // The flag is cleared before the press wakes the screen, so the same press turns it on.
    setScreenForcedOff(false);
    LOG_INFO("MeshSat: a press on the unit brought the screen back");
}
} // namespace meshsat

#endif
