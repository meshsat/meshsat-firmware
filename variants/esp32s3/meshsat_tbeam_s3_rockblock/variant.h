#include "../tbeam-s3-core/variant.h"

// RockBLOCK 9603 on the long header, by its printed labels: TXD (GPIO43) to modem pin 6, RXD (GPIO44) from modem pin 1.
// UART1 belongs to the GPS, so the modem gets UART2.
#define MESHSAT_IRIDIUM_TX_PIN 43
#define MESHSAT_IRIDIUM_RX_PIN 44
#define MESHSAT_IRIDIUM_UART_NUM 2
#define MESHSAT_IRIDIUM_BAUD 19200

// Modem supply from the AXP2101 DCDC5 rail (header label DC5, 1.4-3.7 V, 1 A); the RockBLOCK takes 3.0-5.4 V.
#define MESHSAT_IRIDIUM_DCDC5_MV 3700
