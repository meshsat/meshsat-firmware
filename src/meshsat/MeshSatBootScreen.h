#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM && HAS_SCREEN

#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

namespace meshsat
{
// Boot timing on the MeshSat node (owner, 3 Nov 2026, MESHSAT-1776): the M-PWRD badge 2 s, then the MeshSat logo 4 s
// (one full orbit of its satellite dot).
constexpr uint32_t BOOT_FIRST_LOGO_MS = 2000;
constexpr uint32_t BOOT_MESHSAT_MS = 4000;

// The first half of the boot screen: the M-PWRD badge, centred.
void drawPoweredScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

// True while the home frame needs more than the idle frame rate: the BCN lamp blinks with the beacon's LED.
bool homeWantsFastFrames();
constexpr uint32_t HOME_BLINK_FPS = 10;

// The second half of the boot screen: the MeshSat mark on the left with its satellite dot
// orbiting, "Mesh" in dots like the orbit and "Sat" solid beside it, region and version along the bottom.
// Same signature as upstream's drawOEMBootScreen.
void drawBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

// The home frame on the MeshSat node, in place of upstream's drawDeviceFocused: the annunciator (owner, 3 Nov 2026,
// MESHSAT-1776). Six lamps (SAT, MESH, GPS, APP, BAT, BCN): lit is working, dark is off, striped needs attention; BCN
// blinks with the LED while the beacon is on. Two lines under them say what matters now.
void drawHomeScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

// One centred line with the satellite glyph, who has the modem and the last result.
void drawHomeSatelliteRow(OLEDDisplay *display, int16_t x, int16_t y);

// A short full-screen animation: an envelope rising from the node to a satellite (sent), or
// a satellite handing an envelope down to the node (received). The caller ends it with
// screen->endAlert() after MESSAGE_ANIMATION_MS.
constexpr uint32_t MESSAGE_ANIMATION_MS = 2600;
void startMessageAnimation(bool inbound);
} // namespace meshsat

#endif
