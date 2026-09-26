#include "meshsat/MeshSatBootScreen.h"

#if MESHSAT_IRIDIUM && HAS_SCREEN

#include "MeshRadio.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "main.h"
#include "meshsat/MeshSatBranding.h"

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

} // namespace meshsat

#endif
