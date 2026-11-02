#pragma once

#include "configuration.h"

#if MESHSAT_IRIDIUM && defined(MESHSAT_CHARGE_GUARD) && defined(HAS_PMU)

namespace meshsat
{
// The charge temperature guard (MESHSAT-1774, rule in ChargeGuard.h): a thread that reads the charger chip, decides,
// and switches charging at the AXP2101. Started once from IridiumPipe::begin().
void startChargeGuard();
// The RockBLOCK 9704's board temperature, as the node's GET hwInfo returned it.
void chargeGuardNoteModemC(float c);
} // namespace meshsat

#endif
