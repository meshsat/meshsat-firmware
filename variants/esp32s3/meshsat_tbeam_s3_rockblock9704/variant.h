#include "../tbeam-s3-core/variant.h"
#include "meshsat/MeshSatBranding.h"

// RockBLOCK 9704 on the long header, by its printed labels: TXD (GPIO43) to 9704 pin 14 RXD, RXD (GPIO44) from pin 13 TXD.
// UART1 belongs to the GPS, so the modem gets UART2.
#define MESHSAT_IRIDIUM_TX_PIN 43
#define MESHSAT_IRIDIUM_RX_PIN 44
#define MESHSAT_IRIDIUM_UART_NUM 2
#define MESHSAT_IRIDIUM_BAUD 230400

// Modem supply from the AXP2101 DCDC5 rail (header label DC5) into 9704 pin 12 V_BATT (3.6-4.5 V).
#define MESHSAT_IRIDIUM_DCDC5_MV 3700
