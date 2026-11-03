#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM && defined(MESHSAT_CHARGE_GUARD) && defined(HAS_PMU)

#include "meshsat/ChargeGuard.h"

namespace meshsat
{
// For the screen (MESHSAT-1776): the guard's last verdict and its estimate of the room. False before the first one.
bool chargeGuardState(ChargeVerdict &verdict, float &roomC);
// The charge temperature guard (MESHSAT-1774, rule in ChargeGuard.h): a thread that reads the charger chip, decides,
// and switches charging at the AXP2101. Started once from IridiumPipe::begin().
void startChargeGuard();
// The RockBLOCK 9704's board temperature, as the node's GET hwInfo returned it.
void chargeGuardNoteModemC(float c);
} // namespace meshsat

#endif
