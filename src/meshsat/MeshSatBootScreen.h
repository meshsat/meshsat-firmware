#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM && HAS_SCREEN

#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

namespace meshsat
{
// Boot timing on the MeshSat node: the Meshtastic logo briefly, then the MeshSat screen.
constexpr uint32_t BOOT_FIRST_LOGO_MS = 2000;
constexpr uint32_t BOOT_MESHSAT_MS = 6000;

// The second half of the boot screen: the MeshSat mark on the left with its satellite dot
// orbiting, "Mesh" bold and "Sat" bolder beside it, region and version along the bottom.
// Same signature as upstream's drawOEMBootScreen.
void drawBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

// The home frame on the MeshSat node, in place of upstream's drawDeviceFocused: name and
// region, signal bars with the modem's owner and state, the last satellite event, the mesh.
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
