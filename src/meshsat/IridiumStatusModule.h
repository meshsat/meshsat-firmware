#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

#include "concurrency/OSThread.h"
#include "mesh/MeshModule.h"

#include <cstdint>

// The Satellite screen frame and its banners: what the Iridium pipe has seen pass, drawn for the
// node's own display. Reads IridiumPipe::stats() only; it never talks to the modem.
class IridiumStatusModule : public MeshModule, private concurrency::OSThread
{
  public:
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
    static void formatAge(char *out, size_t size, uint32_t sinceMs);

    uint32_t seenSessionMs = 0;
    uint32_t seenRingMs = 0;
    bool seenModemAnswered = false;
};

#endif
