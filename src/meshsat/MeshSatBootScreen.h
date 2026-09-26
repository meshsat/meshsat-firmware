#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM && HAS_SCREEN

#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

namespace meshsat
{
// The second half of the boot screen: the MeshSat mark on the left, the word MeshSat beside it,
// region and version along the bottom. Same signature as upstream's drawOEMBootScreen.
void drawBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);
} // namespace meshsat

#endif
