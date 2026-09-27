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
#include "meshsat/IridiumStatusModule.h"
#include "meshsat/MeshSatBranding.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace meshsat
{

static const uint8_t markBits[] PROGMEM = MESHSAT_MARK_DATA;

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

void drawBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    const uint32_t now = millis();
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    const int16_t markX = x + 2;
    const int16_t markY = y + 4;
    drawMarkWithOrbit(display, markX, markY, now);

    display->setFont(FONT_MEDIUM);
    const int16_t wordX = markX + MESHSAT_MARK_WIDTH + 3;
    const int16_t wordY = markY + (MESHSAT_MARK_HEIGHT - FONT_HEIGHT_MEDIUM) / 2;
    drawWeighted(display, wordX, wordY, "Mesh", 2);
    const int16_t satX = wordX + display->getStringWidth("Mesh") + 2;
    drawWeighted(display, satX, wordY, "Sat", 3);

    display->setFont(FONT_SMALL);
    const int16_t lineY = y + display->getHeight() - FONT_HEIGHT_SMALL;
    if (myRegion && myRegion->name)
        display->drawString(x + 2, lineY, myRegion->name);
    const char *version = xstr(APP_VERSION_SHORT);
    display->drawString(x + display->getWidth() - display->getStringWidth(version) - 2, lineY, version);

    if (screen)
        screen->forceDisplay();
}

// ---- home screen ----

static void drawSignalBars(OLEDDisplay *display, int16_t x, int16_t baseY, int bars)
{
    for (int i = 0; i < 5; ++i) {
        const int h = 3 + i * 2;
        const int bx = x + i * 4;
        if (i < bars)
            display->fillRect(bx, baseY - h, 3, h);
        else
            display->drawRect(bx, baseY - h, 3, h);
    }
}

void drawHomeScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    graphics::drawCommonHeader(display, x, y, "");
    y += BASEUI_BELOW_HEADER_MARGIN;

    const int *rows = graphics::getTextPositions(display);
    const int16_t left = x + BASEUI_BODY_LR_MARGIN;
    const int16_t right = x + display->getWidth() - BASEUI_BODY_LR_MARGIN;
    const int16_t line1 = rows[1] + y;
    const uint32_t now = millis();
    char text[32];
    char age[8];

    // Line 1: the node's short name in bold and its region on the left; bars and the modem's owner on the right.
    const char *shortName = owner.short_name[0] ? owner.short_name : "node";
    drawWeighted(display, left, line1, shortName, 2);
    snprintf(text, sizeof(text), "  %s", myRegion && myRegion->name ? myRegion->name : "");
    display->drawString(left + display->getStringWidth(shortName) + 1, line1, text);

    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe) {
        display->drawString(left, rows[2] + y, "no Iridium pipe");
        return;
    }
    const IridiumStats &st = pipe->stats();

    const char *ownerWord = "free";
    if (pipe->owner() == IridiumModemOwner::Phone)
        ownerWord = "phone";
    else if (pipe->owner() == IridiumModemOwner::Node)
        ownerWord = "node";
    const int16_t ownerW = display->getStringWidth(ownerWord);
    display->drawString(right - ownerW, line1, ownerWord);
    const bool csqFresh = st.lastCsq >= 0 && !Throttle::hasElapsed(st.lastCsqMs, 30 * 60 * 1000UL);
    drawSignalBars(display, right - ownerW - 24, line1 + FONT_HEIGHT_SMALL - 2, csqFresh ? (st.lastCsq > 5 ? 5 : st.lastCsq) : 0);

    // Line 2, the one big thing: the satellite state in a few words.
    if (!st.modemAnswered) {
        snprintf(text, sizeof(text), "Modem silent");
    } else if (st.sessionInFlight) {
        snprintf(text, sizeof(text), "Session...");
    } else if (st.ringPending) {
        snprintf(text, sizeof(text), "Msg waiting");
    } else if (st.lastSessionMs == 0) {
        snprintf(text, sizeof(text), "Ready");
    } else {
        formatAge(age, sizeof(age), now - st.lastSessionMs);
        if (st.lastMoStatus >= 0 && st.lastMoStatus <= 4) {
            snprintf(text, sizeof(text), "Sent %s ago", age);
        } else {
            // Capitalised status word, e.g. "No network".
            const char *word = IridiumStatusModule::moStatusWord(st.lastMoStatus);
            snprintf(text, sizeof(text), "%s", word);
            if (text[0] >= 'a' && text[0] <= 'z')
                text[0] = static_cast<char>(text[0] - 'a' + 'A');
        }
    }
    display->setFont(FONT_MEDIUM);
    const int16_t bigY = line1 + FONT_HEIGHT_SMALL + 4;
    if (display->getStringWidth(text) > display->getWidth() - 2 * BASEUI_BODY_LR_MARGIN)
        display->setFont(FONT_SMALL);
    display->drawString(left, bigY, text);
    display->setFont(FONT_SMALL);
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
            ownerWord = "phone";
        else if (pipe->owner() == IridiumModemOwner::Node)
            ownerWord = "node";
        const uint32_t now = millis();
        if (!st.modemAnswered) {
            snprintf(text, sizeof(text), "Sat: %s, modem silent", ownerWord);
        } else if (st.sessionInFlight) {
            snprintf(text, sizeof(text), "Sat: %s, session...", ownerWord);
        } else if (st.ringPending) {
            snprintf(text, sizeof(text), "Sat: %s, msg waiting", ownerWord);
        } else if (st.lastSessionMs == 0) {
            snprintf(text, sizeof(text), "Sat: %s, ready", ownerWord);
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
