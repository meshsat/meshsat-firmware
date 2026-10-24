#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM

namespace meshsat
{
// The owner's switches for the node itself, set from the phone over the Iridium service's CONTROL
// characteristic (MESHSAT-1671) and kept in NVS across reboots: the status LED and the screen.
// "Forced off" means the owner switched it off; the node's own logic runs as before underneath.
void loadNodeControl();
bool ledForcedOff();
bool screenForcedOff();
// Persist the switch and act on it: the screen goes dark at once, or comes back on.
void setLedForcedOff(bool off);
void setScreenForcedOff(bool off);
// A press on the unit's own button lifts a screen-off, so a node is never left dark without a phone.
void onUserButton();
} // namespace meshsat

#endif
