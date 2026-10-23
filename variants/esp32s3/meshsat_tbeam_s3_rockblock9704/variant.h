#include "../tbeam-s3-core/variant.h"
#include "meshsat/MeshSatBranding.h"

// RockBLOCK 9704 on the long header, by its printed labels: TXD (GPIO43) to 9704 pin 14 RXD, RXD (GPIO44) from pin 13 TXD.
// UART1 belongs to the GPS, so the modem gets UART2.
#define MESHSAT_IRIDIUM_TX_PIN 43
#define MESHSAT_IRIDIUM_RX_PIN 44
#define MESHSAT_IRIDIUM_UART_NUM 2
#define MESHSAT_IRIDIUM_BAUD 230400

// The modem's supply is the cell itself: the T-Beam's VBAT header pin into 9704 pin 12 V_BATT (owner ruling of
// 23 Oct 2026, MESHSAT-1507 option A, built on MESHSAT-1657). No DCDC5 define here: the DC5 rail is not wired to
// the modem any more, and the two must never be tied together (the AXP2101 switches itself off at 120 % of a
// DCDC setting). The firmware starts and stops the modem through its enable input, in Ground Control's order:
// I_EN (9704 pin 3, pulled up on the 9704, so the modem boots by itself when the cell goes in) from the pin
// printed IO21, I_BTD (9704 pin 7, high once booted, low once shut down) into the pin printed IO39.
// LILYGO's schematic numbers: PN1 pin 12 and PN1 pin 7.
#define MESHSAT_IRIDIUM_ENABLE_PIN 21
#define MESHSAT_IRIDIUM_BOOTED_PIN 39

// Cell thresholds for the modem (mV, hysteresis), near the 9704's own V_BATT floor of 3.6 V: below the off value
// the modem is shut down, above the on value it is started again. Proposed on MESHSAT-1657; the bench reading at
// the modem's pin 12 under the charge-up load (CE technical file test P1) still has to confirm them.
#define MESHSAT_IRIDIUM_CELL_ON_MV 3750
#define MESHSAT_IRIDIUM_CELL_OFF_MV 3650
