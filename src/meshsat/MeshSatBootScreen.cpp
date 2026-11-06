#include "meshsat/MeshSatBootScreen.h"

#if MESHSAT_IRIDIUM && HAS_SCREEN

#include "MeshRadio.h"
#include "NodeDB.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TimeFormatters.h"
#include "graphics/draw/UIRenderer.h"
#include "graphics/images.h"
#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/IridiumPipe.h"
#if MESHSAT_IRIDIUM_JSPR
#include "meshsat/IridiumImtModule.h"
#endif
#include "meshsat/IridiumStatusModule.h"
#include "meshsat/MeshSatBranding.h"
#include "meshsat/SatBeacon.h"
#if defined(MESHSAT_CHARGE_GUARD) && defined(HAS_PMU)
#include "meshsat/ChargeGuardThread.h"
#endif

#include "BluetoothStatus.h"
#include "GPSStatus.h"
#include "PowerStatus.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace meshsat
{

static const uint8_t markBits[] PROGMEM = MESHSAT_MARK_DATA;
static const uint8_t mpwrdBits[] PROGMEM = MESHSAT_MPWRD_DATA;

// One orbit of the satellite dot every 4 s.
static constexpr uint32_t ORBIT_PERIOD_MS = 4000;
static constexpr int16_t ORBIT_DOT_RADIUS = 3;

static void formatAge(char *out, size_t size, uint32_t sinceMs)
{
    const uint32_t s = sinceMs / 1000;
    if (s < 60)
        snprintf(out, size, "%us", (unsigned)s);
    else if (s < 3600)
        snprintf(out, size, "%um", (unsigned)(s / 60));
    else if (s < 86400)
        snprintf(out, size, "%uh", (unsigned)(s / 3600));
    else
        snprintf(out, size, "%ud", (unsigned)(s / 86400));
}

// Bold by over-striking: weight 2 for bold, 3 for bolder.
static void drawWeighted(OLEDDisplay *display, int16_t x, int16_t y, const char *text, int weight)
{
    for (int i = 0; i < weight; ++i)
        display->drawString(x + i, y, text);
}

static void drawMarkWithOrbit(OLEDDisplay *display, int16_t markX, int16_t markY, uint32_t now)
{
    display->drawXbm(markX, markY, MESHSAT_MARK_WIDTH, MESHSAT_MARK_HEIGHT, markBits);
    const float angle = (float)(now % ORBIT_PERIOD_MS) / (float)ORBIT_PERIOD_MS * 6.2831853f;
    const int16_t dotX = markX + (int16_t)lroundf(MESHSAT_MARK_ORBIT_CX + MESHSAT_MARK_ORBIT_AX * cosf(angle));
    const int16_t dotY = markY + (int16_t)lroundf(MESHSAT_MARK_ORBIT_CY + MESHSAT_MARK_ORBIT_AY * sinf(angle));
    // A clear ring around the dot so it reads on top of the orbit's dots.
    display->setColor(BLACK);
    display->fillCircle(dotX, dotY, ORBIT_DOT_RADIUS + 1);
    display->setColor(WHITE);
    display->fillCircle(dotX, dotY, ORBIT_DOT_RADIUS);
}

void drawPoweredScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    display->setColor(WHITE);
    display->drawXbm(x + (display->getWidth() - MESHSAT_MPWRD_WIDTH) / 2, y + (display->getHeight() - MESHSAT_MPWRD_HEIGHT) / 2,
                     MESHSAT_MPWRD_WIDTH, MESHSAT_MPWRD_HEIGHT, mpwrdBits);
}

void drawBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    const uint32_t now = millis();
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    const int16_t markX = x;
    const int16_t markY = y + 4;
    drawMarkWithOrbit(display, markX, markY, now);

    // "Mesh" in dots like the orbit, "Sat" solid (owner's pick of 3 Nov 2026, MESHSAT-1776): Mesh is drawn bold, then
    // every other pixel of its box is cleared, so it reads as the second, softer tone.
    display->setFont(FONT_MEDIUM);
    const int16_t wordX = markX + MESHSAT_MARK_WIDTH + 3;
    const int16_t wordY = markY + (MESHSAT_MARK_HEIGHT - FONT_HEIGHT_MEDIUM) / 2;
    drawWeighted(display, wordX, wordY, "Mesh", 2);
    const int16_t meshW = display->getStringWidth("Mesh") + 1;
    display->setColor(BLACK);
    for (int16_t px = wordX; px <= wordX + meshW; ++px)
        for (int16_t py = wordY; py < wordY + FONT_HEIGHT_MEDIUM; ++py)
            if (((px + py) & 1) != 0)
                display->setPixel(px, py);
    display->setColor(WHITE);
    drawWeighted(display, wordX + display->getStringWidth("Mesh") + 3, wordY, "Sat", 2);

    display->setFont(FONT_SMALL);
    const int16_t lineY = y + display->getHeight() - FONT_HEIGHT_SMALL;
    if (myRegion && myRegion->name)
        display->drawString(x + 2, lineY, myRegion->name);
    // The full build string, with the commit, so a flash is recognisable on the screen.
    const char *version = xstr(APP_VERSION);
    display->drawString(x + display->getWidth() - display->getStringWidth(version) - 2, lineY, version);

    if (screen)
        screen->forceDisplay();
}

// ---- home screen: the annunciator (owner, 3 Nov 2026, MESHSAT-1776) ----

enum class Lamp : uint8_t { Off, On, Alert };

// How long the BCN lamp stays lit from the start of each LED blip: the LED's 80 ms is too short to see on the panel.
static constexpr uint32_t BCN_LIT_MS = 500;
static constexpr int16_t LAMP_H = 15;

// One lamp: lit (filled, dark words), off (outlined), or alert (striped, words on a dark field).
static void drawLamp(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, const char *label, const char *value, Lamp lamp)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setColor(WHITE);
    if (lamp == Lamp::Alert) {
        for (int16_t px = x + 1; px < x + w - 1; ++px)
            for (int16_t py = y + 1; py < y + LAMP_H - 1; ++py)
                if ((px + py) % 3 == 0)
                    display->setPixel(px, py);
        char words[16];
        if (value && value[0])
            snprintf(words, sizeof(words), "%s %s", label, value);
        else
            snprintf(words, sizeof(words), "%s", label);
        display->setColor(BLACK);
        display->fillRect(x + 1, y + 1, display->getStringWidth(words) + 4, LAMP_H - 2);
        display->setColor(WHITE);
        display->drawRect(x, y, w, LAMP_H);
        display->drawString(x + 3, y + 1, words);
        return;
    }
    if (lamp == Lamp::On) {
        display->fillRect(x, y, w, LAMP_H);
        display->setColor(BLACK);
    } else {
        display->drawRect(x, y, w, LAMP_H);
    }
    display->drawString(x + 3, y + 1, label);
    if (value && value[0]) {
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(x + w - 3, y + 1, value);
        display->setTextAlignment(TEXT_ALIGN_LEFT);
    }
    display->setColor(WHITE);
}

bool homeWantsFastFrames()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    return pipe && pipe->beaconOn();
}

void drawHomeScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    const uint32_t now = millis();
    char value[12];
    char line1[40];
    char line2[48];
    line2[0] = 0;
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setColor(WHITE);
    IridiumPipe *pipe = IridiumPipe::instance();

    // SAT: lit while the modem runs and answers, with its bars; striped on a modem fault; off otherwise (starting, or
    // the cell too low for it).
    Lamp sat = Lamp::Off;
    value[0] = 0;
    if (pipe) {
        const IridiumStats &st = pipe->stats();
        const IridiumModemPower power = pipe->modemPower();
        if (power == IridiumModemPower::Fault) {
            sat = Lamp::Alert;
        } else if (power == IridiumModemPower::Running && st.modemAnswered) {
            sat = Lamp::On;
            if (st.lastCsq >= 0 && !Throttle::hasElapsed(st.lastCsqMs, 30 * 60 * 1000UL))
                snprintf(value, sizeof(value), "%d", st.lastCsq > 5 ? 5 : st.lastCsq);
        }
    }
    drawLamp(display, x, y, 39, "SAT", value, sat);

    // MESH: lit while other nodes were heard in the last two hours, with how many. The most recent one goes on line 2.
    size_t others = 0;
    const meshtastic_NodeInfoLite *latest = nullptr;
    uint32_t latestSince = UINT32_MAX;
    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); ++i) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == nodeDB->getNodeNum() || n->last_heard == 0)
            continue;
        const uint32_t since = sinceLastSeen(n);
        if (since < 2 * 60 * 60) {
            ++others;
            if (since < latestSince) {
                latestSince = since;
                latest = n;
            }
        }
    }
    snprintf(value, sizeof(value), "%u", (unsigned)others);
    drawLamp(display, x + 40, y, 48, "MESH", value, others > 0 ? Lamp::On : Lamp::Off);

    // GPS: lit with a fix, with the satellites in view.
    const bool fix = gpsStatus && gpsStatus->getHasLock();
    value[0] = 0;
    if (fix)
        snprintf(value, sizeof(value), "%u", (unsigned)gpsStatus->getNumSatellites());
    drawLamp(display, x + 89, y, 39, "GPS", value, fix ? Lamp::On : Lamp::Off);

    // APP: lit while a phone is connected over Bluetooth.
    const bool app =
        bluetoothStatus && bluetoothStatus->getConnectionState() == meshtastic::BluetoothStatus::ConnectionState::CONNECTED;
    drawLamp(display, x, y + 16, 39, "APP", "", app ? Lamp::On : Lamp::Off);

    // BAT: lit with the charge; striped when low on the cell alone, or when the charge guard holds charging back.
    Lamp bat = Lamp::Off;
    value[0] = 0;
    const bool usb = powerStatus && powerStatus->getHasUSB();
    if (powerStatus && powerStatus->getHasBattery()) {
        const unsigned pct = powerStatus->getBatteryChargePercent();
        snprintf(value, sizeof(value), "%u", pct);
        bat = (pct <= 15 && !usb) ? Lamp::Alert : Lamp::On;
    }
    bool chargeHeld = false;
    bool chargeCold = false;
    float roomC = 0.0f;
#if defined(MESHSAT_CHARGE_GUARD) && defined(HAS_PMU)
    ChargeVerdict verdict = ChargeVerdict::NoReading;
    if (usb && chargeGuardState(verdict, roomC)) {
        chargeCold = verdict == ChargeVerdict::TooCold || verdict == ChargeVerdict::ColdHold;
        chargeHeld = chargeCold || verdict == ChargeVerdict::TooHot || verdict == ChargeVerdict::HotHold;
        if (chargeHeld)
            bat = Lamp::Alert;
    }
#endif
    drawLamp(display, x + 40, y + 16, 48, "BAT", value, bat);

    // BCN: blinks with the LED while the beacon is on, lit for BCN_LIT_MS from the start of each blip.
    Lamp bcn = Lamp::Off;
    const bool beaconOn = pipe && pipe->beaconOn();
    if (beaconOn) {
        IridiumStatusModule *status = IridiumStatusModule::instance();
        const uint32_t blip = status ? status->beaconBlipStartMs() : 0;
        if (blip != 0 && now - blip < BCN_LIT_MS)
            bcn = Lamp::On;
    }
    drawLamp(display, x + 89, y + 16, 39, "BCN", "", bcn);

    // Two lines: what matters now, in priority order.
    bool bar = false;
    uint32_t nextMs = 0;
    unsigned waiting = 0;
#if MESHSAT_IRIDIUM_JSPR
    if (pipe)
        waiting = pipe->stats().moInModem + (iridiumImtModule ? static_cast<unsigned>(iridiumImtModule->waitingCount()) : 0);
#endif
    value[0] = 0;
    if (pipe && pipe->modemPower() == IridiumModemPower::Fault) {
        snprintf(line1, sizeof(line1), "Satellite modem fault.");
    } else if (pipe && pipe->beaconDraining()) {
        snprintf(line1, sizeof(line1), "SAT BEACON OFF");
        snprintf(line2, sizeof(line2), "sending its last message");
    } else if (beaconOn) {
        snprintf(line1, sizeof(line1), "SAT BEACON ON");
        nextMs = pipe->beaconNextInMs();
        if (nextMs == 0) {
            snprintf(value, sizeof(value), "sending");
        } else {
            const uint32_t secs = (nextMs + 999) / 1000;
            snprintf(value, sizeof(value), "%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
        }
        bar = true;
    } else if (chargeHeld) {
        snprintf(line1, sizeof(line1), "Not charging: too %s.", chargeCold ? "cold" : "hot");
        if (chargeCold)
            snprintf(line2, sizeof(line2),
                     "%d \xC2\xB0"
                     "C here, starts at 10 \xC2\xB0"
                     "C",
                     (int)lroundf(roomC));
        else
            snprintf(line2, sizeof(line2),
                     "%d \xC2\xB0"
                     "C here, starts under 35 \xC2\xB0"
                     "C",
                     (int)lroundf(roomC));
    } else if (bat == Lamp::Alert) {
        snprintf(line1, sizeof(line1), "Battery low.");
        snprintf(line2, sizeof(line2), "Plug in USB-C.");
    } else if (waiting > 0) {
        snprintf(line1, sizeof(line1), "%u waiting for the satellite", waiting);
    } else {
        snprintf(line1, sizeof(line1), "All quiet.");
    }
    if (!line2[0] && !bar) {
        if (latest) {
            char age[8];
            formatAge(age, sizeof(age), latestSince > 4000000UL ? 4000000000UL : latestSince * 1000UL);
            char idName[12];
            snprintf(idName, sizeof(idName), "!%08x", (unsigned)latest->num);
            const bool named = nodeInfoLiteHasUser(latest);
            const char *shortName = (named && latest->short_name[0]) ? latest->short_name : idName;
            const char *name = (named && latest->long_name[0]) ? latest->long_name : shortName;
            snprintf(line2, sizeof(line2), "%s heard %s ago", name, age);
            if (display->getStringWidth(line2) > display->getWidth())
                snprintf(line2, sizeof(line2), "%s heard %s ago", shortName, age);
        } else {
            snprintf(line2, sizeof(line2), "No other node heard yet");
        }
    }

    const int16_t row1 = y + 35;
    const int16_t row2 = y + 48;
    display->setColor(WHITE);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(x, row1, line1);
    if (value[0]) {
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(x + display->getWidth() - 1, row1, value);
        display->setTextAlignment(TEXT_ALIGN_LEFT);
    }
    if (bar) {
        // The bar fills towards the next position.
        const uint32_t interval = BeaconTimes{}.intervalMs;
        const uint32_t left = nextMs > interval ? interval : nextMs;
        const int16_t inner = display->getWidth() - 4;
        display->drawRect(x, y + 52, display->getWidth(), 8);
        const int16_t fill = (int16_t)((uint64_t)inner * (interval - left) / interval);
        if (fill > 0)
            display->fillRect(x + 2, y + 54, fill, 4);
    } else if (line2[0]) {
        display->drawString(x, row2, line2);
    }
}

void drawHomeSatelliteRow(OLEDDisplay *display, int16_t x, int16_t y)
{
    char text[40];
    char age[8];
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe) {
        snprintf(text, sizeof(text), "Sat: no pipe");
    } else {
        const IridiumStats &st = pipe->stats();
        const char *ownerWord = "free";
        if (pipe->owner() == IridiumModemOwner::Phone)
            ownerWord = "app";
        else if (pipe->owner() == IridiumModemOwner::Node)
            ownerWord = "mesh";
        const uint32_t now = millis();
        const IridiumModemPower power = pipe->modemPower();
        const bool buttonInUse = pipe->panelButtonActive();
        // The switch is the beacon's only (owner, 6 Nov 2026): with it out the modem runs as on a unit without one.
        if (buttonInUse && pipe->beaconDraining()) {
            snprintf(text, sizeof(text), "Sat: beacon off, sending");
        } else if (power == IridiumModemPower::Inhibited) {
            snprintf(text, sizeof(text), "Sat: modem off, cell low");
        } else if (power == IridiumModemPower::Starting) {
            snprintf(text, sizeof(text), "Sat: modem starting");
        } else if (power == IridiumModemPower::Fault) {
            snprintf(text, sizeof(text), "Sat: modem fault");
        } else if (!st.modemAnswered) {
            snprintf(text, sizeof(text), "Sat: %s, modem silent", ownerWord);
        } else if (buttonInUse && pipe->beaconOn()) {
            const uint32_t next = pipe->beaconNextInMs();
            if (next == 0) {
                snprintf(text, sizeof(text), "Sat: beacon on, sending");
            } else {
                formatAge(age, sizeof(age), next);
                snprintf(text, sizeof(text), "Sat: beacon on, next %s", age);
            }
        } else if (st.sessionInFlight) {
            snprintf(text, sizeof(text), "Sat: %s, session...", ownerWord);
        } else if (st.ringPending) {
            snprintf(text, sizeof(text), "Sat: %s, msg waiting", ownerWord);
        } else if (st.lastSessionMs == 0) {
            snprintf(text, sizeof(text), pipe->owner() == IridiumModemOwner::None ? "Sat: %s, changing mode" : "Sat: %s, ready",
                     ownerWord);
        } else {
            formatAge(age, sizeof(age), now - st.lastSessionMs);
            if (st.lastMoStatus >= 0 && st.lastMoStatus <= 4)
                snprintf(text, sizeof(text), "Sat: %s, sent %s ago", ownerWord, age);
            else
                snprintf(text, sizeof(text), "Sat: %s, %s %s", ownerWord, IridiumStatusModule::moStatusWord(st.lastMoStatus),
                         age);
        }
    }
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const int16_t width = display->getStringWidth(text) + imgSatellite_width + 3;
    int16_t startX = x + (display->getWidth() - width) / 2;
    if (startX < x + 2)
        startX = x + 2;
    display->drawXbm(startX, y + (FONT_HEIGHT_SMALL - imgSatellite_height) / 2, imgSatellite_width, imgSatellite_height,
                     imgSatellite);
    display->drawString(startX + imgSatellite_width + 3, y, text);
}

// ---- message animation ----

static bool animationInbound = false;
static uint32_t animationStartMs = 0;

static void drawEnvelope(OLEDDisplay *display, int16_t x, int16_t y)
{
    // A 14x10 envelope: outline and the flap.
    display->setColor(BLACK);
    display->fillRect(x, y, 14, 10);
    display->setColor(WHITE);
    display->drawRect(x, y, 14, 10);
    display->drawLine(x, y, x + 6, y + 5);
    display->drawLine(x + 13, y, x + 7, y + 5);
}

static void drawNodeGlyph(OLEDDisplay *display, int16_t x, int16_t y)
{
    // A small node: a box with an antenna.
    display->drawRect(x, y + 6, 12, 8);
    display->drawLine(x + 6, y + 6, x + 6, y);
    display->drawLine(x + 3, y + 2, x + 6, y);
    display->drawLine(x + 9, y + 2, x + 6, y);
}

static void drawMessageAnimation(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    const uint32_t elapsed = millis() - animationStartMs;
    // 0.2 s still, 1.6 s travel, then still.
    const uint32_t travelStart = 200;
    const uint32_t travelMs = 1600;
    float t = 0.0f;
    if (elapsed > travelStart)
        t = (float)(elapsed - travelStart) / (float)travelMs;
    if (t > 1.0f)
        t = 1.0f;
    // Ease in and out.
    t = t * t * (3.0f - 2.0f * t);

    const int16_t satX = x + display->getWidth() - SATELLITE_IMAGE_WIDTH - 6;
    const int16_t satY = y + 4;
    const int16_t nodeX = x + 8;
    const int16_t nodeY = y + display->getHeight() - 30;

    display->setColor(WHITE);
    display->drawXbm(satX, satY, SATELLITE_IMAGE_WIDTH, SATELLITE_IMAGE_HEIGHT, SATELLITE_IMAGE);
    drawNodeGlyph(display, nodeX, nodeY);
    // The link, dotted.
    for (int i = 0; i <= 10; ++i) {
        const int16_t lx = nodeX + 14 + (satX - 4 - (nodeX + 14)) * i / 10;
        const int16_t ly = nodeY + 6 + (satY + SATELLITE_IMAGE_HEIGHT - (nodeY + 6)) * i / 10;
        display->setPixel(lx, ly);
    }

    const int16_t fromX = animationInbound ? satX - 8 : nodeX + 14;
    const int16_t fromY = animationInbound ? satY + SATELLITE_IMAGE_HEIGHT - 2 : nodeY;
    const int16_t toX = animationInbound ? nodeX + 14 : satX - 8;
    const int16_t toY = animationInbound ? nodeY : satY + SATELLITE_IMAGE_HEIGHT - 2;
    const int16_t envX = fromX + (int16_t)((toX - fromX) * t);
    const int16_t envY = fromY + (int16_t)((toY - fromY) * t);
    drawEnvelope(display, envX, envY);

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(x + display->getWidth() / 2, y + display->getHeight() - FONT_HEIGHT_SMALL - 1,
                        animationInbound ? "Satellite message received" : "Message sent by satellite");
    display->setTextAlignment(TEXT_ALIGN_LEFT);
}

void startMessageAnimation(bool inbound)
{
    if (!screen)
        return;
    animationInbound = inbound;
    animationStartMs = millis();
    screen->startAlert(drawMessageAnimation);
}

} // namespace meshsat

#endif
