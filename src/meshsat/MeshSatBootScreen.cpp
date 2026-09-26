#include "meshsat/MeshSatBootScreen.h"

#if MESHSAT_IRIDIUM && HAS_SCREEN

#include "MeshRadio.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/images.h"
#include "main.h"
#include "meshsat/IridiumPipe.h"
#include "meshsat/IridiumStatusModule.h"
#include "meshsat/MeshSatBranding.h"

#include <cstdio>

namespace meshsat
{

static const uint8_t markBits[] PROGMEM = MESHSAT_MARK_DATA;

void drawBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // The mark on the left, the word beside it, both centred on the upper part of the screen.
    const int16_t markX = x + 2;
    const int16_t markY = y + 4;
    display->drawXbm(markX, markY, MESHSAT_MARK_WIDTH, MESHSAT_MARK_HEIGHT, markBits);

    display->setFont(FONT_MEDIUM);
    const int16_t wordX = markX + MESHSAT_MARK_WIDTH + 4;
    const int16_t wordY = markY + (MESHSAT_MARK_HEIGHT - FONT_HEIGHT_MEDIUM) / 2;
    display->drawString(wordX, wordY, "MeshSat");

    // Bottom line: region on the left, version on the right.
    display->setFont(FONT_SMALL);
    const int16_t lineY = y + display->getHeight() - FONT_HEIGHT_SMALL;
    if (myRegion && myRegion->name)
        display->drawString(x + 2, lineY, myRegion->name);
    const char *version = xstr(APP_VERSION_SHORT);
    display->drawString(x + display->getWidth() - display->getStringWidth(version) - 2, lineY, version);

    if (screen)
        screen->forceDisplay();
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
            const uint32_t s = (now - st.lastSessionMs) / 1000;
            if (s < 60)
                snprintf(age, sizeof(age), "%us", (unsigned)s);
            else if (s < 3600)
                snprintf(age, sizeof(age), "%um", (unsigned)(s / 60));
            else
                snprintf(age, sizeof(age), "%uh", (unsigned)(s / 3600));
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

} // namespace meshsat

#endif
