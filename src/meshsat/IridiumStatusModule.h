#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "concurrency/OSThread.h"
#include "mesh/MeshModule.h"

#include <cstddef>
#include <cstdint>

// With MESHSAT_LED_DARK the LED shows only "sent" and "message waiting".
#ifndef MESHSAT_LED_DARK
#define MESHSAT_LED_DARK 0
#endif

// The Satellite screen frame, its banners and the blue LED: what the Iridium pipe has seen pass,
// for the node's own display. Reads IridiumPipe::stats() only; it never talks to the modem.
//
// The LED (the PMU's charge LED on the T-Beam, single colour) shows state, never signal bars,
// and stays dark whenever a phone holds the modem: the app shows the diagnostics then.
//   idle, modem answers      one short blink every 10 s
//   session in flight        4 Hz
//   last session sent        three quick blinks once
//   last session failed      one long blink once, only when the last CSQ within 60 s was 1 or more
//   message waiting          double blink every 5 s until fetched
//   modem silent             1 Hz
class IridiumStatusModule : public MeshModule, private concurrency::OSThread
{
  public:
    struct BlinkStep {
        uint16_t onMs;
        uint16_t offMs;
    };

    IridiumStatusModule();

#if HAS_SCREEN
    bool wantUIFrame() override { return true; }
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

    // Words for an SBDIX MO status, for the frame, the banners and the log.
    static const char *moStatusWord(int status);

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override { return false; }
    int32_t runOnce() override;

  private:
    enum class Led : uint8_t { Off, On, Blink1Hz, Blink4Hz };

    static void formatAge(char *out, size_t size, uint32_t sinceMs);
    void noteEvents();
    int32_t driveLed();
    void setLed(Led mode);
    void startScript(const BlinkStep *steps, uint8_t count);
    bool runScript(uint32_t now);

    uint32_t seenSessionMs = 0;
    uint32_t seenRingMs = 0;
    bool seenModemAnswered = false;

    Led led = Led::Off;
    bool ledKnown = false;
    const BlinkStep *script = nullptr;
    uint8_t scriptCount = 0;
    uint8_t scriptStep = 0;
    bool scriptOn = false;
    uint32_t scriptStepMs = 0;
    uint32_t lastHeartbeatMs = 0;
    uint32_t lastWaitingBlinkMs = 0;
    bool pendingSentBlink = false;
    bool pendingFailBlink = false;
    bool animationRunning = false;
    uint32_t animationUntilMs = 0;
};

#endif
