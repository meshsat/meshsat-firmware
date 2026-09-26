#include "meshsat/IridiumStatusModule.h"

#if MESHSAT_IRIDIUM

#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/IridiumPipe.h"

#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/images.h"
#endif

#include <cstdio>

static constexpr uint32_t POLL_INTERVAL_MS = 500;
static constexpr uint32_t BANNER_MS = 4 * 1000UL;
// A CSQ older than this is shown as unknown.
static constexpr uint32_t CSQ_FRESH_MS = 30 * 60 * 1000UL;

IridiumStatusModule::IridiumStatusModule() : MeshModule("IridiumStatus"), concurrency::OSThread("IridiumStatus") {}

const char *IridiumStatusModule::moStatusWord(int status)
{
    if (status < 0)
        return "none yet";
    if (status <= 4)
        return "sent";
    switch (status) {
    case 32:
        return "no network";
    case 33:
        return "antenna fault";
    case 34:
        return "radio disabled";
    case 35:
        return "modem busy";
    case 36:
        return "try later";
    case 37:
        return "not registered";
    case 38:
        return "session busy";
    default:
        return status >= 10 && status <= 18 ? "gateway error" : "failed";
    }
}

void IridiumStatusModule::formatAge(char *out, size_t size, uint32_t sinceMs)
{
    const uint32_t s = sinceMs / 1000;
    if (s < 60)
        snprintf(out, size, "%us", (unsigned)s);
    else if (s < 3600)
        snprintf(out, size, "%um", (unsigned)(s / 60));
    else
        snprintf(out, size, "%uh", (unsigned)(s / 3600));
}

int32_t IridiumStatusModule::runOnce()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return POLL_INTERVAL_MS;
    const IridiumStats &st = pipe->stats();

    if (st.lastSessionMs != seenSessionMs && st.lastSessionMs != 0) {
        seenSessionMs = st.lastSessionMs;
#if HAS_SCREEN
        if (screen) {
            char text[48];
            if (st.lastMoStatus >= 0 && st.lastMoStatus <= 4 && st.lastMtLength > 0)
                snprintf(text, sizeof(text), "Satellite: sent, %u B in", (unsigned)st.lastMtLength);
            else if (st.lastMoStatus >= 0 && st.lastMoStatus <= 4)
                snprintf(text, sizeof(text), "Satellite: sent");
            else if (st.lastMoStatus < 0)
                snprintf(text, sizeof(text), "Satellite: session error");
            else
                snprintf(text, sizeof(text), "Satellite: %s (%d)", moStatusWord(st.lastMoStatus), st.lastMoStatus);
            screen->showSimpleBanner(text, BANNER_MS);
        }
#endif
    }

    if (st.ringPending && st.ringMs != seenRingMs) {
        seenRingMs = st.ringMs;
#if HAS_SCREEN
        if (screen)
            screen->showSimpleBanner("Satellite: message waiting", BANNER_MS);
#endif
    }

    if (st.modemAnswered != seenModemAnswered) {
        seenModemAnswered = st.modemAnswered;
#if HAS_SCREEN
        if (screen && !st.modemAnswered)
            screen->showSimpleBanner("Satellite: modem silent", BANNER_MS);
#endif
    }

    return POLL_INTERVAL_MS;
}

#if HAS_SCREEN
void IridiumStatusModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    graphics::drawCommonHeader(display, x, y, "Satellite");
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    IridiumPipe *pipe = IridiumPipe::instance();
    const int lineH = FONT_HEIGHT_SMALL + 1;
    // Rows start under the header line (y 20 on 128x64, 14 on smaller panels).
    const int top = y + (graphics::currentResolution == graphics::ScreenResolution::High ? 22 : 16);
    char line[40];
    char age[8];
    const uint32_t now = millis();

    if (!pipe) {
        display->drawString(x + 2, top, "no Iridium pipe");
        return;
    }
    const IridiumStats &st = pipe->stats();

    // Row 1: signal bars and the CSQ reading.
    const bool csqFresh = st.lastCsq >= 0 && !Throttle::hasElapsed(st.lastCsqMs, CSQ_FRESH_MS);
    const int bars = csqFresh ? (st.lastCsq > 5 ? 5 : st.lastCsq) : 0;
    const int barX = x + 3;
    const int barBase = top + FONT_HEIGHT_SMALL - 2;
    for (int i = 0; i < 5; ++i) {
        const int h = 3 + i * 2;
        const int bx = barX + i * 4;
        if (i < bars)
            display->fillRect(bx, barBase - h, 3, h);
        else
            display->drawRect(bx, barBase - h, 3, h);
    }
    if (csqFresh) {
        formatAge(age, sizeof(age), now - st.lastCsqMs);
        snprintf(line, sizeof(line), "signal %d/5, %s ago", st.lastCsq, age);
    } else {
        snprintf(line, sizeof(line), "signal not read yet");
    }
    display->drawString(barX + 24, top, line);

    // Row 2: who has the modem, and whether it answers.
    const char *ownerWord = "free";
    switch (pipe->owner()) {
    case IridiumModemOwner::Phone:
        ownerWord = "phone";
        break;
    case IridiumModemOwner::Node:
        ownerWord = "node";
        break;
    case IridiumModemOwner::None:
        break;
    }
    if (st.sessionInFlight)
        snprintf(line, sizeof(line), "modem: %s, session...", ownerWord);
    else
        snprintf(line, sizeof(line), "modem: %s, %s", ownerWord, st.modemAnswered ? "answers" : "silent");
    display->drawString(x + 3, top + lineH, line);

    // Row 3: the last session, or what is waiting.
    if (st.ringPending) {
        formatAge(age, sizeof(age), now - st.ringMs);
        snprintf(line, sizeof(line), "message waiting, %s", age);
    } else if (st.lastSessionMs == 0) {
        snprintf(line, sizeof(line), "sessions: %u, none done", (unsigned)st.sessions);
    } else {
        formatAge(age, sizeof(age), now - st.lastSessionMs);
        if (st.lastMoStatus >= 0 && st.lastMoStatus <= 4)
            snprintf(line, sizeof(line), "sent #%u, %s ago", (unsigned)st.lastMomsn, age);
        else
            snprintf(line, sizeof(line), "%s (%d), %s ago", moStatusWord(st.lastMoStatus), st.lastMoStatus, age);
    }
    display->drawString(x + 3, top + 2 * lineH, line);
}
#endif

#endif
