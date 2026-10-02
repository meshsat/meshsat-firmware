#include "meshsat/IridiumStatusModule.h"

#if MESHSAT_IRIDIUM

#include "main.h"
#include "mesh/Throttle.h"
#include "meshsat/DownReason.h"
#include "meshsat/IridiumPipe.h"

#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/images.h"
#include "meshsat/MeshSatBootScreen.h"
#endif

#if defined(HAS_PMU)
#include "Power.h"
#endif

#include <cstdio>

static constexpr int32_t POLL_INTERVAL_MS = 500;
static constexpr int32_t POLL_LED_MS = 50;
static constexpr uint32_t BANNER_MS = 4 * 1000UL;
// A CSQ older than this is shown as unknown on the frame and does not gate the fail blink.
static constexpr uint32_t CSQ_FRESH_MS = 30 * 60 * 1000UL;
static constexpr uint32_t CSQ_GATE_MS = 60 * 1000UL;
static constexpr uint32_t HEARTBEAT_EVERY_MS = 10 * 1000UL;
static constexpr uint32_t WAITING_EVERY_MS = 5 * 1000UL;
// No banners or animations this long after boot: the boot screen owns the display.
static constexpr uint32_t BOOT_QUIET_MS = 30 * 1000UL;

static const IridiumStatusModule::BlinkStep SENT_SCRIPT[] = {{100, 120}, {100, 120}, {100, 0}};
static const IridiumStatusModule::BlinkStep FAIL_SCRIPT[] = {{1000, 0}};
static const IridiumStatusModule::BlinkStep WAITING_SCRIPT[] = {{80, 120}, {80, 0}};
static const IridiumStatusModule::BlinkStep HEARTBEAT_SCRIPT[] = {{60, 0}};

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

// ---- events: banners and one-shot blinks ----

void IridiumStatusModule::noteEvents()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return;
    const IridiumStats &st = pipe->stats();

    // The node takes the modem while the boot screen is still up; its first status read and
    // session would pop a banner over the MeshSat logo. During the quiet window the events are
    // noted for the LED and the frame, nothing is shown.
    const bool quiet = !Throttle::hasElapsed(0, BOOT_QUIET_MS);

#if HAS_SCREEN
    // Once, when the boot screen is gone: say so if the last run did not end by a normal start or restart.
    if (!quiet && !lastStopShown) {
        lastStopShown = true;
        const meshsat::DownCause cause = meshsat::lastDownCause();
        if (screen && (meshsat::downCauseIsFault(cause) || cause == meshsat::DownCause::LowBattery ||
                       cause == meshsat::DownCause::BleWatchdog)) {
            char text[48];
            snprintf(text, sizeof(text), "Last stop: %s", meshsat::downCauseText(cause));
            screen->showSimpleBanner(text, 2 * BANNER_MS);
        }
    }
#endif

    if (st.lastSessionMs != seenSessionMs && st.lastSessionMs != 0) {
        seenSessionMs = st.lastSessionMs;
        const bool sent = st.lastMoStatus >= 0 && st.lastMoStatus <= 4;
        if (sent) {
            pendingSentBlink = true;
        } else {
            // A failure with bars is worth a blink; at 0 bars it is just a wall in the way.
            const bool bars = st.lastCsq >= 1 && !Throttle::hasElapsed(st.lastCsqMs, CSQ_GATE_MS);
            if (bars)
                pendingFailBlink = true;
        }
#if HAS_SCREEN
        if (screen && !quiet) {
            const bool received = st.lastMtStatus == 1 && st.lastMtLength > 0;
            if (received || sent) {
                // A message came in or went out: the envelope animation, ended from runOnce.
                meshsat::startMessageAnimation(received);
                animationUntilMs = millis() + meshsat::MESSAGE_ANIMATION_MS;
                animationRunning = true;
            } else {
                char text[48];
                if (st.lastMoStatus < 0)
                    snprintf(text, sizeof(text), "Satellite: session error");
                else
                    snprintf(text, sizeof(text), "Satellite: %s (%d)", moStatusWord(st.lastMoStatus), st.lastMoStatus);
                screen->showSimpleBanner(text, BANNER_MS);
            }
        }
#endif
    }

    if (st.ringPending && st.ringMs != seenRingMs) {
        seenRingMs = st.ringMs;
#if HAS_SCREEN
        if (screen && !quiet)
            screen->showSimpleBanner("Satellite: message waiting", BANNER_MS);
#endif
    }

    if (st.modemAnswered != seenModemAnswered) {
        seenModemAnswered = st.modemAnswered;
#if HAS_SCREEN
        if (screen && !quiet && !st.modemAnswered)
            screen->showSimpleBanner("Satellite: modem silent", BANNER_MS);
#endif
    }
}

// ---- the LED ----

void IridiumStatusModule::setLed(Led mode)
{
    if (ledKnown && mode == led)
        return;
    led = mode;
    ledKnown = true;
#if defined(HAS_PMU)
    if (!PMU)
        return;
    switch (mode) {
    case Led::Off:
        PMU->setChargingLedMode(XPOWERS_CHG_LED_OFF);
        break;
    case Led::On:
        PMU->setChargingLedMode(XPOWERS_CHG_LED_ON);
        break;
    case Led::Blink1Hz:
        PMU->setChargingLedMode(XPOWERS_CHG_LED_BLINK_1HZ);
        break;
    case Led::Blink4Hz:
        PMU->setChargingLedMode(XPOWERS_CHG_LED_BLINK_4HZ);
        break;
    }
#endif
}

void IridiumStatusModule::startScript(const BlinkStep *steps, uint8_t count)
{
    script = steps;
    scriptCount = count;
    scriptStep = 0;
    scriptOn = true;
    scriptStepMs = millis();
    setLed(Led::On);
}

// Advances the running blink script; true while it still runs.
bool IridiumStatusModule::runScript(uint32_t now)
{
    if (!script)
        return false;
    const BlinkStep &step = script[scriptStep];
    const uint32_t phaseMs = scriptOn ? step.onMs : step.offMs;
    if (!Throttle::hasElapsed(scriptStepMs, phaseMs))
        return true;
    if (scriptOn) {
        scriptOn = false;
        scriptStepMs = now;
        setLed(Led::Off);
        if (step.offMs > 0)
            return true;
    }
    scriptStep++;
    if (scriptStep >= scriptCount) {
        script = nullptr;
        return false;
    }
    scriptOn = true;
    scriptStepMs = now;
    setLed(Led::On);
    return true;
}

int32_t IridiumStatusModule::driveLed()
{
    IridiumPipe *pipe = IridiumPipe::instance();
    if (!pipe)
        return POLL_INTERVAL_MS;
    const IridiumStats &st = pipe->stats();
    const uint32_t now = millis();

    // With a phone on the modem the app shows the diagnostics; the LED keeps quiet.
    if (pipe->owner() == IridiumModemOwner::Phone) {
        script = nullptr;
        pendingSentBlink = false;
        pendingFailBlink = false;
        setLed(Led::Off);
        return POLL_INTERVAL_MS;
    }

    if (!st.modemAnswered && !MESHSAT_LED_DARK) {
        script = nullptr;
        setLed(Led::Blink1Hz);
        return POLL_INTERVAL_MS;
    }
    if (st.sessionInFlight && !MESHSAT_LED_DARK) {
        script = nullptr;
        setLed(Led::Blink4Hz);
        return POLL_INTERVAL_MS;
    }

    if (runScript(now))
        return POLL_LED_MS;

    if (pendingSentBlink) {
        pendingSentBlink = false;
        startScript(SENT_SCRIPT, sizeof(SENT_SCRIPT) / sizeof(SENT_SCRIPT[0]));
        return POLL_LED_MS;
    }
    if (pendingFailBlink && !MESHSAT_LED_DARK) {
        pendingFailBlink = false;
        startScript(FAIL_SCRIPT, sizeof(FAIL_SCRIPT) / sizeof(FAIL_SCRIPT[0]));
        return POLL_LED_MS;
    }
    pendingFailBlink = false;

    if (st.ringPending) {
        if (Throttle::hasElapsed(lastWaitingBlinkMs, WAITING_EVERY_MS)) {
            lastWaitingBlinkMs = now;
            startScript(WAITING_SCRIPT, sizeof(WAITING_SCRIPT) / sizeof(WAITING_SCRIPT[0]));
            return POLL_LED_MS;
        }
        setLed(Led::Off);
        return POLL_INTERVAL_MS;
    }

    if (!MESHSAT_LED_DARK && Throttle::hasElapsed(lastHeartbeatMs, HEARTBEAT_EVERY_MS)) {
        lastHeartbeatMs = now;
        startScript(HEARTBEAT_SCRIPT, sizeof(HEARTBEAT_SCRIPT) / sizeof(HEARTBEAT_SCRIPT[0]));
        return POLL_LED_MS;
    }
    setLed(Led::Off);
    return POLL_INTERVAL_MS;
}

int32_t IridiumStatusModule::runOnce()
{
    noteEvents();
#if HAS_SCREEN
    if (animationRunning && Throttle::deadlinePassed(animationUntilMs)) {
        animationRunning = false;
        if (screen)
            screen->endAlert();
    }
#endif
    const int32_t next = driveLed();
    return animationRunning && next > POLL_LED_MS ? POLL_LED_MS : next;
}

// ---- the frame ----

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
